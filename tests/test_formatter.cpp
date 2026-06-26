// test_formatter.cpp — 测试格式化器

#include "log/log.hpp"
#include <cstdio>
#include <cassert>
#include <string>

#define CPP109_TEST(name) void test_##name()
#define CPP109_ASSERT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while(0)
#define CPP109_ASSERT_EQ(a, b) do { if ((a) != (b)) { fprintf(stderr, "FAIL: %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while(0)

namespace {

CPP109_TEST(default_pattern)
{
    cpp109::Formatter fmt;
    cpp109::LogEvent event(
        "test_formatter",
        cpp109::LogLevel::INFO,
        "hello formatter",
        cpp109::Timestamp(),
        cpp109::SourceLoc{__FILE__, __LINE__, __func__},
        12345
    );

    std::string output;
    fmt.format(event, output);

    CPP109_ASSERT(output.find("hello formatter") != std::string::npos);
    CPP109_ASSERT(output.find("INFO") != std::string::npos);
    CPP109_ASSERT(output.find("test_formatter") != std::string::npos);
}

CPP109_TEST(custom_pattern)
{
    cpp109::Formatter fmt("[%L] %v");
    cpp109::LogEvent event(
        "test_custom",
        cpp109::LogLevel::WARN,
        "custom pattern test",
        cpp109::Timestamp(),
        cpp109::SourceLoc{__FILE__, __LINE__, __func__},
        0
    );

    std::string output;
    fmt.format(event, output);

    CPP109_ASSERT(output.find("[WARN]") != std::string::npos);
    CPP109_ASSERT(output.find("custom pattern test") != std::string::npos);
}

CPP109_TEST(custom_pattern_level_lowercase)
{
    cpp109::Formatter fmt("%l %v");
    cpp109::LogEvent event(
        "test_lower",
        cpp109::LogLevel::ERROR,
        "lowercase level",
        cpp109::Timestamp(),
        cpp109::SourceLoc{__FILE__, __LINE__, __func__},
        0
    );

    std::string output;
    fmt.format(event, output);

    CPP109_ASSERT(output.find("error") != std::string::npos);
    CPP109_ASSERT(output.find("lowercase level") != std::string::npos);
}

CPP109_TEST(escape_percent)
{
    cpp109::Formatter fmt("%% %v");
    cpp109::LogEvent event(
        "test_escape",
        cpp109::LogLevel::INFO,
        "percent test",
        cpp109::Timestamp(),
        cpp109::SourceLoc{__FILE__, __LINE__, __func__},
        0
    );

    std::string output;
    fmt.format(event, output);

    CPP109_ASSERT(output.find("% percent test") != std::string::npos);
}

CPP109_TEST(colored_formatter)
{
    auto fmt = cpp109::Formatter::colored("[%L] %v");
    cpp109::LogEvent event(
        "test_colored",
        cpp109::LogLevel::ERROR,
        "colored output",
        cpp109::Timestamp(),
        cpp109::SourceLoc{__FILE__, __LINE__, __func__},
        0
    );

    std::string output;
    fmt->format(event, output);

    CPP109_ASSERT(output.find("[ERROR]") != std::string::npos);
    CPP109_ASSERT(output.find("colored output") != std::string::npos);
}

} // anonymous namespace

int main() {
    test_default_pattern();
    test_custom_pattern();
    test_custom_pattern_level_lowercase();
    test_escape_percent();
    test_colored_formatter();

    fprintf(stdout, "test_formatter.cpp: all tests passed\n");
    return 0;
}
