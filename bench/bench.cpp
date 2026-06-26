#include "log/log.hpp"
#include "log/platform.hpp"
#include "log/cpu_pool.hpp"
#include "log/sinks/null_sink.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
    #include <pthread.h>
#endif

#ifdef _WIN32
    #include <windows.h>
    #include <psapi.h>
    double now_us()
    {
        static LARGE_INTEGER freq;
        static int init = (QueryPerformanceFrequency(&freq), 0);
        (void)init;
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return (double)(t.QuadPart) * 1e6 / (double)freq.QuadPart;
    }
    size_t current_rss_mb()
    {
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
            return pmc.WorkingSetSize / 1024 / 1024;
        return 0;
    }
    double process_cpu_sec()
    {
        FILETIME ct, et, kt, ut;
        if (!GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) return 0;
        ULARGE_INTEGER u; u.LowPart = ut.dwLowDateTime; u.HighPart = ut.dwHighDateTime;
        return u.QuadPart * 1e-7;
    }
#else
    #include <sys/resource.h>
    #include <sys/time.h>
    #include <unistd.h>
    double now_us()
    {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        return tv.tv_sec * 1e6 + tv.tv_usec;
    }
    size_t current_rss_mb()
    {
        struct rusage u;
        getrusage(RUSAGE_SELF, &u);
        return u.ru_maxrss / 1024;
    }
    double process_cpu_sec()
    {
        struct rusage u;
        getrusage(RUSAGE_SELF, &u);
        return u.ru_utime.tv_sec + u.ru_utime.tv_usec * 1e-6
             + u.ru_stime.tv_sec + u.ru_stime.tv_usec * 1e-6;
    }
#endif

namespace {

constexpr int WARMUP_SEC = 2;
constexpr int MEASURE_SEC = 10;
constexpr int WORK_US    = 50;

#ifdef _WIN32
void simulated_work()
{
    static LARGE_INTEGER freq;
    static int init = (QueryPerformanceFrequency(&freq), 0);
    (void)init;
    LARGE_INTEGER start, now;
    QueryPerformanceCounter(&start);
    do { QueryPerformanceCounter(&now); }
    while ((now.QuadPart - start.QuadPart) * 1000000 / freq.QuadPart < WORK_US);
}
#else
void simulated_work()
{
    auto s = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - s).count() < WORK_US) {}
}
#endif

size_t count_lines(const char* filename)
{
    FILE* f = fopen(filename, "r");
    if (!f) return 0;
    size_t n = 0;
    char buf[65536];
    while (size_t r = fread(buf, 1, sizeof(buf), f))
        for (size_t i = 0; i < r; ++i)
            if (buf[i] == '\n') ++n;
    fclose(f);
    return n;
}

struct LatencyStats { double p50, p90, p99; };

LatencyStats compute_latency(std::vector<double>& samples)
{
    if (samples.size() < 100) return {0, 0, 0};
    std::sort(samples.begin(), samples.end());
    size_t n = samples.size();
    return {
        samples[(size_t)(n * 0.50)],
        samples[(size_t)(n * 0.90)],
        samples[(size_t)(n * 0.99)]
    };
}

struct BenchResult
{
    std::string name;
    int    threads;
    uint64_t produced;
    uint64_t consumed;
    double   elapsed_sec;
    double   cpu_sec;
    size_t   rss_mb;
    LatencyStats lat;
    uint64_t rate() const { return (uint64_t)(produced / elapsed_sec); }
    double   loss_pct() const {
        return produced > 0 ? 100.0 * (double)(produced - consumed) / produced : 0;
    }
};

// ── 输出 ──────────────────────────────────────────────────

static FILE* g_out = nullptr;

void tee_printf(const char* fmt, ...)
{
    va_list args1, args2;
    va_start(args1, fmt);
    va_copy(args2, args1);

    vprintf(fmt, args1);
    if (g_out) vfprintf(g_out, fmt, args2);

    va_end(args2);
    va_end(args1);
}

void print_header(const char* title)
{
    tee_printf("\n================================================================================\n");
    tee_printf("  %s\n", title);
    tee_printf("================================================================================\n");
    tee_printf("%-4s %-34s %10s %10s %6s %7s %10s %10s %10s %10s\n",
           "Th", "Scenario", "Produced", "Consumed", "Loss%",
           "RSS_MB", "msg/s", "P50us", "P90us", "P99us");
    tee_printf("%s\n", std::string(117, '-').c_str());
}

