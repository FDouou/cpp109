// bench_latency_mt.cpp — 多线程并发入队延迟基准
//
// 两种模式（可 --direct 切换）：
//   默认宏路径 (logger->info)     : 走 tl_batch 批量缓冲，4KB 满才真正入队（业务常态）
//   --direct                      : 每条立即 SPSC 入队 + notify（暴露 ring 争用/worker 阻塞）
//
// 用法: bench_latency_mt.exe [--direct] [--threads 1,2,4,8]
// 统计：每线程独立采样；每线程各自统计 P50，全体样本再合并算 P50/P99。

#include "log/log.hpp"

#ifdef _WIN32
#include <intrin.h>
#pragma intrinsic(__rdtsc)
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

inline uint64_t rdtsc() noexcept {
#ifdef _WIN32
    return __rdtsc();
#else
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#endif
}

uint64_t calibrate_rdtsc_freq() {
    auto t1 = std::chrono::steady_clock::now();
    uint64_t c1 = rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    uint64_t c2 = rdtsc();
    auto t2 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t2 - t1).count();
    return static_cast<uint64_t>((c2 - c1) / sec);
}

static constexpr int WARMUP  = 50'000;
static constexpr int MEASURE = 100'000;

struct Stats { double p50; double p99; };

static Stats compute_stats(std::vector<uint64_t>& v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return {
        static_cast<double>(v[n / 2]),
        static_cast<double>(v[static_cast<size_t>(n * 0.99)])
    };
}

// ── 全局被测对象 ─────────────────────────────────────────────
static std::shared_ptr<cpp109::Logger> g_logger;
static cpp109::AsyncSinkBase*          g_async = nullptr;
static std::atomic<int>                g_barrier{0};

static const cpp109::TinyMeta _meta_mt{
    "bench_latency_mt.cpp", 42, "main", "m {}",
    &cpp109::detail::decode_and_format<int>
};

// ── 生产者主体 ──────────────────────────────────────────────
// direct == false : 宏路径（batch 缓冲，仅 4KB 满才真正入队）
// direct == true  : 直连 log_encoded（每条入队 + notify）
static void producer(int tid, bool direct, std::vector<uint64_t>& out) {
    out.reserve(static_cast<size_t>(MEASURE));

    if (!direct) {
        for (int i = 0; i < WARMUP; ++i) g_logger->info("m {}", i);
    } else {
        for (int i = 0; i < WARMUP; ++i) {
            std::uint32_t sz = static_cast<std::uint32_t>(
                cpp109::detail::compute_encoded_size(i));
            std::byte* buf = cpp109::detail::get_encode_buffer(sz);
            cpp109::detail::encode_args(buf, i);
            g_async->log_encoded(&_meta_mt, cpp109::LogLevel::INFO,
                                 static_cast<std::uint64_t>(tid),
                                 cpp109::rdtsc_ns(), buf, sz);
        }
    }

    g_barrier.fetch_sub(1, std::memory_order_acq_rel);
    while (g_barrier.load(std::memory_order_acquire) > 0) {}

    for (int i = 0; i < MEASURE; ++i) {
        uint64_t c1 = rdtsc();
        if (!direct) {
            g_logger->info("m {}", i);
        } else {
            std::uint32_t sz = static_cast<std::uint32_t>(
                cpp109::detail::compute_encoded_size(i));
            std::byte* buf = cpp109::detail::get_encode_buffer(sz);
            cpp109::detail::encode_args(buf, i);
            g_async->log_encoded(&_meta_mt, cpp109::LogLevel::INFO,
                                 static_cast<std::uint64_t>(tid),
                                 cpp109::rdtsc_ns(), buf, sz);
        }
        uint64_t c2 = rdtsc();
        out.push_back(c2 - c1);
    }
}

// ── 运行一组并发生成者 ──────────────────────────────────────
static void run_case(const char* mode, int nthr, double ns_per_cycle) {
    std::vector<std::vector<uint64_t>> samples(static_cast<size_t>(nthr));

    g_barrier.store(nthr, std::memory_order_relaxed);
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(nthr));
    for (int t = 0; t < nthr; ++t) {
        threads.emplace_back([t, &samples]() {
            producer(t, false, samples[static_cast<size_t>(t)]);
        });
    }
    for (auto& th : threads) th.join();

    // 每线程 P50
    std::printf("  [%s x%d] per-thread P50:", mode, nthr);
    double worst = 0;
    for (int t = 0; t < nthr; ++t) {
        auto s = compute_stats(samples[static_cast<size_t>(t)]);
        worst = std::max(worst, s.p50);
        std::printf(" %6.1fns", s.p50 * ns_per_cycle);
    }

    // 全体合并 P50/P99
    std::vector<uint64_t> merged;
    for (auto& v : samples) merged.insert(merged.end(), v.begin(), v.end());
    auto g = compute_stats(merged);
    std::printf("   | merged P50=%7.1fns P99=%8.1fns  (worst-thread P50=%6.1fns)\n",
                g.p50 * ns_per_cycle, g.p99 * ns_per_cycle, worst * ns_per_cycle);
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    bool direct = false;
    std::vector<int> thread_counts{1, 2, 4, 8};

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--direct") direct = true;
        else if (a == "--threads") {
            thread_counts.clear();
            for (int j = i + 1; j < argc; ++j) {
                thread_counts.push_back(std::atoi(argv[j]));
            }
            break;
        }
    }

    uint64_t freq = calibrate_rdtsc_freq();
    double ns_per_cycle = 1e9 / static_cast<double>(freq);
    std::printf("=== MT Enqueue Latency (mode: %s, ~%.2f GHz) ===\n",
                direct ? "direct log_encoded" : "macro + batch", freq / 1e9);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const char* path = "__lat_mt.log";
    std::remove(path);
    auto async_sink = cpp109::make_async_sink<cpp109::NullSink>();
    g_logger = cpp109::get_logger("mt");
    g_logger->clear_sinks();
    g_logger->set_level(cpp109::LogLevel::TRACE);
    g_logger->add_sink(async_sink);
    g_async = dynamic_cast<cpp109::AsyncSinkBase*>(async_sink.get());

    for (int nthr : thread_counts) {
        run_case(direct ? "direct" : "macro", nthr, ns_per_cycle);
        // 场景间排空，避免上一轮残留影响；不换 logger（保持 fast path 命中）
        std::printf("  [flush all batches] "); std::fflush(stdout);
        cpp109::detail::flush_all_batches();
        std::printf("ok\n  [logger flush] "); std::fflush(stdout);
        g_logger->flush();
        std::printf("ok\n"); std::fflush(stdout);
    }

    std::printf("  [final flush] "); std::fflush(stdout);
    g_logger->flush();
    std::printf("ok\n  [sink stop] "); std::fflush(stdout);
    async_sink->stop();
    std::printf("ok\n  [clear sinks] "); std::fflush(stdout);
    g_logger->clear_sinks();
    std::printf("ok\n  [exit]\n"); std::fflush(stdout);
    return 0;
}
