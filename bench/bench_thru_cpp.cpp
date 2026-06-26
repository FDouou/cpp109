#include "log/log.hpp"
#include <cstdio>
#include <chrono>
#include <thread>
#include <atomic>

int main() {
    std::remove("__thru_cpp.log");
    auto sink = cpp109::make_async_sink<cpp109::FileSink>("__thru_cpp.log", true);
    auto logger = cpp109::get_logger("thru");
    logger->clear_sinks();
    logger->set_level(cpp109::LogLevel::TRACE);
    logger->add_sink(sink);

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
    printf("cpp109: %llu msgs in %.1fs = %.0f msgs/s\n",
           (unsigned long long)count.load(), sec, count.load() / sec);

    logger->flush();
    sink->stop();
    std::remove("__thru_cpp.log");
}