void print_mt_header(const char* title)
{
    tee_printf("\n================================================================================\n");
    tee_printf("  %s\n", title);
    tee_printf("================================================================================\n");
    tee_printf("%-4s %-34s %10s %10s %6s %7s %10s %10s\n",
           "Th", "Scenario", "Produced", "Consumed", "Loss%",
           "RSS_MB", "msg/s", "total msg/s");
    tee_printf("%s\n", std::string(97, '-').c_str());
}

void print_result(const BenchResult& r)
{
    tee_printf("%-4d %-34s %10llu %10llu %5.1f %6zu %10llu %10.0f %10.0f %10.0f\n",
           r.threads, r.name.c_str(),
           (unsigned long long)r.produced, (unsigned long long)r.consumed,
           r.loss_pct(), r.rss_mb, (unsigned long long)r.rate(),
           r.lat.p50, r.lat.p90, r.lat.p99);
}

void print_mt_result(const BenchResult& r)
{
    double per_thread = r.rate() / (double)r.threads;
    tee_printf("%-4d %-34s %10llu %10llu %5.1f %6zu %10llu %10.0f\n",
           r.threads, r.name.c_str(),
           (unsigned long long)r.produced, (unsigned long long)r.consumed,
           r.loss_pct(), r.rss_mb, (unsigned long long)r.rate(),
           per_thread);
}

// ── 单线程 — sync ───────────────────────────────────────

BenchResult bench_sync(const char* label, bool with_work)
{
    size_t rss_before = current_rss_mb();

    const char* path = "__bs_sync.log";
    std::remove(path);

    auto null = std::make_shared<cpp109::NullSink>();
    auto file = std::make_shared<cpp109::FileSink>(path, true);
    file->set_flush_interval(-1);

    auto logger = cpp109::get_logger("_bs");
    logger->clear_sinks();
    logger->set_level(cpp109::LogLevel::TRACE);
    logger->add_sink(null);

    uint64_t count = 0;
    auto warmup_end = std::chrono::steady_clock::now() + std::chrono::seconds(WARMUP_SEC);
    while (std::chrono::steady_clock::now() < warmup_end) {
        logger->info("w {}", count);
        if (with_work) simulated_work();
        ++count;
    }

    logger->clear_sinks();
    logger->add_sink(file);
    count = 0;

    std::vector<double> lat;
    double cpu_before = process_cpu_sec();
    size_t rss_peak = current_rss_mb();

    auto measure_end = std::chrono::steady_clock::now() + std::chrono::seconds(MEASURE_SEC);

    while (std::chrono::steady_clock::now() < measure_end) {
        auto t1 = now_us();
        logger->info("m {}", count);
        auto t2 = now_us();
        lat.push_back(t2 - t1);
        if (with_work) simulated_work();
        ++count;
    }

    double elapsed = (double)MEASURE_SEC;
    logger->flush();
    double cpu_elapsed = process_cpu_sec() - cpu_before;
    uint64_t actual = (uint64_t)count_lines(path);

    logger->clear_sinks();
    std::remove(path);

    size_t rss_delta = (rss_peak > rss_before) ? (rss_peak - rss_before) : 0;
    return {label, 1, count, actual, elapsed, cpu_elapsed, rss_delta, compute_latency(lat)};
}

// ── 单线程 — async ──────────────────────────────────────

