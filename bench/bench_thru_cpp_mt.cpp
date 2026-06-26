#include "log/log.hpp"
#include <cstdio>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <string>

void test_cpp109(int n_threads) {
    std::vector<std::shared_ptr<cpp109::Logger>> loggers;
    std::vector<std::string> paths;

    for (int i = 0; i < n_threads; ++i) {
        std::string path = "__thru_cpp_" + std::to_string(i) + ".log";
        paths.push_back(path);
        std::remove(path.c_str());

        auto sink = cpp109::make_async_sink<cpp109::FileSink>(path, true);
        sink->set_pattern("%v");

        auto logger = cpp109::get_logger("thru_cpp_" + std::to_string(i));
        logger->clear_sinks();
        logger->set_level(cpp109::LogLevel::TRACE);
        logger->add_sink(sink);
        loggers.push_back(logger);
    }

    std::atomic<uint64_t> total{0};
    std::atomic<bool> warmup{true};
    std::atomic<bool> running{true};

    std::vector<std::thread> threads;
    for (int t = 0; t < n_threads; ++t) {
        threads.emplace_back([&, t]() {
            auto& l = loggers[t];
            uint64_t local = 0;
            while (warmup) { l->info("m {}", ++local); }
            local = 0;
            while (running) { l->info("m {}", ++local); }
            total += local;
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));  // warmup
    warmup = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    total = 0;
    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(2));  // measure
    running = false;
    auto t1 = std::chrono::steady_clock::now();

    for (auto& th : threads) th.join();
    // flush all loggers
    for (auto& l : loggers) l->flush();

    double sec = std::chrono::duration<double>(t1 - t0).count();
    printf("cpp109 %2dt: %10llu msgs/s\n", n_threads,
           (unsigned long long)(total.load() / sec));

    for (auto& p : paths) std::remove(p.c_str());
    cpp109::Registry::instance().remove_all();
}

int main() {
    printf("=== Multi-thread Throughput (async, file write) ===\n");
    printf("%-15s", "Library");
    for (int n : {1, 4, 8, 16}) printf(" %8dt", n);
    printf("\n%s\n", std::string(55, '-').c_str());

    printf("%-15s", "cpp109");
    for (int n : {1, 4, 8, 16}) {
        test_cpp109(n);
        printf(" ");
    }
    printf("\n");
    return 0;
}
