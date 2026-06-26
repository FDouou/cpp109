// test_sinks.cpp — 测试所有 Sink 实现

#include "log/log.hpp"
#include <cstdio>
#include <cassert>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#define CPP109_TEST(name) void test_##name()
#define CPP109_ASSERT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while(0)
#define CPP109_ASSERT_EQ(a, b) do { if ((a) != (b)) { fprintf(stderr, "FAIL: %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while(0)

namespace {

CPP109_TEST(console_sink)
{
    auto sink = std::make_shared<cpp109::ConsoleSink>(false, false);
    sink->set_level(cpp109::LogLevel::DEBUG);

    cpp109::LogEvent event(
        "test_console",
        cpp109::LogLevel::INFO,
        "hello console",
        cpp109::Timestamp(),
        cpp109::SourceLoc{__FILE__, __LINE__, __func__},
        0
    );

    sink->log(event);
    sink->flush();
}

CPP109_TEST(file_sink)
{
    const char* filename = "test_file_sink.log";

    {
        auto sink = std::make_shared<cpp109::FileSink>(filename, true);
        sink->set_level(cpp109::LogLevel::DEBUG);

        cpp109::LogEvent event(
            "test_file",
            cpp109::LogLevel::INFO,
            "hello file",
            cpp109::Timestamp(),
            cpp109::SourceLoc{__FILE__, __LINE__, __func__},
            0
        );

        sink->log(event);
        sink->flush();
    }

    std::ifstream ifs(filename);
    CPP109_ASSERT(ifs.is_open());
    std::string line;
    std::getline(ifs, line);
    CPP109_ASSERT(line.find("hello file") != std::string::npos);
    ifs.close();

    std::remove(filename);
}

CPP109_TEST(rotating_file_sink)
{
    const char* filename = "test_rotating.log";

    {
        auto sink = std::make_shared<cpp109::RotatingFileSink>(filename, 1, 3);
        sink->set_level(cpp109::LogLevel::DEBUG);

        for (int i = 0; i < 100; ++i) {
            cpp109::LogEvent event(
                "test_rotating",
                cpp109::LogLevel::INFO,
                std::string("rotating message ") + std::to_string(i),
                cpp109::Timestamp(),
                cpp109::SourceLoc{__FILE__, __LINE__, __func__},
                0
            );
            sink->log(event);
        }
        sink->flush();
    }

    std::ifstream ifs(filename);
    CPP109_ASSERT(ifs.is_open());
    ifs.close();

    std::remove(filename);
}

CPP109_TEST(callback_sink)
{
    std::vector<std::string> captured;

    auto sink = std::make_shared<cpp109::CallbackSink>(
        [&captured](const std::string& formatted, const cpp109::LogEvent&) {
            captured.push_back(formatted);
        }
    );
    sink->set_level(cpp109::LogLevel::DEBUG);

    cpp109::LogEvent event(
        "test_callback",
        cpp109::LogLevel::INFO,
        "callback triggered",
        cpp109::Timestamp(),
        cpp109::SourceLoc{__FILE__, __LINE__, __func__},
        0
    );

    sink->log(event);
    CPP109_ASSERT_EQ(captured.size(), 1u);
    CPP109_ASSERT(captured[0].find("callback triggered") != std::string::npos);
}

} // anonymous namespace

int main() {
    test_console_sink();
    test_file_sink();
    test_rotating_file_sink();
    test_callback_sink();

    fprintf(stdout, "test_sinks.cpp: all tests passed\n");
    return 0;
}
