#include "log/log.hpp"
#include "log/sinks/null_sink.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int RUN_SECONDS = 4;
constexpr int WORK_US = 10;

struct BenchResult {
    std::string name;
    uint64_t    total_msgs;
    double      seconds;
    int         threads;
    double      rate() const { return total_msgs / seconds; }
};

void simulated_work()
{
#ifdef _WIN32
    LARGE_INTEGER freq, start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    LARGE_INTEGER now;
    do { QueryPerformanceCounter(&now); }
    while ((now.QuadPart - start.QuadPart) * 1000000 / freq.QuadPart < WORK_US);
#else
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - start).count() < WORK_US) {}
#endif
}

// ── 单线程紧循环吞吐 ──────────────────────────────────────
BenchResult run_tight_loop(const std::string& name,
                           std::shared_ptr<cpp109::Sink> sink)
{
    auto logger = cpp109::get_logger("__tight__");
    logger->clear_sinks();
    logger->set_level(cpp109::LogLevel::TRACE);
    logger->add_sink(sink);

    uint64_t count = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(RUN_SECONDS);

    while (std::chrono::steady_clock::now() < deadline) {
        logger->info("b {}", count);
        ++count;
    }

    auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - deadline + std::chrono::seconds(RUN_SECONDS));
    logger->clear_sinks();
    return {name, count, elapsed.count(), 1};
}

// ── 模拟工作负载（每次日志前做一点真实工作） ──────────────
BenchResult run_workload(const std::string& name,
                          std::shared_ptr<cpp109::Sink> sink)
{
    auto logger = cpp109::get_logger("__work__");
    logger->clear_sinks();
    logger->set_level(cpp109::LogLevel::TRACE);
    logger->add_sink(sink);

    uint64_t count = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(RUN_SECONDS);

    while (std::chrono::steady_clock::now() < deadline) {
        logger->info("w {}", count);
        simulated_work();
        ++count;
    }

    auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - deadline + std::chrono::seconds(RUN_SECONDS));
    logger->clear_sinks();
    return {name, count, elapsed.count(), 1};
}

// ── 多线程并发（每线程独立 Logger，共享 Sink） ──────────
BenchResult run_multi(const std::string& name,
                       std::shared_ptr<cpp109::Sink> sink,
                       int n_threads)
{
    std::vector<std::shared_ptr<cpp109::Logger>> loggers;
    loggers.reserve(n_threads);
    for (int i = 0; i < n_threads; ++i) {
        auto logger = cpp109::get_logger("__m_" + std::to_string(i));
        logger->clear_sinks();
        logger->set_level(cpp109::LogLevel::TRACE);
        logger->add_sink(sink);
        loggers.push_back(logger);
    }

    std::atomic<uint64_t> total{0};
    std::atomic<bool> running{true};

    auto worker = [&](int id) {
        auto& logger = loggers[id];
        uint64_t local = 0;
        while (running) {
            logger->info("m{} {}", id, local);
            ++local;
        }
        total += local;
    };

    std::vector<std::thread> threads;
    threads.reserve(n_threads);
    for (int i = 0; i < n_threads; ++i)
        threads.emplace_back(worker, i);

    std::this_thread::sleep_for(std::chrono::seconds(RUN_SECONDS));
    running = false;
    for (auto& t : threads) t.join();

    return {name, total.load(), (double)RUN_SECONDS, n_threads};
}

// ── 多线程 + 工作负载（每线程独立 Logger，共享 Sink） ──────
BenchResult run_multi_workload(const std::string& name,
                                std::shared_ptr<cpp109::Sink> sink,
                                int n_threads)
{
    std::vector<std::shared_ptr<cpp109::Logger>> loggers;
    loggers.reserve(n_threads);
    for (int i = 0; i < n_threads; ++i) {
        auto logger = cpp109::get_logger("__mw_" + std::to_string(i));
        logger->clear_sinks();
        logger->set_level(cpp109::LogLevel::TRACE);
        logger->add_sink(sink);
        loggers.push_back(logger);
    }

    std::atomic<uint64_t> total{0};
    std::atomic<bool> running{true};

    auto worker = [&](int id) {
        auto& logger = loggers[id];
        uint64_t local = 0;
        while (running) {
            logger->info("mw{} {}", id, local);
            simulated_work();
            ++local;
        }
        total += local;
    };

    std::vector<std::thread> threads;
    threads.reserve(n_threads);
    for (int i = 0; i < n_threads; ++i)
        threads.emplace_back(worker, i);

    std::this_thread::sleep_for(std::chrono::seconds(RUN_SECONDS));
    running = false;
    for (auto& t : threads) t.join();

    return {name, total.load(), (double)RUN_SECONDS, n_threads};
}