BenchResult bench_async(const char* label, bool with_work)
{
    size_t rss_before = current_rss_mb();

    const char* path = "__bs_async.log";
    std::remove(path);

    auto null = std::make_shared<cpp109::NullSink>();
    auto sink = cpp109::make_async_sink<cpp109::FileSink>(path, true);

    auto logger = cpp109::get_logger("_ba");
    logger->clear_sinks();
    logger->set_level(cpp109::LogLevel::TRACE);
    logger->add_sink(null);

    uint64_t count = 0;
    auto warmup_end = std::chrono::steady_clock::now() + std::chrono::seconds(WARMUP_SEC);
    while (std::chrono::steady_clock::now() < warmup_end) {
        logger->info("w {}", count);
        if (with_work) simulated_work();
        ++count;
    }

    logger->clear_sinks();
    logger->add_sink(sink);
    count = 0;

    std::vector<double> lat;
    double cpu_before = process_cpu_sec();
    size_t rss_peak = current_rss_mb();

    auto measure_end = std::chrono::steady_clock::now() + std::chrono::seconds(MEASURE_SEC);

    while (std::chrono::steady_clock::now() < measure_end) {
        auto t1 = now_us();
        logger->info("m {}", count);
        auto t2 = now_us();
        lat.push_back(t2 - t1);
        if (with_work) simulated_work();
        ++count;
    }

    double elapsed = (double)MEASURE_SEC;
    sink->flush();
    double cpu_elapsed = process_cpu_sec() - cpu_before;
    uint64_t actual = (uint64_t)count_lines(path);

    logger->clear_sinks();
    std::remove(path);

    size_t rss_delta = (rss_peak > rss_before) ? (rss_peak - rss_before) : 0;
    return {label, 1, count, actual, elapsed, cpu_elapsed, rss_delta, compute_latency(lat)};
}

// ── 多线程 — 每线程独立 async sink + 独立文件 ───────────

BenchResult bench_multi(int n, bool with_work)
{
    size_t rss_before = current_rss_mb();

    std::vector<std::string> paths;
    std::vector<std::shared_ptr<cpp109::Logger>> loggers;

    for (int i = 0; i < n; ++i) {
        std::string p = "__bm_" + std::to_string(i) + ".log";
        paths.push_back(p);
        std::remove(p.c_str());
        auto sink = cpp109::make_async_sink<cpp109::FileSink>(p, true);
        auto l = cpp109::get_logger("__bm_" + std::to_string(i));
        l->clear_sinks();
        l->set_level(cpp109::LogLevel::TRACE);
        l->add_sink(sink);
        loggers.push_back(l);
    }

    std::atomic<uint64_t> total{0};
    std::atomic<bool> warmup{true};
    std::atomic<bool> running{true};

    auto worker = [&](int id) {
        auto& l = loggers[id];
        uint64_t local = 0;
        while (warmup) {
            l->info("w{} {}", id, local);
            if (with_work) simulated_work();
            ++local;
        }
        while (running) {
            l->info("r{} {}", id, local);
            if (with_work) simulated_work();
            ++local;
        }
        total += local;
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < n; ++i)
        threads.emplace_back(worker, i);

    std::this_thread::sleep_for(std::chrono::seconds(WARMUP_SEC));
    total.store(0);
    warmup = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    double cpu_before = process_cpu_sec();
    size_t rss_peak = current_rss_mb();

    std::this_thread::sleep_for(std::chrono::seconds(MEASURE_SEC));
    double cpu_elapsed = process_cpu_sec() - cpu_before;
    running = false;

    for (auto& t : threads) t.join();

    uint64_t actual = 0;
    for (const auto& p : paths) {
        for (auto& l : loggers) l->flush();
        actual += count_lines(p.c_str());
        std::remove(p.c_str());
    }

    loggers.clear();

    size_t rss_delta = (rss_peak > rss_before) ? (rss_peak - rss_before) : 0;
    std::string tag = std::to_string(n) + "t async" + (with_work ? " + work" : "");
    return {"cpp109 " + tag, n, total.load(), actual,
            (double)MEASURE_SEC, cpu_elapsed, rss_delta, {0, 0, 0}};
}

// ── 多线程 — 每线程独立 async sink + 亲和性绑定 ────────

