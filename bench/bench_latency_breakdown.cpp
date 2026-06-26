// bench_latency_breakdown.cpp — rdtsc 分段测量异步入队路径各环节耗时
//
// 目标：找出 P50 ~3000ns 入队的瓶颈环节。
// 测量 11 个环节 + 额外辅助项，每项 200 万次取 P50/P99。
//
// 编译:
//   cl /std:c++20 /O2 /EHsc /I include bench\bench_latency_breakdown.cpp
//   g++ -std=c++20 -O2 -I include bench/bench_latency_breakdown.cpp -o bench/bd -lpthread
//
// 运行: bench/bd  (或 build_release\Release\bench_latency_breakdown.exe)

#include "log/log.hpp"

#ifdef _WIN32
#include <intrin.h>
#endif
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

// ── rdtsc 跨平台封装 ──────────────────────────────────────────
inline uint64_t rdtsc() noexcept {
#ifdef _WIN32
    return __rdtsc();
#else
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#endif
}

// ── 校准 rdtsc 频率 ──────────────────────────────────────────
uint64_t calibrate_rdtsc_freq() {
    auto t1 = std::chrono::steady_clock::now();
    uint64_t c1 = rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    uint64_t c2 = rdtsc();
    auto t2 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t2 - t1).count();
    return static_cast<uint64_t>((c2 - c1) / sec);
}

// ── 统计结果 ──────────────────────────────────────────────────
struct Stats { double p50; double p99; };

Stats compute_stats(std::vector<uint64_t>& v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return {
        static_cast<double>(v[n / 2]),
        static_cast<double>(v[static_cast<size_t>(n * 0.99)])
    };
}

// ── 打印一行结果 ─────────────────────────────────────────────
void print_row(const char* name, const Stats& s, double ns_per_cycle) {
    std::printf("%-32s %8.0f cyc  %7.1f ns   (P99: %8.0f cyc  %7.1f ns)\n",
        name, s.p50, s.p50 * ns_per_cycle,
        s.p99, s.p99 * ns_per_cycle);
    std::fflush(stdout);
}

// ── 核心测量模板 ─────────────────────────────────────────────
template<typename Fn>
std::vector<uint64_t> measure(int warmup, int measure, Fn&& fn) {
    for (int i = 0; i < warmup; ++i) { fn(i); }
    std::vector<uint64_t> samples;
    samples.reserve(static_cast<size_t>(measure));
    for (int i = 0; i < measure; ++i) {
        uint64_t c1 = rdtsc();
        fn(i);
        uint64_t c2 = rdtsc();
        samples.push_back(c2 - c1);
    }
    return samples;
}