// ── 输出 ───────────────────────────────────────────────────
void print_header(const char* title)
{
    printf("\n=== %s ===\n", title);
    printf("%-36s %10s %8s %10s %10s\n",
           "Scenario", "Threads", "Msgs", "Seconds", "msg/s");
    printf("%s\n", std::string(78, '-').c_str());
}

void print_result(const BenchResult& r)
{
    printf("%-36s %10d %8llu %10.2f %10.0f\n",
           r.name.c_str(),
           r.threads,
           (unsigned long long)r.total_msgs,
           r.seconds,
           r.rate());
}

void section_pure_throughput()
{
    print_header("1. single thread, tight loop");

    {
        auto r = run_tight_loop("null (format only)",
            std::make_shared<cpp109::NullSink>());
        print_result(r);
    }
    {
        auto async_null = cpp109::make_async_sink<cpp109::NullSink>();
        auto r = run_tight_loop("async null (+queue)", async_null);
        print_result(r);
    }
    {
        auto inner = std::make_shared<cpp109::NullSink>();
        auto async_drop = std::make_shared<
            cpp109::AsyncSink<8192, cpp109::OverflowPolicy::DROP_NEWEST>>(inner);
        auto r = run_tight_loop("async null (drop)", async_drop);
        print_result(r);
    }
    {
        auto file = std::make_shared<cpp109::FileSink>("__bench_sync.log", true);
        file->set_flush_interval(-1);
        auto r = run_tight_loop("sync file", file);
        print_result(r);
        std::remove("__bench_sync.log");
    }
    {
        auto f = cpp109::make_async_sink<cpp109::FileSink>("__bench_async.log", true);
        auto r = run_tight_loop("async file", f);
        print_result(r);
        std::remove("__bench_async.log");
    }
}

void section_simulated_workload()
{
    print_header("2. single thread, simulated work (10us between logs)");

    {
        auto file = std::make_shared<cpp109::FileSink>("__sw_sync.log", true);
        file->set_flush_interval(-1);
        auto r = run_workload("sync file + work", file);
        print_result(r);
        std::remove("__sw_sync.log");
    }
    {
        auto f = cpp109::make_async_sink<cpp109::FileSink>("__sw_async.log", true);
        auto r = run_workload("async file + work", f);
        print_result(r);
        std::remove("__sw_async.log");
    }
}

void section_multithreaded()
{
    const int N = 8;

    print_header("3. multithreaded, tight loop");

    {
        auto file = std::make_shared<cpp109::FileSink>("__mt_sync.log", true);
        file->set_flush_interval(-1);
        auto r = run_multi("sync file", file, N);
        print_result(r);
        std::remove("__mt_sync.log");
    }
    {
        auto f = cpp109::make_async_sink<cpp109::FileSink>("__mt_async.log", true);
        auto r = run_multi("async file", f, N);
        print_result(r);
        std::remove("__mt_async.log");
    }

    print_header("4. multithreaded, simulated work");

    {
        auto file = std::make_shared<cpp109::FileSink>("__mw_sync.log", true);
        file->set_flush_interval(-1);
        auto r = run_multi_workload("sync file + work", file, N);
        print_result(r);
        std::remove("__mw_sync.log");
    }
    {
        auto f = cpp109::make_async_sink<cpp109::FileSink>("__mw_async.log", true);
        auto r = run_multi_workload("async file + work", f, N);
        print_result(r);
        std::remove("__mw_async.log");
    }
}

} // anonymous namespace

int main()
{
    section_pure_throughput();
    section_simulated_workload();
    section_multithreaded();
    printf("\n");
    return 0;
}
