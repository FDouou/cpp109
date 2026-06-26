#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/FileSink.h>
#include <cstdio>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <string>

// UnboundedUnlimited: queue grows dynamically; producers never block.
// This avoids deadlock with multiple producer threads when the single
// backend can't drain fast enough. Large initial capacity (512 MiB)
// reduces the number of expensive reallocations during the test.
struct QuillMtOptions {
    static constexpr quill::QueueType queue_type = quill::QueueType::UnboundedUnlimited;
    static constexpr uint32_t initial_queue_capacity = 512U * 1024U * 1024U; // 512 MiB
    static constexpr uint32_t blocking_queue_retry_interval_ns = 800;
    static constexpr bool huge_pages_enabled = false;
};
using MtFrontend = quill::FrontendImpl<QuillMtOptions>;
using MtLogger = quill::LoggerImpl<QuillMtOptions>;

void test_quill(int n_threads) {
    quill::Backend::start();

    std::string path = "__thru_quill.log";
    std::remove(path.c_str());
    auto sink = MtFrontend::create_or_get_sink<quill::FileSink>(path);
    auto logger = MtFrontend::create_or_get_logger("thru_quill", std::move(sink));

    std::atomic<uint64_t> total{0};
    std::atomic<bool> warmup{true};
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;

    for (int t = 0; t < n_threads; ++t) {
        threads.emplace_back([&, t]() {
            uint64_t local = 0;
            while (warmup) { QUILL_LOG_INFO(logger, "m {}", ++local); }
            local = 0;
            while (running) { QUILL_LOG_INFO(logger, "m {}", ++local); }
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
    logger->flush_log();

    double sec = std::chrono::duration<double>(t1 - t0).count();
    printf("quill  %2dt: %10llu msgs/s\n", n_threads,
           (unsigned long long)(total.load() / sec));

    quill::Backend::stop();
    std::remove(path.c_str());
}

int main(int argc, char** argv) {
    int n_threads = 1;
    if (argc > 1) n_threads = std::atoi(argv[1]);
    if (n_threads <= 0) n_threads = 1;
    test_quill(n_threads);
    return 0;
}