BenchResult bench_multi_affinity(int n, bool with_work, const std::vector<int>& bg_cpus)
{
    size_t rss_before = current_rss_mb();

    std::vector<std::string> paths;
    std::vector<std::shared_ptr<cpp109::Logger>> loggers;
    std::vector<std::shared_ptr<cpp109::AsyncSink<>>> sinks;

    cpp109::BackgroundCpuPool pool(bg_cpus);

    for (int i = 0; i < n; ++i) {
        std::string p = "__bma_" + std::to_string(i) + ".log";
        paths.push_back(p);
        std::remove(p.c_str());
        auto sink = cpp109::make_async_sink<cpp109::FileSink>(p, true);
        auto affinity = pool.next();
        sink->set_affinity(std::move(affinity));
        sinks.push_back(sink);
        auto l = cpp109::get_logger("__bma_" + std::to_string(i));
        l->clear_sinks();
        l->set_level(cpp109::LogLevel::TRACE);
        l->add_sink(sink);
        loggers.push_back(l);
    }

    std::atomic<uint64_t> total{0};
    std::atomic<bool> warmup{true};
    std::atomic<bool> running{true};

    auto worker = [&](int id) {
        auto& l = loggers[id];
        uint64_t local = 0;
        while (warmup) {
            l->info("w{} {}", id, local);
            if (with_work) simulated_work();
            ++local;
        }
        while (running) {
            l->info("r{} {}", id, local);
            if (with_work) simulated_work();
            ++local;
        }
        total += local;
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < n; ++i)
        threads.emplace_back(worker, i);

    std::this_thread::sleep_for(std::chrono::seconds(WARMUP_SEC));
    total.store(0);
    warmup = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    double cpu_before = process_cpu_sec();
    size_t rss_peak = current_rss_mb();

    std::this_thread::sleep_for(std::chrono::seconds(MEASURE_SEC));
    double cpu_elapsed = process_cpu_sec() - cpu_before;
    running = false;

    for (auto& t : threads) t.join();
    for (auto& s : sinks) s->flush();

    uint64_t actual = 0;
    for (const auto& p : paths) {
        actual += count_lines(p.c_str());
        std::remove(p.c_str());
    }

    sinks.clear();
    loggers.clear();

    size_t rss_delta = (rss_peak > rss_before) ? (rss_peak - rss_before) : 0;
    std::string tag = std::to_string(n) + "t async+affinity" + (with_work ? " + work" : "");
    return {"cpp109 " + tag, n, total.load(), actual,
            (double)MEASURE_SEC, cpu_elapsed, rss_delta, {0, 0, 0}};
}

// ── 多线程 — NullSink 异步（排除磁盘 I/O 瓶颈）──────────

BenchResult bench_multi_nullsink(int n, bool with_work)
{
    size_t rss_before = current_rss_mb();

    std::vector<std::shared_ptr<cpp109::Logger>> loggers;
    std::vector<std::shared_ptr<cpp109::Sink>> sinks;

    for (int i = 0; i < n; ++i) {
        auto null = std::make_shared<cpp109::NullSink>();
        auto sink = std::make_shared<cpp109::AsyncSink<>>(null);
        sinks.push_back(sink);
        auto l = cpp109::get_logger("__bn_" + std::to_string(i));
        l->clear_sinks();
        l->set_level(cpp109::LogLevel::TRACE);
        l->add_sink(sink);
        loggers.push_back(l);
    }

    std::atomic<uint64_t> total{0};
    std::atomic<bool> warmup{true};
    std::atomic<bool> running{true};

    auto worker = [&](int id) {
        auto& l = loggers[id];
        uint64_t local = 0;
        while (warmup) {
            l->info("w{} {}", id, local);
            if (with_work) simulated_work();
            ++local;
        }
        while (running) {
            l->info("r{} {}", id, local);
            if (with_work) simulated_work();
            ++local;
        }
        total += local;
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < n; ++i)
        threads.emplace_back(worker, i);

    std::this_thread::sleep_for(std::chrono::seconds(WARMUP_SEC));
    total.store(0);
    warmup = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    double cpu_before = process_cpu_sec();
    size_t rss_peak = current_rss_mb();

    std::this_thread::sleep_for(std::chrono::seconds(MEASURE_SEC));
    double cpu_elapsed = process_cpu_sec() - cpu_before;
    running = false;

    for (auto& t : threads) t.join();

    for (auto& s : sinks) s->flush();
    sinks.clear();
    loggers.clear();

    uint64_t produced = total.load();
    size_t rss_delta = (rss_peak > rss_before) ? (rss_peak - rss_before) : 0;
    std::string tag = std::to_string(n) + "t async+null" + (with_work ? " + work" : "");
    return {"cpp109 " + tag, n, produced, produced,
            (double)MEASURE_SEC, cpu_elapsed, rss_delta, {0, 0, 0}};
}

// ── 多线程 — NullSink 同步（测量纯 logger 调用开销）─────

