// bench_latency_instrumented.cpp — 插桩延迟基准（分段剖析异步入队路径）
//
// 本文件由 bench_latency_instrumented.cpp 与 bench_latency_breakdown.cpp 合并而来：
//   - 保留 instrumented 的 A/B/C 三段式结构，含真实 logger->info() 宏路径
//   - 并入 breakdown 独有的参考/旧路径环节（level()、atomic_load(shared_ptr)、
//     system_clock::now、std::format、thread_local vector::data 等）
//   - 样本量取 breakdown 的大样本（200K 预热 + 2M 测量）
//
// 目标：定位前台线程入队延迟（P50 ~18ns）中每一环节的开销占比。
//
// 编译:
//   cl /std:c++20 /O2 /EHsc /I include bench\bench_latency_instrumented.cpp
//   g++ -std=c++20 -O2 -I include bench/bench_latency_instrumented.cpp -lpthread
//
// 运行: build_release\Release\bench_latency_instrumented.exe
// 提示: 若想快速迭代可把 WARMUP/MEASURE 调小（如 5000 / 50000）。

#include "log/log.hpp"

#ifdef _WIN32
#include <intrin.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>
#include <source_location>
#include <string>
#include <thread>
#include <vector>

// ── rdtsc 封装 ──────────────────────────────────────────────────────
inline uint64_t rdtsc() noexcept {
#ifdef _WIN32
    return __rdtsc();
#else
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#endif
}

// ── 校准 rdtsc 频率 ──────────────────────────────────────────────────
uint64_t calibrate_rdtsc_freq() {
    auto t1 = std::chrono::steady_clock::now();
    uint64_t c1 = rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    uint64_t c2 = rdtsc();
    auto t2 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t2 - t1).count();
    return static_cast<uint64_t>((c2 - c1) / sec);
}

// ── 统计结果 ────────────────────────────────────────────────────────
struct Stats { double p50; double p99; };

Stats compute_stats(std::vector<uint64_t>& v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return {
        static_cast<double>(v[n / 2]),
        static_cast<double>(v[static_cast<size_t>(n * 0.99)])
    };
}

// ── 打印一行结果（指定格式）─────────────────────────────────────────
void print_row(int num, const char* name, const Stats& s, double ns_per_cycle) {
    std::printf("%2d. %-30s P50=%8.0f cyc (%8.1f ns)  P99=%8.0f cyc (%8.1f ns)\n",
        num, name,
        s.p50, s.p50 * ns_per_cycle,
        s.p99, s.p99 * ns_per_cycle);
    std::fflush(stdout);
}

// ── 核心测量模板（预热 + 采样）─────────────────────────────────────
template<typename Fn>
std::vector<uint64_t> measure(int warmup, int measure_n, Fn&& fn) {
    for (int i = 0; i < warmup; ++i) { fn(i); }
    std::vector<uint64_t> samples;
    samples.reserve(static_cast<size_t>(measure_n));
    for (int i = 0; i < measure_n; ++i) {
        uint64_t c1 = rdtsc();
        fn(i);
        uint64_t c2 = rdtsc();
        samples.push_back(c2 - c1);
    }
    return samples;
}

// ── 主函数 ───────────────────────────────────────────────────────────
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Windows 下固定到核心 0 减少 rdtsc 波动
#ifdef _WIN32
    HANDLE hThread = ::GetCurrentThread();
    DWORD_PTR prev_mask = ::SetThreadAffinityMask(hThread, 1);