// ── main ──────────────────────────────────────────────────────
int main() {
    // 设置 stdout 为无缓冲，确保崩溃前输出可捕获
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Windows 下提高时钟精度
#ifdef _WIN32
    // 让当前线程在固定核心上运行，减少 rdtsc 波动
    HANDLE hThread = ::GetCurrentThread();
    DWORD_PTR prev_mask = ::SetThreadAffinityMask(hThread, 1);
#endif

    std::printf("=== Latency Breakdown (rdtsc) ===\n");
    std::fflush(stdout);

    uint64_t freq = calibrate_rdtsc_freq();
    double ns_per_cycle = 1e9 / static_cast<double>(freq);
    std::printf("CPU frequency: ~%.2f GHz  (%.2f ns/cycle)\n\n", freq / 1e9, ns_per_cycle);
    std::fflush(stdout);

    constexpr int WARMUP  = 200'000;
    constexpr int MEASURE = 2'000'000;  // 原始值

    // ── 创建 async sink + logger（供完整入队测量使用） ──
    const char* path = "__bd.log";
    std::remove(path);
    auto async_sink = cpp109::make_async_sink<cpp109::FileSink>(path, true);
    auto logger     = cpp109::get_logger("bd");
    logger->clear_sinks();
    logger->set_level(cpp109::LogLevel::TRACE);
    logger->add_sink(async_sink);

    std::vector<uint64_t> samples;
    samples.reserve(MEASURE);

    // ═══════════════════════════════════════════════════════════
    // 1. rdtsc 本身开销
    // ═══════════════════════════════════════════════════════════
    {
        for (int i = 0; i < WARMUP; ++i) { volatile auto c = rdtsc(); (void)c; }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row("1. rdtsc overhead", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════
    // 2. std::source_location::current() — 每行日志都调用
    // ═══════════════════════════════════════════════════════════
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
        print_row("2. source_location::current()", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════
    // 3. logger->level() atomic acquire（level check）
    // ═══════════════════════════════════════════════════════════
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
        print_row("3. level() atomic acquire", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════
    // 4. fast_sink_ — atomic_load(shared_ptr, acquire)
    // 模拟 Logger::do_log 中的 atomic_load_explicit(&fast_sink_, acquire)
    // ═══════════════════════════════════════════════════════════
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
        print_row("4. atomic_load(shared_ptr, acq)", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════
    // 5. dynamic_cast<AsyncSinkBase*> — RTTI 类型检测
    // ═══════════════════════════════════════════════════════════
    {
        // 创建一个 AsyncSink 实例，取得基类 Sink* 后 dynamic_cast
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
        print_row("5. dynamic_cast<AsyncSinkBase*>", s, ns_per_cycle);

        dc_sink->stop();
        std::remove(dc_path);
    }

    // ═══════════════════════════════════════════════════════════
    // 6. compute_encoded_size — 计算参数编码大小
    // 模拟 detail::compute_encoded_size(int)
    // ═══════════════════════════════════════════════════════════
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
        print_row("6. compute_encoded_size(int)", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════
    // 7. get_encode_buffer — thread_local 缓冲区获取
    // ═══════════════════════════════════════════════════════════
    {
        for (int i = 0; i < WARMUP; ++i) {
            volatile auto p = cpp109::detail::get_encode_buffer(64);
            (void)p;
        }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            volatile auto p = cpp109::detail::get_encode_buffer(64);
            (void)p;
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row("7. get_encode_buffer(64)", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════
    // 8. encode_args — memcpy 编码一个 int 参数
    // ═══════════════════════════════════════════════════════════
    {
        // 先获取缓冲区
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
        print_row("8. encode_args(int)", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════
    // 9. prepare_write — ring buffer 快路径（relaxed load + cached check）
    // 独立 ByteRingBuffer，只调 prepare_write 不 commit，永远快路径
    // ═══════════════════════════════════════════════════════════
    {
        constexpr std::size_t RB_CAP = 1 << 20;  // 1MB，够 2M 次 fast path
        cpp109::ByteRingBuffer<RB_CAP> rb;
        std::size_t req = sizeof(cpp109::LogRecordHeader) + 4;  // 76 bytes
        for (int i = 0; i < WARMUP; ++i) { volatile auto p = rb.prepare_write(req); (void)p; }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            volatile auto p = rb.prepare_write(req);
            (void)p;
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row("9. prepare_write(76B, fast)", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════
    // 10. memcpy 72 bytes — 写入 LogRecordHeader
    // ═══════════════════════════════════════════════════════════
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
        print_row("10. memcpy 72B (header)", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════
    // 11. commit_write — ring buffer release store
    // ═══════════════════════════════════════════════════════════
    {
        constexpr std::size_t RB_CAP = 1 << 20;
        cpp109::ByteRingBuffer<RB_CAP> rb;
        std::size_t req = 76;
        for (int i = 0; i < WARMUP; ++i) { rb.commit_write(req); }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            rb.commit_write(req);
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row("11. commit_write(76, release)", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════
    // 12. worker_sleeping_.load(acquire) — atomic<bool> acquire
    // ═══════════════════════════════════════════════════════════
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
        print_row("12. atomic<bool> load(acquire)", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════
    // 13. 完整入队：logger->info("m {}", i) — 用单条 log_encoded 路径
    // ═══════════════════════════════════════════════════════════
    {
        std::printf("--- test 13: log_encoded single-path ---\n"); std::fflush(stdout);
        auto* abase = dynamic_cast<cpp109::AsyncSinkBase*>(async_sink.get());
        // 每个模板实例化一个唯一的 static TinyMeta
        static const cpp109::TinyMeta _meta_with_args{
            "bench.cpp", 42, "test", "m {}",
            &cpp109::detail::decode_and_format<int>
        };
        // warmup
        for (int i = 0; i < WARMUP; ++i) {
            std::uint32_t args_size = static_cast<std::uint32_t>(
                cpp109::detail::compute_encoded_size(i));
            std::byte* enc_buf = cpp109::detail::get_encode_buffer(args_size);
            cpp109::detail::encode_args(enc_buf, i);
            abase->log_encoded(&_meta_with_args,
                cpp109::LogLevel::INFO, 12345, cpp109::rdtsc_ns(),
                enc_buf, args_size);
        }
        // measure
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
        print_row("13. full info(\"m {}\", i)", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════
    // 14. 完整入队：logger->info("hello") — 无参快速路径
    //     也走单条 log_encoded（无参，decode_fn=nullptr）
    // ═══════════════════════════════════════════════════════════
    {
        auto* abase = dynamic_cast<cpp109::AsyncSinkBase*>(async_sink.get());
        static const cpp109::TinyMeta _meta_no_args{
            "bench.cpp", 42, "test", "hello", nullptr
        };
        for (int i = 0; i < WARMUP; ++i) {
            abase->log_encoded(&_meta_no_args,
                cpp109::LogLevel::INFO, 12345, cpp109::rdtsc_ns(),
                nullptr, 0);
        }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            abase->log_encoded(&_meta_no_args,
                cpp109::LogLevel::INFO, 12345, cpp109::rdtsc_ns(),
                nullptr, 0);
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row("14. full info(\"hello\")", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════
    // 15. system_clock::now() — 旧路径时间戳开销（用于对比）
    // ═══════════════════════════════════════════════════════════
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
        print_row("15. system_clock::now()", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════
    // 16. thread_local 变量访问（如 tl_tid, tl_buf 等）
    // ═══════════════════════════════════════════════════════════
    {
        static thread_local uint64_t tl_var = 0;
        for (int i = 0; i < WARMUP; ++i) { volatile auto v = tl_var; (void)v; }
        samples.clear();
        for (int i = 0; i < MEASURE; ++i) {
            uint64_t c1 = rdtsc();
            volatile auto v = tl_var;
            (void)v;
            uint64_t c2 = rdtsc();
            samples.push_back(c2 - c1);
        }
        auto s = compute_stats(samples);
        print_row("16. thread_local access", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════
    // 17. std::format — 完整格式化（慢路径 fallback 用，对比）
    // ═══════════════════════════════════════════════════════════
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
        print_row("17. std::format(\"m {}\", i)", s, ns_per_cycle);
    }

    // ═══════════════════════════════════════════════════════════
    // 18. thread_local vector::data() — get_encode_buffer 内部操作
    // ═══════════════════════════════════════════════════════════
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
        print_row("18. thread_local vector::data()", s, ns_per_cycle);
    }

    // ── 清理 ──
    // 保活 async_sink 避免 thread_local TlBatchBuf 在析构时使用悬空指针
    static std::shared_ptr<cpp109::AsyncSinkBase> keep_alive = async_sink;

    logger->flush();
    async_sink->stop();
    logger->clear_sinks();
    cpp109::Registry::instance().remove_all();
    std::remove(path);

#ifdef _WIN32
    // 恢复线程亲和性
    if (prev_mask) ::SetThreadAffinityMask(hThread, prev_mask);
#endif

    return 0;
}
