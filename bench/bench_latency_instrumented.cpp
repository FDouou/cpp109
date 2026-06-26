// bench_latency_instrumented.cpp — 插桩延迟基准测试
//
// 用 rdtsc 精确测量 cpp109 异步入队每个关键步骤的耗时，输出 P50/P99。
// 目标是找出延迟瓶颈。
//
// 编译:
//   cl /std:c++20 /O2 /EHsc /I include bench\bench_latency_instrumented.cpp
//   g++ -std=c++20 -O2 -I include bench/bench_latency_instrumented.cpp -lpthread
//
// 运行: bench_latency_instrumented
// 预期总运行时间 < 30 秒

#include "log/log.hpp"

#include <intrin.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <source_location>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

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
    std::printf("%2d. %-25s P50=%8.0f cyc (%8.1f ns)  P99=%8.0f cyc (%8.1f ns)\n",
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
    std::printf("=== Latency Instrumented Benchmark ===\n");
    std::printf("CPU: ~%.2f GHz (%.3f ns/cycle)\n\n", freq / 1e9, ns_per_cycle);

    constexpr int WARMUP  = 5000;
    constexpr int MEASURE = 50000;

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
    // B. 优化后的快路径各环节
    // ═══════════════════════════════════════════════════════════════════

    // ── 5. dynamic_cast<AsyncSinkBase*> 开销 ──────────────────────
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
        print_row(5, "dynamic_cast<AsyncSinkBase*>", s, ns_per_cycle);

        dc_sink->stop();
        std::remove(dc_path);
    }

    // ── 6. compute_encoded_size(int) 开销 ─────────────────────────
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
        print_row(6, "compute_encoded_size(int)", s, ns_per_cycle);
    }

    // ── 7. get_encode_buffer(4) 开销 ──────────────────────────────
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
        print_row(7, "get_encode_buffer(4)", s, ns_per_cycle);
    }

    // ── 8. encode_args(int) 开销 ──────────────────────────────────
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
        print_row(8, "encode_args(int)", s, ns_per_cycle);
    }

    // ── 9. spinlock test_and_set + clear (无竞争) ────────────────
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
        print_row(9, "spinlock TAS+clear (no cont)", s, ns_per_cycle);
    }

    // ── 10. prepare_write(76, fast) — ring buffer 快路径 ─────────
    {
        constexpr std::size_t RB_CAP = 1 << 20;   // 1MB 够 fast path
        cpp109::ByteRingBuffer<RB_CAP> rb;
        constexpr std::size_t REQ = sizeof(cpp109::LogRecordHeader) + 4;  // 76B
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
        print_row(10, "prepare_write(76, fast)", s, ns_per_cycle);
    }

    // ── 11. memcpy 72B (LogRecordHeader) ──────────────────────────
    {
        cpp109::LogRecordHeader hdr{};
        alignas(64) char dst[128]{};
        std::memset(&hdr, 0xAB, sizeof(hdr));
        for (int i = 0; i < WARMUP; ++i) {
            std::memcpy(dst, &hdr, sizeof(cpp109::LogRecordHeader));
        }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            std::memcpy(dst, &hdr, sizeof(cpp109::LogRecordHeader));
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row(11, "memcpy 72B (LogRecordHeader)", s, ns_per_cycle);
    }

    // ── 12. commit_write(76, release) ─────────────────────────────
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
        print_row(12, "commit_write(76, release)", s, ns_per_cycle);
    }

    // ── 13. worker_sleeping_.load(acquire) — 条件检查 ────────────
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
        print_row(13, "worker_sleeping_.load(acq)", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════════════
    // C. 完整路径测量
    // ═══════════════════════════════════════════════════════════════════

    // ── 14. 完整 async+args: logger->info("m {}", i) ──────────────
    //     走优化的 log_deferred 路径，包含 level check + source_location + encode + log_encoded
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
        print_row(14, "full info(\"m {}\", i)", s, ns_per_cycle);
    }

    // ── 15. 完整 async no args: logger->info("hello world") ──────
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
        print_row(15, "full info(\"hello world\")", s, ns_per_cycle);
    }

    // ── 16. 完整 log_encoded 直接调用（有参，绕过宏）────────────
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
        print_row(16, "log_encoded direct (w/ args)", s, ns_per_cycle);
    }

    // ── 17. 完整 log_encoded 无参直接调用 ────────────────────────
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
        print_row(17, "log_encoded direct (no args)", s, ns_per_cycle);
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
