// bench_latency_mt.cpp — 多线程并发入队延迟基准（多轮取中位数）
//
// 两种模式（可 --direct 切换）：
//   默认宏路径 (LOG_*_TO)         : 走 tl_batch 批量缓冲，4KB 满才真正入队（业务常态）
//   --direct                      : 每条立即 SPSC 入队 + notify（暴露 ring 争用/worker 阻塞）
//
// 用法: bench_latency_mt.exe [--direct] [--threads 1,2,4,8] [--rounds 5] [--pin]
// 统计：每轮独立统计（每线程采样 → 合并 P50/P99），多轮后取中位数。
// 单轮 P99 噪声极大（同配置两次可差 2 倍以上），看中位数与 min/max。
// --pin：worker 绑物理核 0，生产者绑其余物理核（消除调度迁移噪声）。
// 批次容量可用 -DCPP109_BATCH_CAPACITY=N 编译期覆盖。

#include "log/log.hpp"

#ifdef _WIN32
#include <intrin.h>
#include <windows.h>
#pragma intrinsic(__rdtsc)
#else
#include <pthread.h>
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

// 绑核实验：g_pin_cpus 为物理核的首个逻辑核列表（[0] 留给 worker）
static bool              g_pin = false;
static std::vector<int>  g_pin_cpus;

static cpp109::platform::thread_handle native_current_thread() {
#ifdef _WIN32
    return ::GetCurrentThread();
#else
    return ::pthread_self();
#endif
}

static void pin_current_thread(int cpu) {
    if (cpu < 0) return;
    cpp109::platform::set_thread_affinity(native_current_thread(), &cpu, 1);
}

static const cpp109::TinyMeta _meta_mt{
    "bench_latency_mt.cpp", 42, "main", "m {}",
    &cpp109::detail::decode_and_format<int>
};

// ── 生产者主体 ──────────────────────────────────────────────
// direct == false : 宏路径（batch 缓冲，仅 4KB 满才真正入队）
// direct == true  : 直连 log_encoded（每条入队 + notify）
static void producer(int tid, bool direct, int pin_cpu,
                     std::vector<uint64_t>& out) {
    pin_current_thread(pin_cpu);
    out.reserve(static_cast<size_t>(MEASURE));

    if (!direct) {
        for (int i = 0; i < WARMUP; ++i) LOG_INFO_TO(g_logger, "m {}", i);
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
            LOG_INFO_TO(g_logger, "m {}", i);
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

// ── 单轮：运行一组并发生成者 ────────────────────────────────
static void run_round(int nthr, bool direct, double ns_per_cycle,
                      Stats& merged_out, double& worst_out) {
    std::vector<std::vector<uint64_t>> samples(static_cast<size_t>(nthr));

    g_barrier.store(nthr, std::memory_order_relaxed);
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(nthr));
    for (int t = 0; t < nthr; ++t) {
        int cpu = -1;
        if (g_pin && g_pin_cpus.size() >= 2) {
            const int n = static_cast<int>(g_pin_cpus.size()) - 1;
            cpu = g_pin_cpus[1 + (t % n)];
        }
        threads.emplace_back([t, direct, cpu, &samples]() {
            producer(t, direct, cpu, samples[static_cast<size_t>(t)]);
        });
    }
    for (auto& th : threads) th.join();

    // 每线程 P50（取最差线程）
    double worst = 0;
    for (int t = 0; t < nthr; ++t) {
        auto s = compute_stats(samples[static_cast<size_t>(t)]);
        worst = std::max(worst, s.p50);
    }

    // 全体合并 P50/P99
    std::vector<uint64_t> merged;
    for (auto& v : samples) merged.insert(merged.end(), v.begin(), v.end());
    merged_out = compute_stats(merged);
    worst_out = worst;
}

static double median_of(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
static double min_of(const std::vector<double>& v) {
    return *std::min_element(v.begin(), v.end());
}
static double max_of(const std::vector<double>& v) {
    return *std::max_element(v.begin(), v.end());
}

// ── 多轮运行，对每轮的统计量取中位数 ─────────────────────────
// 单轮 P99 受调度/背压等噪声影响波动极大（同一配置两次可差 2 倍以上），
// 因此每轮独立统计后取中位数，并输出 min/max 以观察离散度。
static void run_case(const char* mode, int nthr, bool direct,
                     double ns_per_cycle, int rounds) {
    std::vector<double> p50s, p99s, worsts;

    for (int r = 0; r < rounds; ++r) {
        Stats s{};
        double worst = 0;
        run_round(nthr, direct, ns_per_cycle, s, worst);

        double p50 = s.p50 * ns_per_cycle;
        double p99 = s.p99 * ns_per_cycle;
        worst *= ns_per_cycle;
        p50s.push_back(p50);
        p99s.push_back(p99);
        worsts.push_back(worst);

        std::printf("  [%s x%d] R%d  P50=%6.1fns  P99=%7.1fns  worst-thread P50=%6.1fns\n",
                    mode, nthr, r + 1, p50, p99, worst);
        std::fflush(stdout);

        // 轮间排空，避免上一轮积压影响下一轮
        cpp109::detail::flush_all_batches();
        g_logger->flush();
    }

    std::printf("  [%s x%d] MEDIAN(%d rounds)  P50=%6.1fns  P99=%7.1fns  "
                "worst-thread P50=%6.1fns  (P99 min/max %.1f/%.1f)\n",
                mode, nthr, rounds,
                median_of(p50s), median_of(p99s), median_of(worsts),
                min_of(p99s), max_of(p99s));
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    bool direct = false;
    bool pin = false;
    int  rounds = 5;
    std::vector<int> thread_counts{1, 2, 4, 8};

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--direct") direct = true;
        else if (a == "--pin") pin = true;
        else if (a == "--rounds" && i + 1 < argc) {
            rounds = std::atoi(argv[++i]);
            if (rounds < 1) rounds = 1;
        }
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
    std::printf("=== MT Enqueue Latency (mode: %s, ~%.2f GHz, rounds=%d%s) ===\n",
                direct ? "direct log_encoded" : "macro + batch",
                freq / 1e9, rounds, pin ? ", pin" : "");
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const char* path = "__lat_mt.log";
    std::remove(path);
    auto async_sink = cpp109::make_async_sink<cpp109::NullSink>();
    g_logger = cpp109::get_logger("mt");
    g_logger->clear_sinks();
    g_logger->set_level(cpp109::LogLevel::TRACE);
    g_logger->add_sink(async_sink);
    g_async = dynamic_cast<cpp109::AsyncSinkBase*>(async_sink.get());

    if (pin) {
        auto topo = cpp109::platform::get_cpu_topology();
        g_pin_cpus = topo.first_logical_of_each();
        if (g_pin_cpus.size() >= 2) {
            async_sink->set_affinity({g_pin_cpus[0]});   // worker 独占物理核 0
            g_pin = true;
            std::printf("  [pin] worker=cpu%d, producers on physical cores 1..%d\n",
                        g_pin_cpus[0], static_cast<int>(g_pin_cpus.size()) - 1);
            std::fflush(stdout);
        } else {
            std::printf("  [pin] topology unavailable, pinning disabled\n");
            std::fflush(stdout);
        }
    }

    for (int nthr : thread_counts) {
        run_case(direct ? "direct" : "macro", nthr, direct, ns_per_cycle, rounds);
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
