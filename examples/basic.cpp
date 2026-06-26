// basic.cpp — 基础用法示例

#include "../include/log/log.hpp"
#include <thread>

int main() {
    auto logger = cpp109::get_logger("app");
    auto console_sink = std::make_shared<cpp109::ConsoleSink>();
    logger->add_sink(console_sink);
    logger->info("hello {}", "world");
    logger->warn("something might be wrong, code={}", 500);
    logger->error("something went wrong");

    cpp109::Registry::instance().set_pattern("[%H:%M:%S.%f] [%l] %v");

    auto file_sink = std::make_shared<cpp109::FileSink>("app.log");
    logger->add_sink(file_sink);
    logger->info("this goes to both console and file");

    std::thread t([=]() {
        auto logger = cpp109::get_logger("worker");
        logger->add_sink(console_sink);
        logger->info("hello from worker thread");
    });
    t.join();

    return 0;
}