#endif

    uint64_t freq = calibrate_rdtsc_freq();
    double ns_per_cycle = 1e9 / static_cast<double>(freq);
    std::printf("=== Latency Instrumented Benchmark (merged breakdown) ===\n");
    std::printf("CPU: ~%.2f GHz (%.3f ns/cycle)\n\n", freq / 1e9, ns_per_cycle);

    constexpr int WARMUP  = 200'000;
    constexpr int MEASURE = 2'000'000;

    // ── 创建 async sink + logger ──────────────────────────────────
    std::remove("__inst.log");
    auto async_sink = cpp109::make_async_sink<cpp109::FileSink>("__inst.log", true);
    auto logger     = cpp109::get_logger("inst");
    logger->clear_sinks();
    logger->set_level(cpp109::LogLevel::TRACE);
    logger->add_sink(async_sink);

    std::vector<uint64_t> samples;
    samples.reserve(MEASURE);

    // ═══════════════════════════════════════════════════════════════════
    // A. 基础开销
    // ═══════════════════════════════════════════════════════════════════

    // ── 1. rdtsc 本身开销 ──────────────────────────────────────────
    {
        for (int i = 0; i < WARMUP; ++i) { volatile auto c = rdtsc(); (void)c; }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(1, "rdtsc overhead", s, ns_per_cycle);
    }

    // ── 2. source_location::current() 开销 ────────────────────────
    {
        for (int i = 0; i < WARMUP; ++i) { volatile auto loc = std::source_location::current(); (void)loc; }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            volatile auto loc = std::source_location::current();
            (void)loc;
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(2, "source_location::current()", s, ns_per_cycle);
    }

    // ── 3. thread_local 访问开销 ──────────────────────────────────
    {
        static thread_local uint64_t tl_val = 42;
        for (int i = 0; i < WARMUP; ++i) { volatile auto v = tl_val; (void)v; }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            volatile auto v = tl_val;
            (void)v;
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(3, "thread_local access", s, ns_per_cycle);
    }

    // ── 4. atomic<bool> load(acquire) 开销 ────────────────────────
    {
        std::atomic<bool> flag{false};
        for (int i = 0; i < WARMUP; ++i) { volatile auto v = flag.load(std::memory_order_acquire); (void)v; }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            volatile auto v = flag.load(std::memory_order_acquire);
            (void)v;
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(4, "atomic<bool> load(acquire)", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════════════
    // B. 前台 Logger / 快路径各环节
    // ═══════════════════════════════════════════════════════════════════

    // ── 5. logger->level() atomic acquire（level check）───────────
    {
        for (int i = 0; i < WARMUP; ++i) { volatile auto lvl = logger->level(); (void)lvl; }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            volatile auto lvl = logger->level();
            (void)lvl;
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(5, "logger->level() atomic acq", s, ns_per_cycle);
    }

    // ── 6. atomic_load(shared_ptr, acquire) — 模拟旧 fast_sink_ ──
    {
        std::atomic<std::shared_ptr<int>> sp{std::make_shared<int>(42)};
        auto load_sp = [&]() {
            volatile auto p = sp.load(std::memory_order_acquire);
            (void)p;
        };
        for (int i = 0; i < WARMUP; ++i) { load_sp(); }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            load_sp();
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(6, "atomic_load(shared_ptr, acq)", s, ns_per_cycle);
    }

    // ── 7. dynamic_cast<AsyncSinkBase*> 开销 ──────────────────────
    {
        const char* dc_path = "__dc.log";
        std::remove(dc_path);
        auto dc_sink = cpp109::make_async_sink<cpp109::FileSink>(dc_path, true);
        cpp109::Sink* base = dc_sink.get();

        for (int i = 0; i < WARMUP; ++i) {
            volatile auto p = dynamic_cast<cpp109::AsyncSinkBase*>(base);
            (void)p;
        }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            volatile auto p = dynamic_cast<cpp109::AsyncSinkBase*>(base);
            (void)p;
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(7, "dynamic_cast<AsyncSinkBase*>", s, ns_per_cycle);

        dc_sink->stop();
        std::remove(dc_path);
    }

    // ── 8. compute_encoded_size(int) 开销 ─────────────────────────
    {
        for (int i = 0; i < WARMUP; ++i) {
            volatile auto sz = cpp109::detail::compute_encoded_size(42);
            (void)sz;
        }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            volatile auto sz = cpp109::detail::compute_encoded_size(i);
            (void)sz;
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(8, "compute_encoded_size(int)", s, ns_per_cycle);
    }

    // ── 9. get_encode_buffer(4) 开销 ──────────────────────────────
    {
        for (int i = 0; i < WARMUP; ++i) {
            volatile auto p = cpp109::detail::get_encode_buffer(4);
            (void)p;
        }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            volatile auto p = cpp109::detail::get_encode_buffer(4);
            (void)p;
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(9, "get_encode_buffer(4)", s, ns_per_cycle);
    }

    // ── 10. encode_args(int) 开销 ─────────────────────────────────
    {
        // 预先分配好缓冲区，只测量编码本身
        std::byte* buf = cpp109::detail::get_encode_buffer(64);
        for (int i = 0; i < WARMUP; ++i) {
            std::byte* tmp = buf;
            cpp109::detail::encode_args(tmp, 42);
        }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            std::byte* tmp = buf;
            uint64_t c1 = rdtsc();
            cpp109::detail::encode_args(tmp, i);
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(10, "encode_args(int)", s, ns_per_cycle);
    }

    // ── 11. spinlock test_and_set + clear (无竞争) ────────────────
    {
        std::atomic_flag lock = ATOMIC_FLAG_INIT;
        for (int i = 0; i < WARMUP; ++i) {
            lock.test_and_set(std::memory_order_acquire);
            lock.clear(std::memory_order_release);
        }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            lock.test_and_set(std::memory_order_acquire);
            lock.clear(std::memory_order_release);
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(11, "spinlock TAS+clear (no cont)", s, ns_per_cycle);
    }

    // ── 12. prepare_write(76, fast) — ring buffer 快路径 ─────────
    {
        constexpr std::size_t RB_CAP = 1 << 20;   // 1MB 够 fast path
        cpp109::ByteRingBuffer<RB_CAP> rb;
        constexpr std::size_t REQ = 72 + 4;  // 76B（legacy 72B header + 4B args 对照基准）
        for (int i = 0; i < WARMUP; ++i) {
            volatile auto p = rb.prepare_write(REQ);
            (void)p;
        }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            volatile auto p = rb.prepare_write(REQ);
            (void)p;
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(12, "prepare_write(76, fast)", s, ns_per_cycle);
    }

    // ── 13. memcpy 72B (legacy header 对照基准) ─────────────────────
    {
        alignas(64) char dst[128]{};
        std::memset(dst, 0xAB, 72);
        for (int i = 0; i < WARMUP; ++i) {
            std::memcpy(dst, dst + 4, 72);
        }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            std::memcpy(dst, dst + 4, 72);
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(13, "memcpy 72B (legacy hdr)", s, ns_per_cycle);
    }

    // ── 14. commit_write(76, release) ─────────────────────────────
    {
        constexpr std::size_t RB_CAP = 1 << 20;
        cpp109::ByteRingBuffer<RB_CAP> rb;
        constexpr std::size_t REQ = 76;
        // 先 prepare 一块空间，否则 commit 不会推进 writer_pos
        rb.prepare_write(REQ * (MEASURE + WARMUP + 1));
        for (int i = 0; i < WARMUP; ++i) {
            rb.commit_write(REQ);
        }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            rb.commit_write(REQ);
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(14, "commit_write(76, release)", s, ns_per_cycle);
    }

    // ── 15. worker_sleeping_.load(acquire) — 条件检查 ────────────
    {
        std::atomic<bool> sleeping{false};
        for (int i = 0; i < WARMUP; ++i) {
            volatile auto v = sleeping.load(std::memory_order_acquire);
            (void)v;
        }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            volatile auto v = sleeping.load(std::memory_order_acquire);
            (void)v;
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(15, "worker_sleeping_.load(acq)", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════════════
    // C. 完整路径测量（真实宏 vs 直接 log_encoded）
    // ═══════════════════════════════════════════════════════════════════

    // ── 16. 完整 async+args: logger->info("m {}", i) ──────────────
    //     走宏快路径，包含 level check + source_location + encode + 批量缓冲
    {
        for (int i = 0; i < WARMUP; ++i) {
            logger->info("m {}", i);
        }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            logger->info("m {}", i);
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(16, "full info(\"m {}\", i)", s, ns_per_cycle);
    }

    // ── 17. 完整 async no args: logger->info("hello world") ──────
    {
        for (int i = 0; i < WARMUP; ++i) {
            (void)i;
            logger->info("hello world");
        }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            (void)i;
            uint64_t c1 = rdtsc();
            logger->info("hello world");
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(17, "full info(\"hello world\")", s, ns_per_cycle);
    }

    // ── 18. log_encoded 直接调用（有参，绕过宏与批量缓冲）────────
    {
        auto* abase = dynamic_cast<cpp109::AsyncSinkBase*>(async_sink.get());
        static const cpp109::TinyMeta _meta_with_args{
            "bench_latency_instrumented.cpp", 42, "main", "m {}",
            &cpp109::detail::decode_and_format<int>
        };
        for (int i = 0; i < WARMUP; ++i) {
            std::uint32_t args_size = static_cast<std::uint32_t>(
                cpp109::detail::compute_encoded_size(i));
            std::byte* enc_buf = cpp109::detail::get_encode_buffer(args_size);
            cpp109::detail::encode_args(enc_buf, i);
            abase->log_encoded(&_meta_with_args,
                cpp109::LogLevel::INFO, 12345, cpp109::rdtsc_ns(),
                enc_buf, args_size);
        }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            std::uint32_t args_size = static_cast<std::uint32_t>(
                cpp109::detail::compute_encoded_size(i));
            std::byte* enc_buf = cpp109::detail::get_encode_buffer(args_size);
            cpp109::detail::encode_args(enc_buf, i);
            uint64_t c1 = rdtsc();
            abase->log_encoded(&_meta_with_args,
                cpp109::LogLevel::INFO, 12345, cpp109::rdtsc_ns(),
                enc_buf, args_size);
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(18, "log_encoded direct (w/ args)", s, ns_per_cycle);
    }

    // ── 19. log_encoded 直接调用（无参，绕过宏与批量缓冲）────────
    {
        auto* abase = dynamic_cast<cpp109::AsyncSinkBase*>(async_sink.get());
        static const cpp109::TinyMeta _meta_no_args{
            "bench_latency_instrumented.cpp", 42, "main", "hello world", nullptr
        };
        for (int i = 0; i < WARMUP; ++i) {
            (void)i;
            abase->log_encoded(&_meta_no_args,
                cpp109::LogLevel::INFO, 12345, cpp109::rdtsc_ns(),
                nullptr, 0);
        }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            (void)i;
            uint64_t c1 = rdtsc();
            abase->log_encoded(&_meta_no_args,
                cpp109::LogLevel::INFO, 12345, cpp109::rdtsc_ns(),
                nullptr, 0);
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(19, "log_encoded direct (no args)", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════════════
    // D. 旧路径/参考环节（已不在热路径，保留作对比基线）
    // ═══════════════════════════════════════════════════════════════════

    // ── 20. system_clock::now() — 旧时间戳方案开销 ────────────────
    {
        for (int i = 0; i < WARMUP; ++i) { volatile auto t = std::chrono::system_clock::now(); (void)t; }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            volatile auto t = std::chrono::system_clock::now();
            (void)t;
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(20, "system_clock::now()", s, ns_per_cycle);
    }

    // ── 21. std::format("m {}", i) — 完整格式化（慢路径对比）─────
    {
        for (int i = 0; i < WARMUP; ++i) { volatile auto s = std::format("m {}", i); (void)s; }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            volatile auto s = std::format("m {}", i);
            (void)s;
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(21, "std::format(\"m {}\", i)", s, ns_per_cycle);
    }

    // ── 22. thread_local vector::data() — get_encode_buffer 内部 ──
    {
        static thread_local std::vector<std::byte> tl_vec;
        if (tl_vec.size() < 128) tl_vec.resize(128);
        for (int i = 0; i < WARMUP; ++i) { volatile auto p = tl_vec.data(); (void)p; }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            volatile auto p = tl_vec.data();
            (void)p;
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(22, "thread_local vector::data()", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════════════
    // 清理
    // ═══════════════════════════════════════════════════════════════════
    // 保活 async_sink 避免 thread_local TlBatchBuf 在析构时使用悬空指针
    static std::shared_ptr<cpp109::AsyncSinkBase> keep_alive = async_sink;

    logger->flush();
    async_sink->stop();
    logger->clear_sinks();
    std::remove("__inst.log");
    std::remove("__dc.log");
    cpp109::Registry::instance().remove_all();

#ifdef _WIN32
    if (prev_mask) ::SetThreadAffinityMask(hThread, prev_mask);
#endif

    return 0;
}
