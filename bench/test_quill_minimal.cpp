#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <atomic>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/FileSink.h>

struct CustomOptions
{
    static constexpr quill::QueueType queue_type = quill::QueueType::UnboundedUnlimited;
    static constexpr uint32_t initial_queue_capacity = 4U * 1024U * 1024U;
    static constexpr uint32_t blocking_queue_retry_interval_ns = 800;
    static constexpr bool huge_pages_enabled = false;
};
using CustomFrontend = quill::FrontendImpl<CustomOptions>;
using CustomLogger = quill::LoggerImpl<CustomOptions>;

int main()
{
    quill::Backend::start();

    auto sink = CustomFrontend::create_or_get_sink<quill::FileSink>("__test_quill.log");
    CustomLogger* logger = CustomFrontend::create_or_get_logger("test", std::move(sink));

    std::atomic<bool> running{true};
    std::atomic<uint64_t> total{0};

    auto worker = [&]() {
        uint64_t local = 0;
        while (running) {
            QUILL_LOG_INFO(logger, "test message {}", local);
            ++local;
        }
        total += local;
    };

    std::thread t1(worker);
    std::thread t2(worker);

    std::this_thread::sleep_for(std::chrono::seconds(3));
    running = false;

    t1.join();
    t2.join();

    logger->flush_log();
    quill::Backend::stop();

    uint64_t lines = 0;
    FILE* f = fopen("__test_quill.log", "r");
    if (f) {
        char buf[65536];
        while (size_t r = fread(buf, 1, sizeof(buf), f))
            for (size_t i = 0; i < r; ++i)
                if (buf[i] == '\n') ++lines;
        fclose(f);
    }

    printf("Produced: %llu, Lines in file: %llu\n", (unsigned long long)total.load(), (unsigned long long)lines);
    std::remove("__test_quill.log");
    return total.load() > 0 && lines > 0 ? 0 : 1;
}