BenchResult bench_multi_sync_nullsink(int n, bool with_work)
{
    size_t rss_before = current_rss_mb();

    std::vector<std::shared_ptr<cpp109::Logger>> loggers;

    for (int i = 0; i < n; ++i) {
        auto sink = std::make_shared<cpp109::NullSink>();
        auto l = cpp109::get_logger("__bsn_" + std::to_string(i));
        l->clear_sinks();
        l->set_level(cpp109::LogLevel::TRACE);
        l->add_sink(sink);
        loggers.push_back(l);
    }

    std::atomic<uint64_t> total{0};
    std::atomic<bool> warmup{true};
    std::atomic<bool> running{true};

    auto worker = [&](int id) {
        auto& l = loggers[id];
        uint64_t local = 0;
        while (warmup) {
            l->info("w{} {}", id, local);
            if (with_work) simulated_work();
            ++local;
        }
        while (running) {
            l->info("r{} {}", id, local);
            if (with_work) simulated_work();
            ++local;
        }
        total += local;
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < n; ++i)
        threads.emplace_back(worker, i);

    std::this_thread::sleep_for(std::chrono::seconds(WARMUP_SEC));
    total.store(0);
    warmup = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    double cpu_before = process_cpu_sec();
    size_t rss_peak = current_rss_mb();

    std::this_thread::sleep_for(std::chrono::seconds(MEASURE_SEC));
    double cpu_elapsed = process_cpu_sec() - cpu_before;
    running = false;

    for (auto& t : threads) t.join();

    loggers.clear();

    uint64_t produced = total.load();
    size_t rss_delta = (rss_peak > rss_before) ? (rss_peak - rss_before) : 0;
    std::string tag = std::to_string(n) + "t sync+null" + (with_work ? " + work" : "");
    return {"cpp109 " + tag, n, produced, produced,
            (double)MEASURE_SEC, cpu_elapsed, rss_delta, {0, 0, 0}};
}

// ── 多线程 — 动态绑核策略对比 ──────────────────────────

BenchResult bench_multi_strategy(int n, bool with_work,
                                 int strategy,
                                 const std::vector<int>& worker_cpus,
                                 const std::vector<int>& bg_cpus)
{
    size_t rss_before = current_rss_mb();

    std::vector<std::string> paths;
    std::vector<std::shared_ptr<cpp109::Logger>> loggers;
    std::vector<std::shared_ptr<cpp109::AsyncSink<>>> sinks;

    cpp109::BackgroundCpuPool pool(bg_cpus);

    for (int i = 0; i < n; ++i) {
        std::string p = "__bms_" + std::to_string(i) + ".log";
        paths.push_back(p);
        std::remove(p.c_str());
        auto sink = cpp109::make_async_sink<cpp109::FileSink>(p, true);

        if (strategy >= 1) {
            auto affinity = pool.next();
            sink->set_affinity(std::move(affinity));
        }

        sinks.push_back(sink);
        auto l = cpp109::get_logger("__bms_" + std::to_string(i));
        l->clear_sinks();
        l->set_level(cpp109::LogLevel::TRACE);
        l->add_sink(sink);
        loggers.push_back(l);
    }

    std::atomic<uint64_t> total{0};
    std::atomic<bool> warmup{true};
    std::atomic<bool> running{true};

    auto worker = [&](int id) {
        if (strategy == 2) {
#ifdef _WIN32
            cpp109::platform::set_thread_affinity(
                ::GetCurrentThread(), worker_cpus.data(), worker_cpus.size());
#else
            cpp109::platform::set_thread_affinity(
                pthread_self(), worker_cpus.data(), worker_cpus.size());
#endif
        }

        auto& l = loggers[id];
        uint64_t local = 0;
        while (warmup) {
            l->info("w{} {}", id, local);
            if (with_work) simulated_work();
            ++local;
        }
        while (running) {
            l->info("r{} {}", id, local);
            if (with_work) simulated_work();
            ++local;
        }
        total += local;
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < n; ++i)
        threads.emplace_back(worker, i);

    std::this_thread::sleep_for(std::chrono::seconds(WARMUP_SEC));
    total.store(0);
    warmup = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    double cpu_before = process_cpu_sec();
    size_t rss_peak = current_rss_mb();

    std::this_thread::sleep_for(std::chrono::seconds(MEASURE_SEC));
    double cpu_elapsed = process_cpu_sec() - cpu_before;
    running = false;

    for (auto& t : threads) t.join();
    for (auto& s : sinks) s->flush();

    uint64_t actual = 0;
    for (const auto& p : paths) {
        actual += count_lines(p.c_str());
        std::remove(p.c_str());
    }

    sinks.clear();
    loggers.clear();

    size_t rss_delta = (rss_peak > rss_before) ? (rss_peak - rss_before) : 0;

    std::string strategy_name;
    switch (strategy) {
        case 0: strategy_name = "no_affinity"; break;
        case 1: strategy_name = "bg_only"; break;
        case 2: strategy_name = "iso K=" + std::to_string(bg_cpus.size()); break;
        default: strategy_name = "s" + std::to_string(strategy); break;
    }
    std::string tag = std::to_string(n) + "t " + strategy_name + (with_work ? " + work" : "");
    return {"cpp109 " + tag, n, total.load(), actual,
            (double)MEASURE_SEC, cpu_elapsed, rss_delta, {0, 0, 0}};
}

} // anonymous namespace

