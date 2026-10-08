// test_config.cpp — 测试配置系统

#include "log/log.hpp"
#include <cstdio>
#include <cassert>

#include "test_common.h"

namespace {

CPP109_TEST(config_builder)
{
    cpp109::Config cfg;
    cfg.add_sink("console")
       .set_class<cpp109::ConsoleSink>()
       .set_level(cpp109::LogLevel::DEBUG);
    cfg.add_logger("config_test")
       .set_sinks({"console"})
       .set_level(cpp109::LogLevel::DEBUG);
    cfg.apply();

    auto logger = cpp109::get_logger("config_test");
    CPP109_ASSERT_EQ(logger->level(), cpp109::LogLevel::DEBUG);
}

CPP109_TEST(config_propagate)
{
    cpp109::Config cfg;
    cfg.add_sink("console2").set_class<cpp109::ConsoleSink>();
    cfg.add_logger("prop_cfg").set_sinks({"console2"}).set_propagate(false);
    cfg.apply();

    auto logger = cpp109::get_logger("prop_cfg");
    CPP109_ASSERT(logger->propagate() == false);
}

CPP109_TEST(config_multiple_sinks)
{
    cpp109::Config cfg;
    cfg.add_sink("console3").set_class<cpp109::ConsoleSink>().set_level(cpp109::LogLevel::INFO);
    cfg.add_sink("file3").set_class<cpp109::FileSink>().set_property("filename", "test_config.log").set_level(cpp109::LogLevel::DEBUG);
    cfg.add_logger("multi_test")
       .set_sinks({"console3", "file3"})
       .set_level(cpp109::LogLevel::DEBUG);
    cfg.apply();

    auto logger = cpp109::get_logger("multi_test");
    CPP109_ASSERT_EQ(logger->level(), cpp109::LogLevel::DEBUG);
}

} // anonymous namespace

int main() {
    test_config_builder();
    test_config_propagate();
    test_config_multiple_sinks();

    if (cpp109_test::failures() == 0) {
        fprintf(stdout, "test_config.cpp: all tests passed\n");
    } else {
        fprintf(stderr, "test_config.cpp: %d failure(s)\n", cpp109_test::failures());
    }
    return CPP109_TEST_RESULT();
}
