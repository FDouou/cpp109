// basic.cpp �� �����÷�ʾ��

#include "../include/log/log.hpp"
#include <thread>

int main() {
    auto logger = cpp109::get_logger("app");
    auto console_sink = std::make_shared<cpp109::ConsoleSink>();
    logger->add_sink(console_sink);
    LOG_INFO_TO(logger, "hello {}", "world");
    LOG_WARN_TO(logger, "something might be wrong, code={}", 500);
    LOG_ERROR_TO(logger, "something went wrong");

    cpp109::Registry::instance().set_pattern("[%H:%M:%S.%f] [%l] %v");

    auto file_sink = std::make_shared<cpp109::FileSink>("app.log");
    logger->add_sink(file_sink);
    LOG_INFO_TO(logger, "this goes to both console and file");

    std::thread t([=]() {
        auto logger = cpp109::get_logger("worker");
        logger->add_sink(console_sink);
        LOG_INFO_TO(logger, "hello from worker thread");
    });
    t.join();

    return 0;
}
