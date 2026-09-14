// bench_throughput.cpp — cpp109 吞吐 + 延迟矩阵基准
//
// 维度: 模式（batch 容量 1K/4K/16K/64K 由 -DCPP109_BATCH_CAPACITY 决定，或 --direct）
//       × 线程数（1/2/4/8）
// 指标: 吞吐（M msg/s，墙钟，min(t0)~max(t1)）
//       + P50/P99（稀疏采样轮，默认每 8 条采 1 条，全体样本合并）
// 统计: 每配置多轮（--rounds，默认 5）后取中位数，并输出 min/max
//
// 方法要点:
//   1. 吞吐轮完全不采样（rdtsc 采样在紧密循环中可把 26ns/条推到 200ns+）；
//      延迟轮单独按 1/8 采样，两者分开统计。
//   2. 每轮 warmup 后、测量前排空 ring（flush），否则 worker 会一边消费
//      上一轮积压一边与生产者争 cache，吞吐被严重低估。
//   3. --big-ring：每分片 8MiB，测量条数不超过容量，生产者不触发 BLOCK
//      背压，用于测「前台入队吞吐上限」；默认 ring 反映端到端吞吐
//      （此时 worker 解码+格式化 ~5.5M msg/s 是瓶颈）。
//
// 用法: bench_throughput.exe [--direct] [--threads 1 2 4 8] [--rounds 5]
//                           [--per-thread 500000] [--sample-every 8] [--big-ring]

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
#include <cstdlib>
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
    return (static_cast<uint64_t>(hi) << 32) | lo;
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

static constexpr int WARMUP_PER_THREAD = 10'000;

struct Stats { double p50; double p99; };

static Stats compute_stats(std::vector<uint64_t>& v) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return {
        static_cast<double>(v[n / 2]),
        static_cast<double>(v[static_cast<size_t>(n * 0.99)])
    };
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
static double mean_of(const std::vector<double>& v) {
    double s = 0;
    for (double x : v) s += x;
    return s / static_cast<double>(v.size());
}

static std::shared_ptr<cpp109::Logger> g_logger;
static cpp109::AsyncSinkBase*          g_async = nullptr;
static std::shared_ptr<cpp109::Sink>   g_sink_owner;
static std::atomic<int>                g_barrier{0};

static const cpp109::TinyMeta _meta_bt{
    "bench_throughput.cpp", 42, "main", "m {}",
    &cpp109::detail::decode_and_format<int>
};

struct ThreadResult {
    std::vector<uint64_t>                       samples;
    std::chrono::steady_clock::time_point       t0;
    std::chrono::steady_clock::time_point       t1;
};

