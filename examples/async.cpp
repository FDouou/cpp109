// async.cpp — 异步日志示例

#include "log/log.hpp"
#include <thread>

int main() {
    auto logger = cpp109::get_logger("async_demo");

    auto async_file = cpp109::make_async_sink<cpp109::RotatingFileSink>(
        "async_factory.log", 10, 5);
    logger->add_sink(async_file);

    auto async_drop = cpp109::make_async_sink<
        cpp109::FileSink,
        1 << 20,
        cpp109::OverflowPolicy::DROP_NEWEST
    >("async_drop.log");
    logger->add_sink(async_drop);

    auto inner = std::make_shared<cpp109::FileSink>("async_manual.log");
    auto async_manual = std::make_shared<
        cpp109::AsyncSink<1 << 20, cpp109::OverflowPolicy::DROP_NEWEST>
    >(inner);
    logger->add_sink(async_manual);

    std::thread t1([&]() {
        for (int i = 0; i < 5000; ++i)
            logger->info("thread1: line {}", i);
    });
    std::thread t2([&]() {
        for (int i = 0; i < 5000; ++i)
            logger->info("thread2: line {}", i);
    });

    t1.join();
    t2.join();

    logger->flush();
    return 0;
}
