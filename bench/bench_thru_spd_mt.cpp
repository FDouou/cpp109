#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <cstdio>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <string>

void test_spdlog(int n_threads) {
    std::string path = "__thru_spd.log";
    std::remove(path.c_str());

    // Use a larger thread pool (n_threads backing threads for async)
    spdlog::init_thread_pool(65536, n_threads);
    auto logger = spdlog::create_async<spdlog::sinks::basic_file_sink_mt>("thru_spd", path, true);
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::trace);

    std::atomic<uint64_t> total{0};
    std::atomic<bool> warmup{true};
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;

    for (int t = 0; t < n_threads; ++t) {
        threads.emplace_back([&, t]() {
            uint64_t local = 0;
            while (warmup) { logger->info("m {}", ++local); }
            local = 0;
            while (running) { logger->info("m {}", ++local); }
            total += local;
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));
    warmup = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    total = 0;
    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    running = false;
    auto t1 = std::chrono::steady_clock::now();

    for (auto& th : threads) th.join();
    logger->flush();
    spdlog::drop_all();

    double sec = std::chrono::duration<double>(t1 - t0).count();
    printf("spdlog %2dt: %10llu msgs/s\n", n_threads,
           (unsigned long long)(total.load() / sec));

    spdlog::shutdown();
    std::remove(path.c_str());
}

int main() {
    printf("=== Multi-thread Throughput (async, file write) ===\n");
    printf("%-15s", "Library");
    for (int n : {1, 4, 8, 16}) printf(" %8dt", n);
    printf("\n%s\n", std::string(55, '-').c_str());

    printf("%-15s", "spdlog");
    for (int n : {1, 4, 8, 16}) {
        test_spdlog(n);
        printf(" ");
    }
    printf("\n");
    return 0;
}