int main()
{
    g_out = fopen("bench/bench.txt", "a");
    if (!g_out) g_out = fopen("../bench/bench.txt", "a");

    tee_printf("=== cpp109 self-benchmark ===\n");
    tee_printf("Warmup: %ds   Measure: %ds   Simulated work: %dus\n\n",
           WARMUP_SEC, MEASURE_SEC, WORK_US);

    // ── Section 1: 单线程基线 ────────────────────────────
    print_header("1. Single-thread throughput (sync / async / async+work)");

    print_result(bench_sync("sync, no work", false));
    cpp109::Registry::instance().remove_all();
    print_result(bench_async("async, no work", false));
    cpp109::Registry::instance().remove_all();
    print_result(bench_async("async + work", true));
    cpp109::Registry::instance().remove_all();

    // ── Section 2: 多线程 async（per-thread 独立 sink + 文件）────
    print_mt_header("2. Multi-thread async (per-thread async sink + file)");
    for (int n : {4, 8, 16, 32}) {
        print_mt_result(bench_multi(n, false));
        cpp109::Registry::instance().remove_all();
    }

    // ── Section 3: 多线程 async+null（排除磁盘 I/O）────────────
    print_mt_header("3. Multi-thread async+null (no disk, isolate ringbuffer overhead)");
    for (int n : {4, 8, 16, 32}) {
        print_mt_result(bench_multi_nullsink(n, false));
        cpp109::Registry::instance().remove_all();
    }

    // ── Section 4: 多线程 sync+null（纯 logger 开销基线）─────────
    print_mt_header("4. Multi-thread sync+null (raw logger overhead, no ringbuffer)");
    for (int n : {4, 8, 16, 32}) {
        print_mt_result(bench_multi_sync_nullsink(n, false));
        cpp109::Registry::instance().remove_all();
    }

    // ── Section 5: 多线程 async + 亲和性（worker 绑核）───────────
    print_mt_header("5. Multi-thread async + affinity (worker pinned to bg cpu pool)");
    {
        unsigned int total_cpus = cpp109::platform::cpu_count();
        std::vector<int> bg;
        for (unsigned int i = (total_cpus >= 4 ? total_cpus - 4 : 0); i < total_cpus; ++i)
            bg.push_back(static_cast<int>(i));
        for (int n : {4, 8, 16, 32}) {
            print_mt_result(bench_multi_affinity(n, false, bg));
            cpp109::Registry::instance().remove_all();
        }
    }

    // ── Section 6: 绑核策略完整矩阵（工作CPU密集 + 后台IO密集）──────────
    print_mt_header("6. Affinity strategy matrix (CPU+IO mixed, with_work=true)");
    {
        unsigned int P = cpp109::platform::cpu_count();
        tee_printf("  CPU logical cores: %u\n\n", P);

        // 辅助 lambda：生成 [start, end) 的核列表
        auto range = [](int start, int end) {
            std::vector<int> v;
            for (int i = start; i < end; ++i) v.push_back(i);
            return v;
        };

        // ── 场景 A：模拟 8 核（核 0-7）──────────────────────────
        tee_printf("  --- Scenario A: 8 cores (cpu 0-7) ---\n");
        // A1: 8+8 线程
        for (int K : {1, 2, 3, 4}) {
            auto bg = range(8 - K, 8);
            auto wc = range(0, 8 - K);
            print_mt_result(bench_multi_strategy(8, true, 2, wc, bg));
            cpp109::Registry::instance().remove_all();
        }
        // A1 baseline
        print_mt_result(bench_multi_strategy(8, true, 0, {}, {}));
        cpp109::Registry::instance().remove_all();

        // A2: 32+32 线程（严重超订）
        for (int K : {2, 4}) {
            auto bg = range(8 - K, 8);
            auto wc = range(0, 8 - K);
            print_mt_result(bench_multi_strategy(32, true, 2, wc, bg));
            cpp109::Registry::instance().remove_all();
        }
        // A2 baseline
        print_mt_result(bench_multi_strategy(32, true, 0, {}, {}));
        cpp109::Registry::instance().remove_all();

        // ── 场景 B：模拟 16 核（核 0-15）─────────────────────────
        tee_printf("  --- Scenario B: 16 cores (cpu 0-15) ---\n");
        // B1: 16+16 线程
        for (int K : {2, 4, 8}) {
            auto bg = range(16 - K, 16);
            auto wc = range(0, 16 - K);
            print_mt_result(bench_multi_strategy(16, true, 2, wc, bg));
            cpp109::Registry::instance().remove_all();
        }
        // B1 baseline
        print_mt_result(bench_multi_strategy(16, true, 0, {}, {}));
        cpp109::Registry::instance().remove_all();

        // B2: 32+32 线程（超订）
        for (int K : {4, 8}) {
            auto bg = range(16 - K, 16);
            auto wc = range(0, 16 - K);
            print_mt_result(bench_multi_strategy(32, true, 2, wc, bg));
            cpp109::Registry::instance().remove_all();
        }
        // B2 baseline
        print_mt_result(bench_multi_strategy(32, true, 0, {}, {}));
        cpp109::Registry::instance().remove_all();

        // ── 场景 C：全 P 核 ───────────────────────────────────
        tee_printf("  --- Scenario C: %u cores (full) ---\n", P);
        int Pc = (int)P;
        // C1: 16+16 线程
        for (int K : {2, 4, 6, 8}) {
            auto bg = range(Pc - K, Pc);
            auto wc = range(0, Pc - K);
            print_mt_result(bench_multi_strategy(16, true, 2, wc, bg));
            cpp109::Registry::instance().remove_all();
        }
        // C1 baseline
        print_mt_result(bench_multi_strategy(16, true, 0, {}, {}));
        cpp109::Registry::instance().remove_all();

        // C2: 32+32 线程（超订）
        for (int K : {4, 8, 12}) {
            auto bg = range(Pc - K, Pc);
            auto wc = range(0, Pc - K);
            print_mt_result(bench_multi_strategy(32, true, 2, wc, bg));
            cpp109::Registry::instance().remove_all();
        }
        // C2 baseline
        print_mt_result(bench_multi_strategy(32, true, 0, {}, {}));
        cpp109::Registry::instance().remove_all();

        // ── 场景 D：拓扑感知绑核（工作线程独占物理核，后台用超线程对）──
        {
            auto topo = cpp109::platform::get_cpu_topology();
            int phys = static_cast<int>(topo.physical_count());
            auto smt_first  = topo.first_logical_of_each();   // 每物理核第1逻辑核
            auto smt_second = topo.second_logical_of_smt();   // P-core超线程第2逻辑核

            tee_printf("  --- Scenario D: topology-aware (workers=1 logical per physical core, bg=SMT sibling) ---\n");
            tee_printf("  Physical cores: %d, SMT pairs: %zu, workers first-logical: %zu, bg SMT-second: %zu\n\n",
                       phys, smt_second.size(), smt_first.size(), smt_second.size());

            // D1: 16+16 线程
            // 工作：前 min(16, physical_count) 个物理核的 first_logical（1:1 物理核独占）
            // 后台：SMT second logical（P-core 超线程对，IO 阻塞不争执行单元）
            {
                std::vector<int> wc(smt_first.begin(),
                                    smt_first.begin() + std::min(smt_first.size(), static_cast<std::size_t>(16)));
                std::vector<int> bg = smt_second;
                print_mt_result(bench_multi_strategy(16, true, 2, wc, bg));
                cpp109::Registry::instance().remove_all();
            }
            // D1 baseline（无绑核）
            print_mt_result(bench_multi_strategy(16, true, 0, {}, {}));
            cpp109::Registry::instance().remove_all();

            // D2: 32+32 线程（超订）
            {
                std::vector<int> wc(smt_first.begin(),
                                    smt_first.begin() + std::min(smt_first.size(), static_cast<std::size_t>(16)));
                std::vector<int> bg = smt_second;
                print_mt_result(bench_multi_strategy(32, true, 2, wc, bg));
                cpp109::Registry::instance().remove_all();
            }
            // D2 baseline（无绑核）
            print_mt_result(bench_multi_strategy(32, true, 0, {}, {}));
            cpp109::Registry::instance().remove_all();
        }

        // ── 场景 E：8核/16核模拟下的拓扑绑核（2:1超订，验证挽回损失）──
        {
            auto topo = cpp109::platform::get_cpu_topology();
            auto smt_first  = topo.first_logical_of_each();
            auto smt_second = topo.second_logical_of_smt();

            tee_printf("  --- Scenario E: topology-aware on limited cores (2:1 oversubscription) ---\n");
            tee_printf("  Physical cores: %zu, SMT pairs: %zu\n\n",
                       topo.physical_count(), smt_second.size());

            // E1: 模拟 8 物理核，8+8 线程（2:1 超订）
            {
                int lim = 8;
                std::vector<int> wc(smt_first.begin(),
                                    smt_first.begin() + std::min(static_cast<size_t>(lim), smt_first.size()));
                std::vector<int> bg;
                for (size_t i = 0; i < std::min(static_cast<size_t>(lim), topo.physical_cores.size()); ++i) {
                    const auto& c = topo.physical_cores[i];
                    if (c.logical_cpus.size() >= 2) bg.push_back(c.logical_cpus[1]);
                }
                tee_printf("  E1: 8 phys cores, 8+8 threads, workers=%zu cores, bg=%zu cores\n", wc.size(), bg.size());
                print_mt_result(bench_multi_strategy(8, true, 2, wc, bg));
                cpp109::Registry::instance().remove_all();
                // baseline
                print_mt_result(bench_multi_strategy(8, true, 0, {}, {}));
                cpp109::Registry::instance().remove_all();
            }

            // E2: 模拟 16 物理核，16+16 线程（2:1 超订）
            {
                int lim = 16;
                std::vector<int> wc(smt_first.begin(),
                                    smt_first.begin() + std::min(static_cast<size_t>(lim), smt_first.size()));
                std::vector<int> bg;
                for (size_t i = 0; i < std::min(static_cast<size_t>(lim), topo.physical_cores.size()); ++i) {
                    const auto& c = topo.physical_cores[i];
                    if (c.logical_cpus.size() >= 2) bg.push_back(c.logical_cpus[1]);
                }
                tee_printf("  E2: 16 phys cores, 16+16 threads, workers=%zu cores, bg=%zu cores\n", wc.size(), bg.size());
                print_mt_result(bench_multi_strategy(16, true, 2, wc, bg));
                cpp109::Registry::instance().remove_all();
                // baseline
                print_mt_result(bench_multi_strategy(16, true, 0, {}, {}));
                cpp109::Registry::instance().remove_all();
            }

            // E3: 模拟 16 物理核，32+32 线程（4:1 严重超订）
            {
                int lim = 16;
                std::vector<int> wc(smt_first.begin(),
                                    smt_first.begin() + std::min(static_cast<size_t>(lim), smt_first.size()));
                std::vector<int> bg;
                for (size_t i = 0; i < std::min(static_cast<size_t>(lim), topo.physical_cores.size()); ++i) {
                    const auto& c = topo.physical_cores[i];
                    if (c.logical_cpus.size() >= 2) bg.push_back(c.logical_cpus[1]);
                }
                tee_printf("  E3: 16 phys cores, 32+32 threads, workers=%zu cores, bg=%zu cores\n", wc.size(), bg.size());
                print_mt_result(bench_multi_strategy(32, true, 2, wc, bg));
                cpp109::Registry::instance().remove_all();
                // baseline
                print_mt_result(bench_multi_strategy(32, true, 0, {}, {}));
                cpp109::Registry::instance().remove_all();
            }
        }
    }

    tee_printf("\n");

    if (g_out) fclose(g_out);
    return 0;
}
