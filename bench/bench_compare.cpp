// bench_compare.cpp — cpp109 延迟-流量矩阵基准
//
// 维度: 总注入流量 rate（--rate，M msg/s；0=不限流） × 线程数（--threads）
// 方法:
//   - 每条日志按 rate 换算节流间隔（spin/yield/sleep，--throttle）
//   - FileSink 真实落盘（进程启动时截断同名文件）
//   - 每条全采样 rdtsc；每轮 warmup 后 flush 排空；多轮取中位数
//   - 输出 real（实测吞吐）+ P50/P99（多轮中位数 + P99 min/max）
//
// 用法: bench_compare.exe --threads 32 --rate 0.5 --per-thread 10000 --rounds 20
//       [--throttle spin|yield|sleep] [--per-round]
//
// 注: 与 quill 的对照基准使用相同框架，但作为本地文件单独维护、不纳入版本库。

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

static const char* log_path() { return "__cmp.log"; }

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

static bool g_drop = false;   // true: AsyncSink<1MB, DROP_NEWEST>（有界丢弃）

// ── 库适配层（cpp109）────────────────────────────────────────
static std::shared_ptr<cpp109::Logger> g_logger;
static std::shared_ptr<cpp109::Sink>   g_sink_owner;

static const char* lib_name() { return g_drop ? "cpp109-drop" : "cpp109"; }

static void backend_init() {
    std::remove(log_path());
    auto inner = std::make_shared<cpp109::FileSink>(log_path(), true);
    std::shared_ptr<cpp109::Sink> sink;
    if (g_drop) {
        sink = std::make_shared<
            cpp109::AsyncSink<1 << 20, cpp109::OverflowPolicy::DROP_NEWEST>>(inner);
    } else {
        sink = std::make_shared<cpp109::AsyncSink<>>(inner);
    }
    g_logger = cpp109::get_logger("cmp");
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
static void backend_flush() { if (g_logger) g_logger->flush(); }

// ── 统计 ─────────────────────────────────────────────────────
struct Stats { double p50; double p99; double p999; double maxv; };

static Stats compute_stats(std::vector<uint64_t>& v) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return { static_cast<double>(v[n / 2]),
             static_cast<double>(v[static_cast<size_t>(static_cast<double>(n) * 0.99)]),
             static_cast<double>(v[static_cast<size_t>(static_cast<double>(n) * 0.999)]),
             static_cast<double>(v[n - 1]) };
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

static std::atomic<int> g_barrier{0};
static thread_local std::uint64_t g_busy_sink = 0;
static int g_throttle_mode = 0;   // 0=spin(忙等), 1=yield(让出), 2=sleep(阻塞)
static bool g_per_round = false;  // 打印延迟轮每轮统计
static int g_warmup = 200;        // 测量前排空用的预热条数（低速率档避免过长）
static bool g_busy = false;       // spin 等待期间做高 IPC 计算，模拟业务线程的密集逻辑
static bool g_preheat = false;    // 测量前先执行一条不计样本的日志，预热路径 cache/BTB

struct ThreadResult {
    std::vector<uint64_t>                 samples;
    std::chrono::steady_clock::time_point t0, t1;
};

static void producer(int per_thread, double throttle_ns, double ns_per_cycle,
                     ThreadResult& out) {
    out.samples.reserve(static_cast<size_t>(per_thread));

    // warmup（低速率档按 --warmup 截断，避免预热耗时超过测量本身）
    const int warmup = per_thread < g_warmup ? per_thread : g_warmup;
    for (int i = 0; i < warmup; ++i) log_one(i);
    // 排空后开始测量（避免 worker 一边消费积压一边与生产者争 cache）
    backend_flush();

    g_barrier.fetch_sub(1, std::memory_order_acq_rel);
    while (g_barrier.load(std::memory_order_acquire) > 0) {}

    const double throttle_cycles =
        throttle_ns > 0.0 ? throttle_ns / ns_per_cycle : 0.0;

    uint64_t next = rdtsc();
    out.t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < per_thread; ++i) {
        if (g_preheat) log_one(i);
        uint64_t c1 = rdtsc();
        log_one(i);
        uint64_t c2 = rdtsc();
        out.samples.push_back(c2 - c1);

        if (throttle_cycles > 0.0) {
            if (g_throttle_mode == 0) {
                // 自旋忙等（默认）：速率精确，但占满 CPU
                next += static_cast<uint64_t>(throttle_cycles);
                if (g_busy) {
                    std::uint64_t a0 = 1, a1 = 2, a2 = 3, a3 = 4;
                    while (rdtsc() < next) {
                        for (int k = 0; k < 64; ++k) {
                            a0 = a0 * 6364136223846793005ULL + 1442695040888963407ULL;
                            a1 = a1 * 6364136223846793005ULL + 1442695040888963407ULL;
                            a2 = a2 * 6364136223846793005ULL + 1442695040888963407ULL;
                            a3 = a3 * 6364136223846793005ULL + 1442695040888963407ULL;
                        }
                    }
                    g_busy_sink += a0 + a1 + a2 + a3;
                } else {
                    while (rdtsc() < next) {}
                }
            } else if (g_throttle_mode == 1) {
                // 让出时间片：循环直到目标时间，但每次 yield 让出 CPU
                next += static_cast<uint64_t>(throttle_cycles);
                while (rdtsc() < next) { std::this_thread::yield(); }
            } else {
                // 真正阻塞睡眠（模拟业务线程等待 IO/锁的间隙）；
                // 实际速率受 OS 定时器粒度限制，real 列会低于目标
                auto target = std::chrono::steady_clock::now() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double, std::nano>(throttle_ns));
                std::this_thread::sleep_until(target);
            }
        }
    }
    out.t1 = std::chrono::steady_clock::now();
}

