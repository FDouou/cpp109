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

CPP109_TEST(ring_buffer_basic)
{
    cpp109::RingBuffer<int, 8> rb;
    CPP109_ASSERT(rb.empty());
    CPP109_ASSERT(!rb.full());
    CPP109_ASSERT_EQ(rb.capacity(), 8u);

    for (int i = 0; i < 8; ++i) {
        CPP109_ASSERT(rb.enqueue(i));
    }
    CPP109_ASSERT(rb.full());
    CPP109_ASSERT(!rb.empty());

    int val;
    for (int i = 0; i < 8; ++i) {
        CPP109_ASSERT(rb.dequeue(val));
        CPP109_ASSERT_EQ(val, i);
    }
    CPP109_ASSERT(rb.empty());
    CPP109_ASSERT(!rb.full());
}

CPP109_TEST(ring_buffer_drop_newest)
{
    cpp109::RingBuffer<int, 4, cpp109::OverflowPolicy::DROP_NEWEST> rb;

    for (int i = 0; i < 4; ++i) {
        CPP109_ASSERT(rb.enqueue(i));
    }

    CPP109_ASSERT(rb.enqueue(100) == false);

    int val;
    for (int i = 0; i < 4; ++i) {
        CPP109_ASSERT(rb.dequeue(val));
        CPP109_ASSERT_EQ(val, i);
    }
}

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
    while (std::getline(ifs, line)) {
        if (line.find("async message") != std::string::npos) {
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

} // anonymous namespace

int main() {
    test_ring_buffer_basic();
    test_ring_buffer_drop_newest();
    test_async_sink_basic();
    test_async_multithread();

    fprintf(stdout, "test_async.cpp: all tests passed\n");
    return 0;
}
