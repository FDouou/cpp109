// file_output.cpp — 文件输出示例

#include "log/log.hpp"

int main() {
    auto logger = cpp109::get_logger("file_demo");

    auto file_sink = std::make_shared<cpp109::FileSink>("output.log");
    logger->add_sink(file_sink);

    for (int i = 0; i < 10; ++i) {
        logger->info("line {}", i);
    }

    logger->flush();
    return 0;
}
