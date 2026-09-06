// test_async.cpp — 测试异步日志

#include "log/log.hpp"
#include <cstdio>
#include <thread>
#include <vector>
#include <cassert>
#include <fstream>
#include <string>

#define CPP109_TEST(name) void test_##name()
#define CPP109_ASSERT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while(0)
#define CPP109_ASSERT_EQ(a, b) do { if ((a) != (b)) { fprintf(stderr, "FAIL: %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while(0)

namespace {

CPP109_TEST(async_sink_basic)
{
    const char* filename = "test_async_basic.log";

    {
        auto file = std::make_shared<cpp109::FileSink>(filename, true);
        auto async = std::make_shared<cpp109::AsyncSink<>>(file);

        for (int i = 0; i < 50; ++i) {
            cpp109::LogEvent event(
                "async_test",
                cpp109::LogLevel::INFO,
                std::string("async message ") + std::to_string(i),
                cpp109::Timestamp(),
                cpp109::SourceLoc{__FILE__, __LINE__, __func__},
                0
            );
            async->log(event);
        }

        async->stop();
    }

    std::ifstream ifs(filename);
    CPP109_ASSERT(ifs.is_open());
    std::string line;
    int count = 0;
    // 慢路径 AsyncSink::log() 不编码消息体，输出只有 header（[INFO] 标记）
    while (std::getline(ifs, line)) {
        if (line.find("[INFO]") != std::string::npos) {
            count++;
        }
    }
    CPP109_ASSERT_EQ(count, 50);
    ifs.close();

    std::remove(filename);
}

CPP109_TEST(async_multithread)
{
    const char* filename = "test_async_multi.log";

    {
        auto file = std::make_shared<cpp109::FileSink>(filename, true);
        auto async = std::make_shared<cpp109::AsyncSink<>>(file);

        auto logger = cpp109::get_logger("async_multi");
        logger->add_sink(async);
        logger->set_level(cpp109::LogLevel::INFO);

        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([logger, t]() {
                for (int i = 0; i < 25; ++i) {
                    logger->info("thread {} msg {}", t, i);
                }
                // 提交本线程 thread_local batch 缓冲，确保日志进入 ring
                logger->flush();
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        async->stop();
    }

    std::ifstream ifs(filename);
    CPP109_ASSERT(ifs.is_open());
    std::string line;
    int count = 0;
    int total_lines = 0;
    while (std::getline(ifs, line)) {
        total_lines++;
        if (line.find("thread ") != std::string::npos && line.find(" msg ") != std::string::npos) {
            count++;
        }
    }
    fprintf(stdout, "  total_lines=%d, async_multi_count=%d\n", total_lines, count);
    CPP109_ASSERT_EQ(count, 100);
    ifs.close();

    std::remove(filename);
}

// 回归测试：codec 字符串族必须按"长度+内容"打包（async 快路径）。
// 曾因先于 trivially_copyable 判断缺失导致：
//   1. 字符串字面量参数被 memcpy 内容前 8 字节，解码端当指针解引用 → 段错误
//   2. const char*/string_view 浅拷贝指针，源析构后消息悬垂
CPP109_TEST(async_codec_string_family)
{
    const char* filename = "test_async_codec_string.log";

    {
        auto logger = std::make_shared<cpp109::Logger>("codec_string");
        auto async = std::make_shared<cpp109::AsyncSink<>>(
            std::make_shared<cpp109::FileSink>(filename, true));

        logger->add_sink(async);
        logger->set_level(cpp109::LogLevel::TRACE);

        logger->info("int={}", 42);
        logger->info("literal={}", "hello literal");
        const char* cp = "const char pointer";
        logger->info("cptr={}", cp);
        {
            // sv / 临时 string 的源在 flush 前销毁：内容须已拷贝进 ring
            std::string keep = "kept string view content";
            std::string_view sv = keep;
            logger->info("sv={}", sv);
        }
        logger->info("tmp={}", std::string("temporary string"));
        logger->info("empty=[{}]", std::string_view{});

        logger->flush();
    }

    std::ifstream ifs(filename);
    CPP109_ASSERT(ifs.is_open());
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    ifs.close();

    CPP109_ASSERT(content.find("int=42") != std::string::npos);
    CPP109_ASSERT(content.find("literal=hello literal") != std::string::npos);
    CPP109_ASSERT(content.find("cptr=const char pointer") != std::string::npos);
    CPP109_ASSERT(content.find("sv=kept string view content") != std::string::npos);
    CPP109_ASSERT(content.find("tmp=temporary string") != std::string::npos);
    CPP109_ASSERT(content.find("empty=[]") != std::string::npos);

    std::remove(filename);
}

} // anonymous namespace

int main() {
    test_async_sink_basic();
    test_async_multithread();
    test_async_codec_string_family();

    fprintf(stdout, "test_async.cpp: all tests passed\n");
    return 0;
}