static void run_round(int nthr, int per_thread, double throttle_ns,
                      double ns_per_cycle, double& thr_out,
                      Stats& merged_out, double& worst_out) {
    std::vector<ThreadResult> results(static_cast<size_t>(nthr));
    g_barrier.store(nthr, std::memory_order_relaxed);

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(nthr));
    for (int t = 0; t < nthr; ++t) {
        threads.emplace_back([per_thread, throttle_ns, ns_per_cycle, &results, t]() {
            producer(per_thread, throttle_ns, ns_per_cycle,
                     results[static_cast<size_t>(t)]);
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
        auto s = compute_stats(r.samples);
        worst = std::max(worst, s.p50);
        merged.insert(merged.end(), r.samples.begin(), r.samples.end());
    }
    double sec = std::chrono::duration<double>(t1 - t0).count();
    thr_out = (static_cast<double>(nthr) * per_thread) / sec / 1e6;
    merged_out = compute_stats(merged);
    worst_out = worst;

    backend_flush();
}

int main(int argc, char** argv) {
    int    nthr        = 1;
    double rate_m      = 0.0;     // 总注入流量目标，M msg/s；0 = 不限流
    int    per_thread  = 10000;
    int    rounds      = 3;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--threads" && i + 1 < argc) nthr = std::atoi(argv[++i]);
        else if (a == "--rate" && i + 1 < argc) rate_m = std::atof(argv[++i]);
        else if (a == "--per-thread" && i + 1 < argc) per_thread = std::atoi(argv[++i]);
        else if (a == "--throttle" && i + 1 < argc) {
            std::string m = argv[++i];
            g_throttle_mode = (m == "yield") ? 1 : (m == "sleep") ? 2 : 0;
        }
        else if (a == "--per-round") g_per_round = true;
        else if (a == "--rounds" && i + 1 < argc) rounds = std::atoi(argv[++i]);
        else if (a == "--warmup" && i + 1 < argc) g_warmup = std::atoi(argv[++i]);
        else if (a == "--busy") g_busy = true;
        else if (a == "--preheat") g_preheat = true;
        else if (a == "--drop") g_drop = true;
    }
    if (nthr < 1) nthr = 1;
    if (per_thread < 100) per_thread = 100;
    if (rounds < 1) rounds = 1;

    uint64_t freq = calibrate_rdtsc_freq();
    double ns_per_cycle = 1e9 / static_cast<double>(freq);

    // 节流：总 rate_m（M msg/s）均分到每线程后，每条日志的间隔
    //   每线程速率 = rate_m*1e6/nthr 条/s
    //   间隔(ns)  = 1e9 * nthr / (rate_m*1e6) = 1000*nthr/rate_m
    const double throttle_ns =
        rate_m > 0.0 ? 1000.0 * static_cast<double>(nthr) / rate_m : 0.0;

    backend_init();

    std::vector<double> thrs, p50s, p99s, p999s, maxs;
    for (int r = 0; r < rounds; ++r) {
        double thr = 0, worst = 0;
        Stats s{};
        run_round(nthr, per_thread, throttle_ns, ns_per_cycle, thr, s, worst);
        thrs.push_back(thr);
        p50s.push_back(s.p50 * ns_per_cycle);
        p99s.push_back(s.p99 * ns_per_cycle);
        p999s.push_back(s.p999 * ns_per_cycle);
        maxs.push_back(s.maxv * ns_per_cycle);
        if (g_per_round) {
            std::printf("    R%02d  thr=%6.2fM/s  P50=%6.1fns  P99=%7.1fns\n",
                        r + 1, thr, s.p50 * ns_per_cycle, s.p99 * ns_per_cycle);
            std::fflush(stdout);
        }
    }

    char rate_str[32];
    if (rate_m >= 1.0)      std::snprintf(rate_str, sizeof(rate_str), "%.2fM", rate_m);
    else if (rate_m > 0.0)  std::snprintf(rate_str, sizeof(rate_str), "%.0fK", rate_m * 1000.0);
    else                    std::snprintf(rate_str, sizeof(rate_str), "max");

    char real_str[32];
    const double real = median_of(thrs);
    if (real >= 1.0) std::snprintf(real_str, sizeof(real_str), "%6.2fM/s", real);
    else             std::snprintf(real_str, sizeof(real_str), "%6.0fK/s", real * 1000.0);

    std::printf("[%s x%d rate=%s] real=%s peakWS=%.0fMB cpu=%.1fs  "
                "P50=%6.1fns  P99=%7.1fns  P99.9=%7.1fns  max=%8.1fns\n",
                lib_name(), nthr, rate_str, real_str,
                peak_working_set_mb(), process_cpu_seconds(),
                median_of(p50s), median_of(p99s), median_of(p999s),
                median_of(maxs));
    std::fflush(stdout);

    backend_shutdown();
    return 0;
}
