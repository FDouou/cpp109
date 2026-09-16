// bench_burst_quill.cpp — quill burst 延迟对照基准（本地文件，不纳入版本库）
//
// 与 bench_burst.cpp 保持完全相同的测量框架，仅替换库适配层。
//
// 用法: bench_burst_quill.exe --threads 8 --per-tick 156 --tick-ms 15.625 --seconds 20

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

#ifdef _WIN32
#include <intrin.h>
#pragma intrinsic(__rdtsc)
#pragma comment(lib, "winmm.lib")
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);
extern "C" __declspec(dllimport) unsigned int __stdcall timeEndPeriod(unsigned int);
#endif

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/FileSink.h>

static inline uint64_t rdtsc() noexcept {
#ifdef _WIN32
    return __rdtsc();
#else
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#endif
}

static uint64_t calibrate_rdtsc_freq() {
    auto t1 = std::chrono::steady_clock::now();
    uint64_t c1 = rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    uint64_t c2 = rdtsc();
    auto t2 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t2 - t1).count();
    return static_cast<uint64_t>((c2 - c1) / sec);
}

static const char* log_path() { return "__burst.log"; }

static long long file_size_bytes(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return 0;
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END);
    const long long n = _ftelli64(f);
#else
    std::fseek(f, 0, SEEK_END);
    const long long n = static_cast<long long>(std::ftell(f));
#endif
    std::fclose(f);
    return n;
}

// ── 进程资源统计（手动声明，避免引入 windows.h 宏污染）──
#ifdef _WIN32
struct MemCountersWin {
    unsigned long      cb;
    unsigned long      PageFaultCount;
    unsigned long long PeakWorkingSetSize;
    unsigned long long WorkingSetSize;
    unsigned long long QuotaPeakPagedPoolUsage;
    unsigned long long QuotaPagedPoolUsage;
    unsigned long long QuotaPeakNonPagedPoolUsage;
    unsigned long long QuotaNonPagedPoolUsage;
    unsigned long long PagefileUsage;
    unsigned long long PeakPagefileUsage;
};
extern "C" __declspec(dllimport) int __stdcall K32GetProcessMemoryInfo(
    void* hProcess, MemCountersWin* pmc, unsigned long cb);

static double peak_working_set_mb() {
    MemCountersWin mc{};
    mc.cb = sizeof(mc);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &mc, sizeof(mc))) return 0.0;
    return static_cast<double>(mc.PeakWorkingSetSize) / 1048576.0;
}
static double process_cpu_seconds() {
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0.0;
    auto to_s = [](const FILETIME& ft) {
        return static_cast<double>(
            (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) |
            ft.dwLowDateTime) * 1e-7;
    };
    return to_s(k) + to_s(u);
}
#else
static double peak_working_set_mb() { return 0.0; }
static double process_cpu_seconds() { return 0.0; }
#endif

