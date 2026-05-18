// ==========================================================================
// custom_formatter.cpp — 自定义格式化器示例
//
// 演示自定义日志输出格式
// ==========================================================================

#include "log/log.hpp"

int main() {
    auto logger = cpp109::get_logger("formatter_demo");

    // 修改全局格式
    cpp109::Registry::instance().set_pattern("[%H:%M:%S] [%l] [%n] %v");

    logger->info("custom format test");
    logger->warn("another test");

    // 也可以为单个 Sink 设置独立格式
    auto custom_formatter = std::make_unique<cpp109::Formatter>("%v  ←  at %g:%#");
    auto console = std::make_shared<cpp109::ConsoleSink>();
    console->set_formatter(std::move(custom_formatter));
    logger->add_sink(console);

    logger->info("this has a special format");

    return 0;
}
