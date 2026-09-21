// bench_mixed.cpp — 混合角色游戏服 burst 基准（cpp109）
//
// 模型（贴近真实游戏服）:
//   tick 线程 T (默认 2): 对齐共享 tick 网格（默认 64tps），每帧开头写 per-tick 条，
//                        模拟战斗/事件日志（硬实时，不能掉帧）。
//   辅助线程 A (默认 6): 独立错峰节拍（默认 10ms），常态每拍写 aux-per-tick 条
//                        （网络/DB/后台日志）；每 burst-period 秒的前 burst-secs 秒
//                        为登录潮窗口，按 burst-factor 放大。
//
// 指标:
//   tick: 入队延迟 P50/P99/P99.9/max、帧 span P50/P99/max、stall>1ms、overrun、late
//   aux : 入队延迟 P50/P99（采样）、stall>1ms、条数
//
// 用法: bench_mixed.exe --tick-threads 2 --aux-threads 6 --aux-per-tick 10
//        --burst-factor 10 --burst-secs 2 --burst-period 10
//        --per-tick 156 --tick-ms 15.625 --seconds 30 --rounds 2

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

#include "log/log.hpp"

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

static const char* log_path() { return "__mixed.log"; }

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

// ── 库适配层（cpp109）────────────────────────────────────────
static std::shared_ptr<cpp109::Logger> g_logger;
static std::shared_ptr<cpp109::Sink>   g_sink_owner;

static const char* lib_name() { return "cpp109"; }

static void backend_init() {
    std::remove(log_path());
    auto inner = std::make_shared<cpp109::FileSink>(log_path(), true);
    std::shared_ptr<cpp109::Sink> sink = std::make_shared<cpp109::AsyncSink<>>(inner);
    g_logger = cpp109::get_logger("mixed");
    g_logger->clear_sinks();
    g_logger->set_level(cpp109::LogLevel::TRACE);
    g_logger->add_sink(sink);
    g_sink_owner = sink;
}
static void backend_shutdown() {
    if (g_logger) {
        g_logger->flush();
        g_logger->clear_sinks();
    }
    g_sink_owner.reset();
    cpp109::LogBackend::instance().stop();
}
static inline void log_one(int i) { LOG_INFO_TO(g_logger, "m {}", i); }

// ── 结果结构 ─────────────────────────────────────────────────
struct TickResult {
    std::vector<uint64_t> samples;      // 入队延迟（cycle，已按 stride 采样）
    std::vector<double>   spans_ns;     // 每帧写完全部 per_tick 条的耗时
    uint64_t stalls_1ms  = 0;           // 单条 >1ms 次数
    uint64_t overruns    = 0;           // span > tick 的帧数
    uint64_t late_frames = 0;           // 帧起点晚于网格 >1ms 的帧数
};

struct AuxResult {
    std::vector<uint64_t> samples;      // 入队延迟（cycle，采样）
    uint64_t stalls_1ms = 0;            // 单条 >1ms 次数
    uint64_t msgs       = 0;            // 写入条数
};

static std::chrono::steady_clock::time_point g_grid_start;
static std::atomic<int> g_barrier{0};