static double median_of(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// ── 库适配层（quill，队列类型运行期可选）─────────────────────
template <quill::QueueType QT, std::size_t Cap>
struct QuillOptions : quill::FrontendOptions {
    static constexpr quill::QueueType queue_type = QT;
    static constexpr std::size_t initial_queue_capacity = Cap;
    static constexpr uint32_t blocking_queue_retry_interval_ns = 800;
};

static int         g_queue_mode = 0;   // 0=unbounded(默认) 1=bounded-dropping 2=bounded-blocking
static const char* g_lib_name   = "quill";
static void (*g_log_one)(int)   = nullptr;

static inline void log_one(int i) { g_log_one(i); }
static const char* lib_name() { return g_lib_name; }

template <typename Logger>
struct QLoggerHolder { static inline Logger* ptr = nullptr; };

template <typename Opts>
static void backend_init_t() {
    using Frontend = quill::FrontendImpl<Opts>;
    using Logger   = quill::LoggerImpl<Opts>;
    quill::Backend::start();
    std::remove(log_path());
    auto sink = Frontend::template create_or_get_sink<quill::FileSink>(
        log_path(),
        []() {
            quill::FileSinkConfig cfg;
            cfg.set_open_mode('w');
            return cfg;
        }(),
        quill::FileEventNotifier{});
    QLoggerHolder<Logger>::ptr = Frontend::create_or_get_logger("burst", std::move(sink));
    g_log_one = [](int i) { QUILL_LOG_INFO(QLoggerHolder<Logger>::ptr, "m {}", i); };
}

static void backend_init() {
    if (g_queue_mode == 1) {
        g_lib_name = "quill-bdrop";
        backend_init_t<QuillOptions<quill::QueueType::BoundedDropping, (1ULL << 20)>>();
    } else if (g_queue_mode == 2) {
        g_lib_name = "quill-bblock";
        backend_init_t<QuillOptions<quill::QueueType::BoundedBlocking, (1ULL << 20)>>();
    } else {
        g_lib_name = "quill";
        backend_init_t<QuillOptions<quill::QueueType::UnboundedBlocking, (4ULL << 20)>>();
    }
}
static void backend_shutdown() { quill::Backend::stop(); }

// ── burst 测量 ───────────────────────────────────────────────
struct BurstResult {
    std::vector<uint64_t> samples;      // 入队延迟（cycle，已按 stride 采样）
    std::vector<double>   spans_ns;     // 每帧写完全部 per_tick 条的耗时
    uint64_t stalls_1ms  = 0;           // 单条 >1ms 次数
    uint64_t overruns    = 0;           // span > tick 的帧数
    uint64_t late_frames = 0;           // 帧起点晚于网格 >1ms 的帧数
};

static std::chrono::steady_clock::time_point g_grid_start;
static std::atomic<int> g_barrier{0};

static void producer(int per_tick, double tick_ns, int total_ticks, int warmup_ticks,
                     uint64_t sample_stride, uint64_t stall_cycles, BurstResult& out) {
    out.samples.reserve(static_cast<size_t>(
        static_cast<uint64_t>(per_tick) * static_cast<uint64_t>(total_ticks) /
        sample_stride + 1));
    out.spans_ns.reserve(static_cast<size_t>(total_ticks));

    g_barrier.fetch_sub(1, std::memory_order_acq_rel);
    while (g_barrier.load(std::memory_order_acquire) > 0) {}

    const int ticks = warmup_ticks + total_ticks;
    for (int k = 0; k < ticks; ++k) {
        const auto target = g_grid_start +
            std::chrono::nanoseconds(static_cast<int64_t>(k * tick_ns));
        std::this_thread::sleep_until(target - std::chrono::microseconds(1000));
        while (std::chrono::steady_clock::now() < target) {}

        const auto b0 = std::chrono::steady_clock::now();
        for (int i = 0; i < per_tick; ++i) {
            const uint64_t c1 = rdtsc();
            log_one(i);
            const uint64_t c2 = rdtsc();
            const uint64_t dt = c2 - c1;
            if (k >= warmup_ticks) {
                if (dt > stall_cycles) ++out.stalls_1ms;
                if ((static_cast<uint64_t>(i) % sample_stride) == 0)
                    out.samples.push_back(dt);
            }
        }
        const auto b1 = std::chrono::steady_clock::now();
        if (k >= warmup_ticks) {
            const double span = std::chrono::duration<double, std::nano>(b1 - b0).count();
            out.spans_ns.push_back(span);
            if (span > tick_ns) ++out.overruns;
            if (std::chrono::duration<double, std::milli>(b0 - target).count() > 1.0)
                ++out.late_frames;
        }
    }
}

static double pct_u64(const std::vector<uint64_t>& sorted, double p) {
    const size_t n = sorted.size();
    size_t idx = static_cast<size_t>(p * static_cast<double>(n));
    if (idx >= n) idx = n - 1;
    return static_cast<double>(sorted[idx]);
}
static double pct_d(const std::vector<double>& sorted, double p) {
    const size_t n = sorted.size();
    size_t idx = static_cast<size_t>(p * static_cast<double>(n));
    if (idx >= n) idx = n - 1;
    return sorted[idx];
}

int main(int argc, char** argv) {
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    int      nthr         = 4;
    int      per_tick     = 156;
    double   tick_ms      = 15.625;
    int      seconds      = 20;
    int      warmup_ticks = 64;
    uint64_t sample_cap   = 500000;
    int      rounds       = 3;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--threads" && i + 1 < argc) nthr = std::atoi(argv[++i]);
        else if (a == "--per-tick" && i + 1 < argc) per_tick = std::atoi(argv[++i]);
        else if (a == "--tick-ms" && i + 1 < argc) tick_ms = std::atof(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (a == "--warmup" && i + 1 < argc) warmup_ticks = std::atoi(argv[++i]);
        else if (a == "--sample-cap" && i + 1 < argc)
            sample_cap = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--rounds" && i + 1 < argc) rounds = std::atoi(argv[++i]);
        else if (a == "--bounded-drop") g_queue_mode = 1;
        else if (a == "--bounded-block") g_queue_mode = 2;
    }
    if (nthr < 1) nthr = 1;
    if (per_tick < 1) per_tick = 1;
    if (tick_ms < 0.1) tick_ms = 0.1;
    if (seconds < 1) seconds = 1;
    if (warmup_ticks < 0) warmup_ticks = 0;
    if (sample_cap < 1000) sample_cap = 1000;
    if (rounds < 1) rounds = 1;

    uint64_t freq = calibrate_rdtsc_freq();
    const double ns_per_cycle = 1e9 / static_cast<double>(freq);

    const double tick_ns = tick_ms * 1e6;
    const int total_ticks =
        static_cast<int>(static_cast<double>(seconds) * 1e9 / tick_ns);
    const uint64_t total_samples_per_thread =
        static_cast<uint64_t>(per_tick) * static_cast<uint64_t>(total_ticks);
    uint64_t sample_stride = 1;
    if (total_samples_per_thread > sample_cap)
        sample_stride = (total_samples_per_thread + sample_cap - 1) / sample_cap;
    const uint64_t stall_cycles = static_cast<uint64_t>(1e6 / ns_per_cycle);

    backend_init();

    struct RoundStats {
        double p50, p99, p999, maxv;
        double span_p50, span_p99, span_max;
        double stalls, overruns, late;
    };
    std::vector<RoundStats> rs;
    rs.reserve(static_cast<size_t>(rounds));
    long long log_bytes_acc = 0;

    for (int r = 0; r < rounds; ++r) {
        const long long log_before = file_size_bytes(log_path());

        std::vector<BurstResult> results(static_cast<size_t>(nthr));
        g_barrier.store(nthr, std::memory_order_relaxed);
        g_grid_start = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);

        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(nthr));
        for (int t = 0; t < nthr; ++t) {
            threads.emplace_back([=, &results]() {
                producer(per_tick, tick_ns, total_ticks, warmup_ticks,
                         sample_stride, stall_cycles, results[static_cast<size_t>(t)]);
            });
        }
        for (auto& th : threads) th.join();

        std::vector<uint64_t> merged;
        std::vector<double>   spans;
        uint64_t stalls = 0, overruns = 0, late = 0;
        for (auto& rr : results) {
            merged.insert(merged.end(), rr.samples.begin(), rr.samples.end());
            spans.insert(spans.end(), rr.spans_ns.begin(), rr.spans_ns.end());
            stalls   += rr.stalls_1ms;
            overruns += rr.overruns;
            late     += rr.late_frames;
        }
        std::sort(merged.begin(), merged.end());
        std::sort(spans.begin(), spans.end());

        RoundStats s{};
        s.p50      = pct_u64(merged, 0.50) * ns_per_cycle;
        s.p99      = pct_u64(merged, 0.99) * ns_per_cycle;
        s.p999     = pct_u64(merged, 0.999) * ns_per_cycle;
        s.maxv     = pct_u64(merged, 1.0) * ns_per_cycle;
        s.span_p50 = pct_d(spans, 0.50);
        s.span_p99 = pct_d(spans, 0.99);
        s.span_max = pct_d(spans, 1.0);
        s.stalls   = static_cast<double>(stalls);
        s.overruns = static_cast<double>(overruns);
        s.late     = static_cast<double>(late);
        rs.push_back(s);

        log_bytes_acc += file_size_bytes(log_path()) - log_before;
    }

    const double eq_m = static_cast<double>(per_tick) * nthr * 1e9 / tick_ns / 1e6;
    const double log_mb = static_cast<double>(log_bytes_acc) / 1048576.0 / rounds;
    const double peak_ws = peak_working_set_mb();
    const double cpu_s = process_cpu_seconds();

    backend_shutdown();
