// test_logger.cpp — 测试 Logger 核心功能

#include "log/log.hpp"
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>

#include "test_common.h"

namespace {

CPP109_TEST(logger_creation)
{
    auto a = cpp109::get_logger("test_creation");
    auto b = cpp109::get_logger("test_creation");
    CPP109_ASSERT(a.get() == b.get());
    CPP109_ASSERT_EQ(std::string(a->name()), "test_creation");
}

CPP109_TEST(level_filter)
{
    auto logger = cpp109::get_logger("filter_test");
    logger->set_level(cpp109::LogLevel::WARN);
    CPP109_ASSERT_EQ(logger->level(), cpp109::LogLevel::WARN);

    auto sink = std::make_shared<cpp109::ConsoleSink>(false, false);
    logger->add_sink(sink);

    CPP109_ASSERT(logger->level() > cpp109::LogLevel::DEBUG);
    CPP109_ASSERT(logger->level() > cpp109::LogLevel::INFO);
    CPP109_ASSERT(logger->level() <= cpp109::LogLevel::WARN);
    CPP109_ASSERT(logger->level() <= cpp109::LogLevel::ERROR);
}

CPP109_TEST(hierarchy)
{
    auto parent = cpp109::get_logger("parent_test");
    auto child = cpp109::get_logger("parent_test.child_test");

    auto parent_ptr = child->parent();
    CPP109_ASSERT(parent_ptr != nullptr);
    CPP109_ASSERT_EQ(std::string(parent_ptr->name()), "parent_test");
}

CPP109_TEST(propagate)
{
    auto logger = cpp109::get_logger("propagate_test");
    CPP109_ASSERT(logger->propagate() == true);

    logger->set_propagate(false);
    CPP109_ASSERT(logger->propagate() == false);

    logger->set_propagate(true);
    CPP109_ASSERT(logger->propagate() == true);
}

CPP109_TEST(sink_add_remove)
{
    auto logger = cpp109::get_logger("sink_test");
    auto sink1 = std::make_shared<cpp109::ConsoleSink>();
    auto sink2 = std::make_shared<cpp109::ConsoleSink>();

    logger->add_sink(sink1);
    logger->add_sink(sink2);
    logger->remove_sink(sink1);
    logger->clear_sinks();
}

} // anonymous namespace

int main() {
    test_logger_creation();
    test_level_filter();
    test_hierarchy();
    test_propagate();
    test_sink_add_remove();

    if (cpp109_test::failures() == 0) {
        fprintf(stdout, "test_logger.cpp: all tests passed\n");
    } else {
        fprintf(stderr, "test_logger.cpp: %d failure(s)\n", cpp109_test::failures());
    }
    return CPP109_TEST_RESULT();
}
