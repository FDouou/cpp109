#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/FileSink.h>
#include <cstdio>
#include <chrono>
#include <thread>
#include <atomic>

struct QuillThruOptions {
    static constexpr quill::QueueType queue_type = quill::QueueType::BoundedBlocking;
    static constexpr uint32_t initial_queue_capacity = 256U * 1024U;
    static constexpr uint32_t blocking_queue_retry_interval_ns = 800;
    static constexpr bool huge_pages_enabled = false;
};

int main() {
    std::remove("__thru_quill.log");
    quill::Backend::start();

    using ThruFrontend = quill::FrontendImpl<QuillThruOptions>;
    using ThruLogger = quill::LoggerImpl<QuillThruOptions>;
    
    auto sink = ThruFrontend::create_or_get_sink<quill::FileSink>("__thru_quill.log");
    auto logger = ThruFrontend::create_or_get_logger("thru_quill", std::move(sink));

    const int WARMUP = 1, MEASURE = 2;
    std::atomic<uint64_t> count{0};
    std::atomic<bool> running{true};

    auto fn = [&]() {
        uint64_t local = 0;
        while (running) { QUILL_LOG_INFO(logger, "m {}", ++local); }
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
    printf("quill: %llu msgs in %.1fs = %.0f msgs/s\n",
           (unsigned long long)count.load(), sec, count.load() / sec);

    logger->flush_log();
    quill::Backend::stop();
    std::remove("__thru_quill.log");
}
