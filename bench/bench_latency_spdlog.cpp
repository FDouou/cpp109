// bench_latency_spdlog.cpp — 用 rdtsc 精确测量 spdlog 异步日志入队延迟 P50/P99
//
// 测量场景:
//   1. async + args   : logger->info("m {}", i)   — 完整路径（format + 入队）
//   2. async no args  : logger->info("hello world") — 快速路径（直接传字符串）
//
// 编译:
//   cmake --build build_release --target bench_latency_spdlog
//
// 预期运行时间: <10s（2 组 × 200 万次测量 + 排序）

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
    #include <intrin.h>
    #pragma intrinsic(__rdtsc)
#endif

// ── rdtsc 读取（跨平台）─────────────────────────────────────────
inline uint64_t rdtsc() noexcept {
#ifdef _WIN32
    return __rdtsc();
#else
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#endif
}

// ── 校准 rdtsc 频率（用 steady_clock）───────────────────────────
uint64_t calibrate_rdtsc_freq() {
    auto t1 = std::chrono::steady_clock::now();
    uint64_t c1 = rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    uint64_t c2 = rdtsc();
    auto t2 = std::chrono::steady_clock::now();
    double elapsed_sec = std::chrono::duration<double>(t2 - t1).count();
    return static_cast<uint64_t>((c2 - c1) / elapsed_sec);
}

// ── 统计结果 ────────────────────────────────────────────────────
struct LatencyStats {
    double p50;
    double p99;
};

LatencyStats compute_stats(std::vector<uint64_t>& samples) {
    std::sort(samples.begin(), samples.end());
    size_t n = samples.size();
    return {
        static_cast<double>(samples[static_cast<size_t>(n * 0.50)]),
        static_cast<double>(samples[static_cast<size_t>(n * 0.99)])
    };
}

// ── 测量模板：预热后记录每次调用的 rdtsc 周期数 ───────────────
template<typename LogFn>
std::vector<uint64_t> measure_latency(int warmup, int measure, LogFn&& fn) {
    // 预热
    for (int i = 0; i < warmup; ++i) {
        fn(i);
    }

    std::vector<uint64_t> cycles;
    cycles.reserve(static_cast<size_t>(measure));

    for (int i = 0; i < measure; ++i) {
        uint64_t c1 = rdtsc();
        fn(i);
        uint64_t c2 = rdtsc();
        cycles.push_back(c2 - c1);
    }
    return cycles;
}

// ── main ────────────────────────────────────────────────────────
int main() {
    // 校准 CPU 频率
    uint64_t freq = calibrate_rdtsc_freq();
    double   ghz          = static_cast<double>(freq) / 1e9;
    double   ns_per_cycle = 1e9 / static_cast<double>(freq);

    constexpr int WARMUP  = 200'000;
    constexpr int MEASURE = 2'000'000;

    // 初始化 spdlog 异步线程池（队列大小 8192，1 个后台线程）
    spdlog::init_thread_pool(8192, 1);

    const char* log_path = "__spd_lat.log";
    std::remove(log_path);

    // 创建异步文件 logger
    auto logger = spdlog::create_async<spdlog::sinks::basic_file_sink_mt>(
        "spdlog_bench", log_path, true);
    logger->set_pattern("%v");   // 最简 pattern
    logger->set_level(spdlog::level::trace);

    // ═══════════════════════════════════════════
    // 测量
    // ═══════════════════════════════════════════

    // 1a. 有参异步
    auto cycles_async_args = measure_latency(WARMUP, MEASURE, [&](int i) {
        logger->info("m {}", i);
    });
    auto stats_async_args = compute_stats(cycles_async_args);

    // 1b. 无参异步
    auto cycles_async_noargs = measure_latency(WARMUP, MEASURE, [&](int i) {
        (void)i;
        logger->info("hello world");
    });
    auto stats_async_noargs = compute_stats(cycles_async_noargs);

    // ── 清理 ────────────────────────────────────
    logger->flush();
    spdlog::shutdown();
    std::remove(log_path);

    // ═══════════════════════════════════════════
    // 输出结果
    // ═══════════════════════════════════════════
    std::printf("=== spdlog Enqueue Latency Benchmark (rdtsc, async only) ===\n");
    std::printf("CPU frequency: ~%.2f GHz\n\n", ghz);

    std::printf("%-18s %10s %10s %10s %10s\n",
        "Scenario", "P50(cyc)", "P50(ns)", "P99(cyc)", "P99(ns)");
    std::printf("%s\n", std::string(62, '-').c_str());

    auto print_row = [&](const char* name, const LatencyStats& s) {
        std::printf("%-18s %10.0f %10.1f %10.0f %10.1f\n",
            name,
            s.p50, s.p50 * ns_per_cycle,
            s.p99, s.p99 * ns_per_cycle);
    };

    print_row("async + args",    stats_async_args);
    print_row("async no args",   stats_async_noargs);

    return 0;
}
