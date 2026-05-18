// rotating.cpp — 按大小滚动日志示例

#include "log/log.hpp"

int main() {
    auto logger = cpp109::get_logger("rotating_demo");

    auto rotating = std::make_shared<cpp109::RotatingFileSink>("rotating.log", 10, 5);
    logger->add_sink(rotating);

    for (int i = 0; i < 100000; ++i) {
        logger->info("line {}: this is a very long message to fill up the file quickly", i);
    }

    logger->flush();
    return 0;
}
