// test_level.cpp — 测试日志级别枚举与转换

#include "log/log.hpp"
#include <cassert>
#include <cstdio>

#include "test_common.h"

namespace {

CPP109_TEST(level_to_string_caps)
{
    CPP109_ASSERT_EQ(std::string(cpp109::level_to_string_caps(cpp109::LogLevel::TRACE)), "TRACE");
    CPP109_ASSERT_EQ(std::string(cpp109::level_to_string_caps(cpp109::LogLevel::DEBUG)), "DEBUG");
    CPP109_ASSERT_EQ(std::string(cpp109::level_to_string_caps(cpp109::LogLevel::INFO)),  "INFO");
    CPP109_ASSERT_EQ(std::string(cpp109::level_to_string_caps(cpp109::LogLevel::WARN)),  "WARN");
    CPP109_ASSERT_EQ(std::string(cpp109::level_to_string_caps(cpp109::LogLevel::ERROR)), "ERROR");
    CPP109_ASSERT_EQ(std::string(cpp109::level_to_string_caps(cpp109::LogLevel::FATAL)), "FATAL");
}

CPP109_TEST(level_to_string)
{
    CPP109_ASSERT_EQ(std::string(cpp109::level_to_string(cpp109::LogLevel::TRACE)), "trace");
    CPP109_ASSERT_EQ(std::string(cpp109::level_to_string(cpp109::LogLevel::DEBUG)), "debug");
    CPP109_ASSERT_EQ(std::string(cpp109::level_to_string(cpp109::LogLevel::INFO)),  "info");
    CPP109_ASSERT_EQ(std::string(cpp109::level_to_string(cpp109::LogLevel::WARN)),  "warn");
    CPP109_ASSERT_EQ(std::string(cpp109::level_to_string(cpp109::LogLevel::ERROR)), "error");
    CPP109_ASSERT_EQ(std::string(cpp109::level_to_string(cpp109::LogLevel::FATAL)), "fatal");
}

CPP109_TEST(level_from_string)
{
    CPP109_ASSERT_EQ(cpp109::level_from_string("trace"), cpp109::LogLevel::TRACE);
    CPP109_ASSERT_EQ(cpp109::level_from_string("DEBUG"), cpp109::LogLevel::DEBUG);
    CPP109_ASSERT_EQ(cpp109::level_from_string("Info"),  cpp109::LogLevel::INFO);
    CPP109_ASSERT_EQ(cpp109::level_from_string("WARN"),  cpp109::LogLevel::WARN);
    CPP109_ASSERT_EQ(cpp109::level_from_string("error"), cpp109::LogLevel::ERROR);
    CPP109_ASSERT_EQ(cpp109::level_from_string("fatal"), cpp109::LogLevel::FATAL);
    CPP109_ASSERT_EQ(cpp109::level_from_string("unknown"), cpp109::LogLevel::OFF);
}

CPP109_TEST(level_comparison)
{
    CPP109_ASSERT(cpp109::LogLevel::TRACE < cpp109::LogLevel::DEBUG);
    CPP109_ASSERT(cpp109::LogLevel::DEBUG < cpp109::LogLevel::INFO);
    CPP109_ASSERT(cpp109::LogLevel::INFO  < cpp109::LogLevel::WARN);
    CPP109_ASSERT(cpp109::LogLevel::WARN  < cpp109::LogLevel::ERROR);
    CPP109_ASSERT(cpp109::LogLevel::ERROR < cpp109::LogLevel::FATAL);
    CPP109_ASSERT(cpp109::LogLevel::FATAL < cpp109::LogLevel::OFF);
}

} // anonymous namespace

int main() {
    test_level_to_string_caps();
    test_level_to_string();
    test_level_from_string();
    test_level_comparison();

    if (cpp109_test::failures() == 0) {
        fprintf(stdout, "test_level.cpp: all tests passed\n");
    } else {
        fprintf(stderr, "test_level.cpp: %d failure(s)\n", cpp109_test::failures());
    }
    return CPP109_TEST_RESULT();
}