static void producer(int tid, bool direct, int per_thread, int sample_every,
                     double throttle_ns, double ns_per_cycle, ThreadResult& out) {
    // warmup 不入样
    if (!direct) {
        for (int i = 0; i < WARMUP_PER_THREAD; ++i) LOG_INFO_TO(g_logger, "m {}", i);
    } else {
        for (int i = 0; i < WARMUP_PER_THREAD; ++i) {
            std::uint32_t sz = static_cast<std::uint32_t>(
                cpp109::detail::compute_encoded_size(i));
            std::byte* buf = cpp109::detail::get_encode_buffer(sz);
            cpp109::detail::encode_args(buf, i);
            g_async->log_encoded(&_meta_bt, cpp109::LogLevel::INFO,
                                 static_cast<std::uint64_t>(tid),
                                 cpp109::rdtsc_ns(), buf, sz);
        }
    }

    // 排空 warmup 积压（本线程 batch + ring），避免 worker 在测量期间
    // 一边消费积压一边与生产者争 cache，导致吞吐被严重低估。
    cpp109::detail::flush_all_batches();
    g_logger->flush();

    if (sample_every > 0) {
        out.samples.reserve(static_cast<size_t>(per_thread / sample_every + 1));
    }

    g_barrier.fetch_sub(1, std::memory_order_acq_rel);
    while (g_barrier.load(std::memory_order_acquire) > 0) {}

    if (sample_every < 0) {
        // --simple 对照：与 wall_diag 完全相同的无分支测量循环
        out.t0 = std::chrono::steady_clock::now();
        if (!direct) {
            for (int i = 0; i < per_thread; ++i) LOG_INFO_TO(g_logger, "m {}", i);
        } else {
            for (int i = 0; i < per_thread; ++i) {
                std::uint32_t sz = static_cast<std::uint32_t>(
                    cpp109::detail::compute_encoded_size(i));
                std::byte* buf = cpp109::detail::get_encode_buffer(sz);
                cpp109::detail::encode_args(buf, i);
                g_async->log_encoded(&_meta_bt, cpp109::LogLevel::INFO,
                                     static_cast<std::uint64_t>(tid),
                                     cpp109::rdtsc_ns(), buf, sz);
            }
        }
        out.t1 = std::chrono::steady_clock::now();
        return;
    }

    const bool sampling = sample_every > 0;
    const double throttle_cycles = throttle_ns > 0.0 ? throttle_ns / ns_per_cycle : 0.0;

    uint64_t next = rdtsc();
    out.t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < per_thread; ++i) {
        const bool take = sampling && (i % sample_every == 0);
        uint64_t c1 = take ? rdtsc() : 0;

        if (!direct) {
            LOG_INFO_TO(g_logger, "m {}", i);
        } else {
            std::uint32_t sz = static_cast<std::uint32_t>(
                cpp109::detail::compute_encoded_size(i));
            std::byte* buf = cpp109::detail::get_encode_buffer(sz);
            cpp109::detail::encode_args(buf, i);
            g_async->log_encoded(&_meta_bt, cpp109::LogLevel::INFO,
                                 static_cast<std::uint64_t>(tid),
                                 cpp109::rdtsc_ns(), buf, sz);
        }

        if (take) {
            uint64_t c2 = rdtsc();
            out.samples.push_back(c2 - c1);
        }

        if (throttle_cycles > 0.0) {
            next += static_cast<uint64_t>(throttle_cycles);
            while (rdtsc() < next) {}
        }
    }
    out.t1 = std::chrono::steady_clock::now();
}

// ── 单轮：返回吞吐（M msg/s）、合并 P50/P99（cyc）、worst-thread P50（cyc）──
static void run_round(int nthr, bool direct, int per_thread, int sample_every,
                      double throttle_ns, double ns_per_cycle,
                      double& thr_out, Stats& merged_out, double& worst_out) {
    std::vector<ThreadResult> results(static_cast<size_t>(nthr));
    g_barrier.store(nthr, std::memory_order_relaxed);

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(nthr));
    for (int t = 0; t < nthr; ++t) {
        threads.emplace_back([t, direct, per_thread, sample_every,
                              throttle_ns, ns_per_cycle, &results]() {
            producer(t, direct, per_thread, sample_every,
                     throttle_ns, ns_per_cycle, results[static_cast<size_t>(t)]);
        });
    }
    for (auto& th : threads) th.join();

    auto t0 = results[0].t0;
    auto t1 = results[0].t1;
    double worst = 0;
    std::vector<uint64_t> merged;
    for (auto& r : results) {
        t0 = std::min(t0, r.t0);
        t1 = std::max(t1, r.t1);
        if (!r.samples.empty()) {
            auto s = compute_stats(r.samples);
            worst = std::max(worst, s.p50);
            merged.insert(merged.end(), r.samples.begin(), r.samples.end());
        }
    }
    double sec = std::chrono::duration<double>(t1 - t0).count();
    thr_out = (static_cast<double>(nthr) * per_thread) / sec / 1e6;

    merged_out = merged.empty() ? Stats{0, 0} : compute_stats(merged);
    worst_out = worst;

    cpp109::detail::flush_all_batches();
    g_logger->flush();
}

