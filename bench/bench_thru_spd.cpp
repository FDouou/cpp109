#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <cstdio>
#include <chrono>
#include <thread>
#include <atomic>

int main() {
    std::remove("__thru_spd.log");
    spdlog::init_thread_pool(8192, 1);
    auto logger = spdlog::create_async<spdlog::sinks::basic_file_sink_mt>("thru_spd", "__thru_spd.log", true);
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::trace);

    const int WARMUP = 1, MEASURE = 2;
    std::atomic<uint64_t> count{0};
    std::atomic<bool> running{true};

    auto fn = [&]() {
        uint64_t local = 0;
        while (running) { logger->info("m {}", ++local); }
        count += local;
    };

    std::thread t(fn);
    std::this_thread::sleep_for(std::chrono::seconds(WARMUP));
    count = 0;
    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(MEASURE));
    running = false;
    auto t1 = std::chrono::steady_clock::now();
    t.join();

    double sec = std::chrono::duration<double>(t1 - t0).count();
    printf("spdlog: %llu msgs in %.1fs = %.0f msgs/s\n",
           (unsigned long long)count.load(), sec, count.load() / sec);

    logger->flush();
    spdlog::shutdown();
    std::remove("__thru_spd.log");
}
