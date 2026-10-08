// test_sinks.cpp — 测试所有 Sink 实现

#include "log/log.hpp"
#include <cstdio>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "test_common.h"

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

// 回归：rotate 失败不得崩溃、不得把已关闭的 FILE* 留在 file_ 里被后续 fwrite
// 使用；失败应计入 io_error_count（而非静默）。这里用"目录占住滚动目标名"
// 稳定制造 rename 失败。
CPP109_TEST(rotating_failure_resilient)
{
    namespace fs = std::filesystem;
    const char* base = "test_rot_fail.log";
    const char* idx1 = "test_rot_fail.log.1";
    std::error_code ec;
    std::remove(base);
    fs::remove_all(idx1, ec);
    fs::create_directory(idx1, ec);   // 使 rename(base → base.1) 失败

    {
        // max_size_mb=0 → 每条日志都触发 rotate；max_files=1 → 只滚动一层
        auto sink = std::make_shared<cpp109::RotatingFileSink>(base, 0, 1);
        sink->set_level(cpp109::LogLevel::DEBUG);

        for (int i = 0; i < 5; ++i) {
            cpp109::LogEvent event(
                "test_rot_fail",
                cpp109::LogLevel::INFO,
                std::string("msg ") + std::to_string(i),
                cpp109::Timestamp(),
                cpp109::SourceLoc{__FILE__, __LINE__, __func__},
                0
            );
            sink->log(event);   // 修复前：rotate 失败后 fwrite 到已关闭/空 file_ → 崩溃
        }
        sink->flush();

        CPP109_ASSERT(sink->io_error_count() >= 1);
    }

    fs::remove_all(idx1, ec);
    std::remove(base);
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
    test_rotating_failure_resilient();
    test_callback_sink();

    if (cpp109_test::failures() == 0) {
        fprintf(stdout, "test_sinks.cpp: all tests passed\n");
    } else {
        fprintf(stderr, "test_sinks.cpp: %d failure(s)\n", cpp109_test::failures());
    }
    return CPP109_TEST_RESULT();
}
