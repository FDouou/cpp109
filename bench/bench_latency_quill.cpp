// bench_latency_quill.cpp -- measure quill async enqueue latency (P50/P99) with rdtsc
//
// Scenarios:
//   1. quill async + args (FileSink)    -- full path (format + enqueue)
//   2. quill async no args (FileSink)   -- fast path (string literal)
//   3. quill async + args (NullSink)    -- pure enqueue latency
//   4. quill async no args (NullSink)   -- pure enqueue latency
//
// Build:
//   cmake --build build_release --target bench_latency_quill
//
// Expected runtime: <15s (4 groups x 2M measurements + sort)

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/FileSink.h>
#include <quill/sinks/NullSink.h>

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

// -- Custom FrontendOptions: UnboundedUnlimited queue + 4 MiB initial capacity --
// Avoids UnboundedBlocking 2GB reallocation crash on Windows.
struct QuillLatencyOptions {
    static constexpr quill::QueueType queue_type = quill::QueueType::UnboundedUnlimited;
    static constexpr uint32_t initial_queue_capacity = 4U * 1024U * 1024U; // 4 MiB
    static constexpr uint32_t blocking_queue_retry_interval_ns = 800;
    static constexpr bool huge_pages_enabled = false;
};
using QuillLatencyFrontend = quill::FrontendImpl<QuillLatencyOptions>;
using QuillLatencyLogger = quill::LoggerImpl<QuillLatencyOptions>;

// -- rdtsc (cross-platform) --
inline uint64_t rdtsc() noexcept {
#ifdef _WIN32
    return __rdtsc();
#else
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#endif
}

// -- Calibrate rdtsc frequency via steady_clock --
uint64_t calibrate_rdtsc_freq() {
    auto t1 = std::chrono::steady_clock::now();
    uint64_t c1 = rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    uint64_t c2 = rdtsc();
    auto t2 = std::chrono::steady_clock::now();
    double elapsed_sec = std::chrono::duration<double>(t2 - t1).count();
    return static_cast<uint64_t>((c2 - c1) / elapsed_sec);
}

// -- Statistics --
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

// -- Measurement template: warmup then record rdtsc delta for each call --
template<typename LogFn>
std::vector<uint64_t> measure_latency(int warmup, int measure, LogFn&& fn) {
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

// -- main --
int main() {
    uint64_t freq = calibrate_rdtsc_freq();
    double   ghz          = static_cast<double>(freq) / 1e9;
    double   ns_per_cycle = 1e9 / static_cast<double>(freq);

    constexpr int WARMUP  = 200'000;
    constexpr int MEASURE = 2'000'000;

    // ---- Start quill backend (once only) ----
    quill::Backend::start();

    // ---- 1 & 2. quill async + args / no args (FileSink) ----
    const char* file_path = "__quill_lat.log";
    std::remove(file_path);

    auto file_sink = QuillLatencyFrontend::create_or_get_sink<quill::FileSink>(file_path);
    auto file_logger = QuillLatencyFrontend::create_or_get_logger("quill_lat_file", std::move(file_sink));

    // 1a. with args
    auto cycles_file_args = measure_latency(WARMUP, MEASURE, [&](int i) {
        QUILL_LOG_INFO(file_logger, "m {}", i);
    });
    auto stats_file_args = compute_stats(cycles_file_args);

    // 1b. no args
    auto cycles_file_noargs = measure_latency(WARMUP, MEASURE, [&](int i) {
        (void)i;
        QUILL_LOG_INFO(file_logger, "hello world");
    });
    auto stats_file_noargs = compute_stats(cycles_file_noargs);

    // flush & cleanup file logger
    file_logger->flush_log();
    std::remove(file_path);

    // ---- 3 & 4. quill async + args / no args (NullSink) ----
    auto null_sink = QuillLatencyFrontend::create_or_get_sink<quill::NullSink>("null");
    auto null_logger = QuillLatencyFrontend::create_or_get_logger("quill_lat_null", std::move(null_sink));

    // 3a. with args
    auto cycles_null_args = measure_latency(WARMUP, MEASURE, [&](int i) {
        QUILL_LOG_INFO(null_logger, "m {}", i);
    });
    auto stats_null_args = compute_stats(cycles_null_args);

    // 3b. no args
    auto cycles_null_noargs = measure_latency(WARMUP, MEASURE, [&](int i) {
        (void)i;
        QUILL_LOG_INFO(null_logger, "hello world");
    });
    auto stats_null_noargs = compute_stats(cycles_null_noargs);

    null_logger->flush_log();

    // ---- Stop quill backend ----
    quill::Backend::stop();

    // ---- Print results (P50 + P99) ----
    std::printf("=== quill Enqueue Latency Benchmark (rdtsc) ===\n");
    std::printf("CPU frequency: ~%.2f GHz\n\n", ghz);

    std::printf("%-24s %10s %10s %10s %10s\n",
        "Scenario", "P50(cyc)", "P50(ns)", "P99(cyc)", "P99(ns)");
    std::printf("%s\n", std::string(68, '-').c_str());

    auto print_row = [&](const char* name, const LatencyStats& s) {
        std::printf("%-24s %10.0f %10.1f %10.0f %10.1f\n",
            name,
            s.p50, s.p50 * ns_per_cycle,
            s.p99, s.p99 * ns_per_cycle);
    };

    print_row("quill async + args (file)",  stats_file_args);
    print_row("quill async no args (file)", stats_file_noargs);
    print_row("quill async + args (null)",  stats_null_args);
    print_row("quill async no args (null)", stats_null_noargs);

    return 0;
}