// ── tick 生产者：共享 tick 网格，每帧突刺 per_tick 条 ─────────
static void tick_producer(int per_tick, double tick_ns, int total_ticks, int warmup_ticks,
                          uint64_t sample_stride, uint64_t stall_cycles, TickResult& out) {
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

// ── 辅助生产者：独立节拍（错峰），潮窗口放大 ─────────────────
// start_offset_ns: 线程间错峰偏移，避免所有辅助线程同一瞬间集中写
static void aux_producer(int aux_per_tick, double interval_ns, double start_offset_ns,
                         uint64_t warmup_ns, uint64_t measure_ns,
                         uint64_t sample_stride, uint64_t stall_cycles,
                         int burst_factor, uint64_t burst_ns, uint64_t period_ns,
                         AuxResult& out) {
    g_barrier.fetch_sub(1, std::memory_order_acq_rel);
    while (g_barrier.load(std::memory_order_acquire) > 0) {}

    const auto end = g_grid_start + std::chrono::nanoseconds(
        static_cast<int64_t>(warmup_ns + measure_ns));
    auto next = g_grid_start + std::chrono::nanoseconds(
        static_cast<int64_t>(start_offset_ns));

    uint64_t seq = 0;
    while (true) {
        std::this_thread::sleep_until(next - std::chrono::microseconds(500));
        const auto now = std::chrono::steady_clock::now();
        if (now >= end) break;

        const uint64_t elapsed_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                now - g_grid_start).count());
        const bool in_burst = burst_factor > 1 && (elapsed_ns % period_ns) < burst_ns;
        const int batch = aux_per_tick * (in_burst ? burst_factor : 1);
        const bool sampling = elapsed_ns >= warmup_ns;

        for (int i = 0; i < batch; ++i) {
            const uint64_t c1 = rdtsc();
            log_one(i);
            const uint64_t c2 = rdtsc();
            const uint64_t dt = c2 - c1;
            if (sampling) {
                ++out.msgs;
                if (dt > stall_cycles) ++out.stalls_1ms;
                if ((seq++ % sample_stride) == 0) out.samples.push_back(dt);
            }
        }

        next += std::chrono::nanoseconds(static_cast<int64_t>(interval_ns));
        const auto after = std::chrono::steady_clock::now();
        if (next < after) next = after;   // 落后不追赶，保持节拍不堆叠
    }
}

static double pct_u64(const std::vector<uint64_t>& sorted, double p) {
    const size_t n = sorted.size();
    if (n == 0) return 0.0;
    size_t idx = static_cast<size_t>(p * static_cast<double>(n));
    if (idx >= n) idx = n - 1;
    return static_cast<double>(sorted[idx]);
}
static double pct_d(const std::vector<double>& sorted, double p) {
    const size_t n = sorted.size();
    if (n == 0) return 0.0;
    size_t idx = static_cast<size_t>(p * static_cast<double>(n));
    if (idx >= n) idx = n - 1;
    return sorted[idx];
}