#ifdef _WIN32
    timeEndPeriod(1);
#endif

    auto med = [&rs](double RoundStats::* field) {
        std::vector<double> v;
        v.reserve(rs.size());
        for (auto& s : rs) v.push_back(s.*field);
        return median_of(v);
    };
    auto rng = [&rs](double RoundStats::* field) {
        std::pair<double, double> p{1e100, -1e100};
        for (auto& s : rs) {
            p.first  = s.*field < p.first  ? s.*field : p.first;
            p.second = s.*field > p.second ? s.*field : p.second;
        }
        return p;
    };

    std::printf("[%s burst x%d per-tick=%d tick=%.3fms %ds rounds=%d eq=%.2fM/s "
                "log=%.0fMB/round peakWS=%.0fMB cpu=%.1fs] "
                "enq P50=%6.1fns P99=%7.1fns P99.9=%7.1fns max=%8.1fns | "
                "stall>1ms=%.0f | span P50=%7.1fus P99=%8.1fus max=%9.1fus | "
                "overrun=%.0f late>1ms=%.0f\n",
                lib_name(), nthr, per_tick, tick_ms, seconds, rounds, eq_m,
                static_cast<double>(log_mb), peak_ws, cpu_s,
                med(&RoundStats::p50), med(&RoundStats::p99), med(&RoundStats::p999),
                med(&RoundStats::maxv), med(&RoundStats::stalls),
                med(&RoundStats::span_p50) / 1000.0,
                med(&RoundStats::span_p99) / 1000.0,
                med(&RoundStats::span_max) / 1000.0,
                med(&RoundStats::overruns), med(&RoundStats::late));

    const auto p99r  = rng(&RoundStats::p99);
    const auto sp99r = rng(&RoundStats::span_p99);
    const auto spmxr = rng(&RoundStats::span_max);
    if (rounds > 1) {
        std::printf("    round-range: P99 %.1f-%.1fns  spanP99 %.1f-%.1fus  "
                    "spanMax %.1f-%.1fus\n",
                    p99r.first, p99r.second, sp99r.first / 1000.0, sp99r.second / 1000.0,
                    spmxr.first / 1000.0, spmxr.second / 1000.0);
    }
    std::fflush(stdout);

    return 0;
}