int main(int argc, char** argv) {
    bool direct = false;
    bool big_ring = false;
    bool use_file = false;
    int  rounds = 5;
    int  per_thread = 500'000;
    int  sample_every = 8;
    double throttle_ns = 0.0;
    std::vector<int> thread_counts{1, 2, 4, 8};

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--direct") direct = true;
        else if (a == "--big-ring") big_ring = true;
        else if (a == "--file") use_file = true;
        else if (a == "--simple") sample_every = -1;
        else if (a == "--throttle-ns" && i + 1 < argc) {
            throttle_ns = std::atof(argv[++i]);
            if (throttle_ns < 0.0) throttle_ns = 0.0;
        }
        else if (a == "--rounds" && i + 1 < argc) {
            rounds = std::atoi(argv[++i]);
            if (rounds < 1) rounds = 1;
        }
        else if (a == "--per-thread" && i + 1 < argc) {
            per_thread = std::atoi(argv[++i]);
            if (per_thread < 1000) per_thread = 1000;
        }
        else if (a == "--sample-every" && i + 1 < argc) {
            int v = std::atoi(argv[++i]);
            if (v >= 1) sample_every = v;
        }
        else if (a == "--threads") {
            thread_counts.clear();
            for (int j = i + 1; j < argc; ++j) {
                if (argv[j][0] == '-') break;   // 只吸收数字，不吞后续选项
                thread_counts.push_back(std::atoi(argv[j]));
                ++i;
            }
            if (thread_counts.empty()) thread_counts.push_back(1);
        }
    }

    uint64_t freq = calibrate_rdtsc_freq();
    double ns_per_cycle = 1e9 / static_cast<double>(freq);
    std::printf("=== cpp109 Throughput + Latency (%s, batch=%d, ring=%s, sink=%s, ~%.2f GHz, "
                "rounds=%d, per-thread=%d, latency-sample=1/%d, throttle=%.0fns) ===\n",
                direct ? "direct" : "batch",
                direct ? 0 : static_cast<int>(cpp109::detail::BATCH_CAPACITY),
                big_ring ? "8MiB/shard" : "default",
                use_file ? "FileSink" : "NullSink",
                freq / 1e9, rounds, per_thread, sample_every, throttle_ns);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    g_logger = cpp109::get_logger("bench_tp");
    g_logger->clear_sinks();
    g_logger->set_level(cpp109::LogLevel::TRACE);

    std::shared_ptr<cpp109::Sink> wrapped;
    if (use_file) {
        std::remove("__bench_tp.log");
        wrapped = std::make_shared<cpp109::FileSink>("__bench_tp.log", true);
    } else {
        wrapped = std::make_shared<cpp109::NullSink>();
    }

    if (big_ring) {
        auto s = std::make_shared<cpp109::AsyncSink<8 << 20>>(wrapped);
        g_logger->add_sink(s);
        g_async = dynamic_cast<cpp109::AsyncSinkBase*>(s.get());
        g_sink_owner = s;
    } else {
        auto s = std::make_shared<cpp109::AsyncSink<>>(wrapped);
        g_logger->add_sink(s);
        g_async = dynamic_cast<cpp109::AsyncSinkBase*>(s.get());
        g_sink_owner = s;
    }

    for (int nthr : thread_counts) {
        // 吞吐轮：完全不采样（--simple 时用无分支对照循环）
        std::vector<double> thrs;
        for (int r = 0; r < rounds; ++r) {
            double thr = 0, worst = 0;
            Stats s{};
            run_round(nthr, direct, per_thread,
                      sample_every < 0 ? -1 : 0, throttle_ns, ns_per_cycle,
                      thr, s, worst);
            thrs.push_back(thr);
        }

        if (sample_every < 0) {
            std::printf("  [x%d] thr=%8.2f Mmsg/s (mean %7.2f, min/max %.2f/%.2f)\n",
                        nthr, median_of(thrs), mean_of(thrs),
                        min_of(thrs), max_of(thrs));
            std::fflush(stdout);
            continue;
        }

        // 延迟轮：稀疏采样
        std::vector<double> p50s, p99s, worsts;
        for (int r = 0; r < rounds; ++r) {
            double thr = 0, worst = 0;
            Stats s{};
            run_round(nthr, direct, per_thread, sample_every,
                      throttle_ns, ns_per_cycle, thr, s, worst);
            p50s.push_back(s.p50 * ns_per_cycle);
            p99s.push_back(s.p99 * ns_per_cycle);
            worsts.push_back(worst * ns_per_cycle);
        }

        std::printf("  [x%d] thr=%8.2f Mmsg/s (mean %7.2f, min/max %.2f/%.2f) | "
                    "P50=%6.1fns P99=%7.1fns (min/max %.1f/%.1f) | worst-thread P50=%6.1fns\n",
                    nthr,
                    median_of(thrs), mean_of(thrs), min_of(thrs), max_of(thrs),
                    median_of(p50s), median_of(p99s), min_of(p99s), max_of(p99s),
                    median_of(worsts));
        std::fflush(stdout);
    }

    g_logger->flush();
    g_logger->clear_sinks();
    g_sink_owner.reset();
    cpp109::LogBackend::instance().stop();
    return 0;
}