int main(int argc, char** argv) {
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    int      tick_threads  = 2;
    int      aux_threads   = 6;
    int      aux_per_tick  = 10;
    double   aux_interval_ms = 10.0;
    int      burst_factor  = 10;
    double   burst_secs    = 2.0;
    double   burst_period  = 10.0;
    int      per_tick      = 156;
    double   tick_ms       = 15.625;
    int      seconds       = 30;
    int      warmup_ticks  = 64;
    int      rounds        = 2;
    uint64_t sample_cap    = 500000;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--tick-threads" && i + 1 < argc) tick_threads = std::atoi(argv[++i]);
        else if (a == "--aux-threads" && i + 1 < argc) aux_threads = std::atoi(argv[++i]);
        else if (a == "--aux-per-tick" && i + 1 < argc) aux_per_tick = std::atoi(argv[++i]);
        else if (a == "--aux-interval-ms" && i + 1 < argc) aux_interval_ms = std::atof(argv[++i]);
        else if (a == "--burst-factor" && i + 1 < argc) burst_factor = std::atoi(argv[++i]);
        else if (a == "--burst-secs" && i + 1 < argc) burst_secs = std::atof(argv[++i]);
        else if (a == "--burst-period" && i + 1 < argc) burst_period = std::atof(argv[++i]);
        else if (a == "--per-tick" && i + 1 < argc) per_tick = std::atoi(argv[++i]);
        else if (a == "--tick-ms" && i + 1 < argc) tick_ms = std::atof(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (a == "--warmup" && i + 1 < argc) warmup_ticks = std::atoi(argv[++i]);
        else if (a == "--rounds" && i + 1 < argc) rounds = std::atoi(argv[++i]);
        else if (a == "--sample-cap" && i + 1 < argc)
            sample_cap = std::strtoull(argv[++i], nullptr, 10);
    }
    if (tick_threads < 1) tick_threads = 1;
    if (aux_threads < 0) aux_threads = 0;
    if (aux_per_tick < 1) aux_per_tick = 1;
    if (aux_interval_ms < 0.1) aux_interval_ms = 0.1;
    if (burst_factor < 1) burst_factor = 1;
    if (burst_secs < 0.0) burst_secs = 0.0;
    if (burst_period < 0.1) burst_period = 0.1;
    if (per_tick < 1) per_tick = 1;
    if (tick_ms < 0.1) tick_ms = 0.1;
    if (seconds < 1) seconds = 1;
    if (warmup_ticks < 0) warmup_ticks = 0;
    if (rounds < 1) rounds = 1;
    if (sample_cap < 1000) sample_cap = 1000;
    if (burst_secs > burst_period) burst_secs = burst_period;

    uint64_t freq = calibrate_rdtsc_freq();
    const double ns_per_cycle = 1e9 / static_cast<double>(freq);

    const double tick_ns = tick_ms * 1e6;
    const int total_ticks =
        static_cast<int>(static_cast<double>(seconds) * 1e9 / tick_ns);
    const uint64_t warmup_ns = static_cast<uint64_t>(warmup_ticks * tick_ns);
    const uint64_t measure_ns = static_cast<uint64_t>(seconds) * 1000000000ULL;
    const double aux_interval_ns = aux_interval_ms * 1e6;
    const uint64_t burst_ns = static_cast<uint64_t>(burst_secs * 1e9);
    const uint64_t period_ns = static_cast<uint64_t>(burst_period * 1e9);

    // tick 采样 stride（与 bench_burst 一致）
    const uint64_t tick_samples_per_thread =
        static_cast<uint64_t>(per_tick) * static_cast<uint64_t>(total_ticks);
    uint64_t tick_stride = 1;
    if (tick_samples_per_thread > sample_cap)
        tick_stride = (tick_samples_per_thread + sample_cap - 1) / sample_cap;

    // aux 采样 stride（按含潮窗口的平均速率估算）
    const double aux_avg_factor = 1.0 +
        static_cast<double>(burst_factor - 1) * (burst_secs / burst_period);
    const double aux_msgs_per_thread =
        static_cast<double>(aux_per_tick) * (static_cast<double>(measure_ns) / aux_interval_ns) *
        aux_avg_factor;
    uint64_t aux_stride = 1;
    {
        const uint64_t est = static_cast<uint64_t>(aux_msgs_per_thread);
        if (est > sample_cap) aux_stride = (est + sample_cap - 1) / sample_cap;
    }

    const uint64_t stall_cycles = static_cast<uint64_t>(1e6 / ns_per_cycle);

    backend_init();

    struct RoundStats {
        // tick
        double t_p50, t_p99, t_p999, t_max;
        double t_span_p50, t_span_p99, t_span_max;
        double t_stalls, t_overruns, t_late;
        // aux
        double a_p50, a_p99;
        double a_stalls, a_msgs;
    };
    std::vector<RoundStats> rs;
    rs.reserve(static_cast<size_t>(rounds));
    long long log_bytes_acc = 0;

    for (int r = 0; r < rounds; ++r) {
        const long long log_before = file_size_bytes(log_path());

        std::vector<TickResult> tick_results(static_cast<size_t>(tick_threads));
        std::vector<AuxResult>  aux_results(static_cast<size_t>(aux_threads));
        g_barrier.store(tick_threads + aux_threads, std::memory_order_relaxed);
        g_grid_start = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);

        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(tick_threads + aux_threads));
        for (int t = 0; t < tick_threads; ++t) {
            threads.emplace_back([=, &tick_results]() {
                tick_producer(per_tick, tick_ns, total_ticks, warmup_ticks,
                              tick_stride, stall_cycles,
                              tick_results[static_cast<size_t>(t)]);
            });
        }
        for (int a = 0; a < aux_threads; ++a) {
            const double offset_ns =
                aux_interval_ns * static_cast<double>(a) / static_cast<double>(aux_threads);
            threads.emplace_back([=, &aux_results]() {
                aux_producer(aux_per_tick, aux_interval_ns, offset_ns,
                             warmup_ns, measure_ns, aux_stride, stall_cycles,
                             burst_factor, burst_ns, period_ns,
                             aux_results[static_cast<size_t>(a)]);
            });
        }
        for (auto& th : threads) th.join();

        // tick 汇总
        std::vector<uint64_t> t_merged;
        std::vector<double>   t_spans;
        uint64_t t_stalls = 0, t_overruns = 0, t_late = 0;
        for (auto& rr : tick_results) {
            t_merged.insert(t_merged.end(), rr.samples.begin(), rr.samples.end());
            t_spans.insert(t_spans.end(), rr.spans_ns.begin(), rr.spans_ns.end());
            t_stalls   += rr.stalls_1ms;
            t_overruns += rr.overruns;
            t_late     += rr.late_frames;
        }
        std::sort(t_merged.begin(), t_merged.end());
        std::sort(t_spans.begin(), t_spans.end());

        // aux 汇总
        std::vector<uint64_t> a_merged;
        uint64_t a_stalls = 0, a_msgs = 0;
        for (auto& rr : aux_results) {
            a_merged.insert(a_merged.end(), rr.samples.begin(), rr.samples.end());
            a_stalls += rr.stalls_1ms;
            a_msgs   += rr.msgs;
        }
        std::sort(a_merged.begin(), a_merged.end());

        RoundStats s{};
        s.t_p50      = pct_u64(t_merged, 0.50) * ns_per_cycle;
        s.t_p99      = pct_u64(t_merged, 0.99) * ns_per_cycle;
        s.t_p999     = pct_u64(t_merged, 0.999) * ns_per_cycle;
        s.t_max      = pct_u64(t_merged, 1.0) * ns_per_cycle;
        s.t_span_p50 = pct_d(t_spans, 0.50);
        s.t_span_p99 = pct_d(t_spans, 0.99);
        s.t_span_max = pct_d(t_spans, 1.0);
        s.t_stalls   = static_cast<double>(t_stalls);
        s.t_overruns = static_cast<double>(t_overruns);
        s.t_late     = static_cast<double>(t_late);
        s.a_p50      = pct_u64(a_merged, 0.50) * ns_per_cycle;
        s.a_p99      = pct_u64(a_merged, 0.99) * ns_per_cycle;
        s.a_stalls   = static_cast<double>(a_stalls);
        s.a_msgs     = static_cast<double>(a_msgs);
        rs.push_back(s);

        log_bytes_acc += file_size_bytes(log_path()) - log_before;
    }

    const double tick_eq_m = static_cast<double>(per_tick) * tick_threads * 1e9 / tick_ns / 1e6;
    const double aux_normal_per_s = static_cast<double>(aux_per_tick) * aux_threads *
                                    1e9 / aux_interval_ns;
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

    std::printf("[%s mixed T=%d A=%d per-tick=%d tick=%.3fms %ds rounds=%d "
                "aux=%.1fk/s burst=%dx/%.1fs/%.1fs eq=%.2fM/s+%.3fM/s "
                "log=%.0fMB/round peakWS=%.0fMB cpu=%.1fs]\n",
                lib_name(), tick_threads, aux_threads, per_tick, tick_ms, seconds, rounds,
                aux_normal_per_s / 1000.0, burst_factor, burst_secs, burst_period,
                tick_eq_m, aux_normal_per_s / 1e6,
                static_cast<double>(log_mb), peak_ws, cpu_s);
    std::printf("  tick: enq P50=%6.1fns P99=%7.1fns P99.9=%7.1fns max=%8.1fns | "
                "stall>1ms=%.0f | span P50=%7.1fus P99=%8.1fus max=%9.1fus | "
                "overrun=%.0f late>1ms=%.0f\n",
                med(&RoundStats::t_p50), med(&RoundStats::t_p99),
                med(&RoundStats::t_p999), med(&RoundStats::t_max),
                med(&RoundStats::t_stalls),
                med(&RoundStats::t_span_p50) / 1000.0,
                med(&RoundStats::t_span_p99) / 1000.0,
                med(&RoundStats::t_span_max) / 1000.0,
                med(&RoundStats::t_overruns), med(&RoundStats::t_late));
    std::printf("  aux : enq P50=%6.1fns P99=%7.1fns | stall>1ms=%.0f | msgs=%.0f/round\n",
                med(&RoundStats::a_p50), med(&RoundStats::a_p99),
                med(&RoundStats::a_stalls), med(&RoundStats::a_msgs));

    if (rounds > 1 && tick_threads > 0) {
        const auto t99r = rng(&RoundStats::t_p99);
        const auto sp99r = rng(&RoundStats::t_span_p99);
        std::printf("    tick round-range: P99 %.1f-%.1fns  spanP99 %.1f-%.1fus\n",
                    t99r.first, t99r.second, sp99r.first / 1000.0, sp99r.second / 1000.0);
    }
    std::fflush(stdout);

    return 0;
}
