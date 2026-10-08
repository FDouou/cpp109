// test_async.cpp — 测试异步日志

#include "log/log.hpp"
#include <cstdio>
#include <thread>
#include <vector>
#include <cassert>
#include <fstream>
#include <string>

#include "test_common.h"

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
                    LOG_INFO_TO(logger, "thread {} msg {}", t, i);
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

        LOG_INFO_TO(logger, "int={}", 42);
        LOG_INFO_TO(logger, "literal={}", "hello literal");
        const char* cp = "const char pointer";
        LOG_INFO_TO(logger, "cptr={}", cp);
        {
            // sv / 临时 string 的源在 flush 前销毁：内容须已拷贝进 ring
            std::string keep = "kept string view content";
            std::string_view sv = keep;
            LOG_INFO_TO(logger, "sv={}", sv);
        }
        LOG_INFO_TO(logger, "tmp={}", std::string("temporary string"));
        LOG_INFO_TO(logger, "empty=[{}]", std::string_view{});

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

// 回归测试：单条记录超过 TLS 批缓冲容量（默认 16KB）必须降级为 direct
// 单条入队。曾因缺少降级导致 needed > BATCH_CAPACITY 的记录直接写 TLS
// 批缓冲数组越界（旧逻辑只 flush 不清算，仍从 offset 0 写超长负载）。
CPP109_TEST(async_large_string_fallback)
{
    const char* filename = "test_async_large_string.log";
    constexpr std::size_t kBigLen = 64 * 1024;   // 4× 默认批缓冲容量

    const std::size_t payload_max =
        cpp109::detail::BATCH_CAPACITY - sizeof(cpp109::TinyHeader) -
        sizeof(std::uint32_t);

    std::string big(kBigLen, 'A');
    big.front() = '<';
    big.back()  = '>';
    // needed == BATCH_CAPACITY 恰好占满批缓冲（不越界）；
    // +1 字节即触发降级路径。
    std::string exact(payload_max, 'E');
    std::string exact_plus1(payload_max + 1, 'F');
    // 超过 ring 容量（默认 1MB）的记录按策略丢弃，不得死等（BLOCK 策略）
    std::string huge((1u << 20) + 1024, 'H');

    {
        auto logger = std::make_shared<cpp109::Logger>("large_string");
        auto async = std::make_shared<cpp109::AsyncSink<>>(
            std::make_shared<cpp109::FileSink>(filename, true));
        logger->add_sink(async);
        logger->set_level(cpp109::LogLevel::TRACE);

        LOG_INFO_TO(logger, "small-before={}", 1);
        LOG_INFO_TO(logger, "big={}", big);
        LOG_INFO_TO(logger, "exact={}", exact);
        LOG_INFO_TO(logger, "exact_plus1={}", exact_plus1);
        LOG_INFO_TO(logger, "huge={}", huge);
        LOG_INFO_TO(logger, "small-after={}", 2);

        logger->flush();
    }

    std::ifstream ifs(filename, std::ios::binary);
    CPP109_ASSERT(ifs.is_open());
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    ifs.close();

    CPP109_ASSERT(content.find("small-before=1") != std::string::npos);
    CPP109_ASSERT(content.find("big=" + big) != std::string::npos);       // 未截断
    CPP109_ASSERT(content.find("exact=" + exact) != std::string::npos);   // 填满批
    CPP109_ASSERT(content.find("exact_plus1=" + exact_plus1) != std::string::npos);
    CPP109_ASSERT(content.find("huge=") == std::string::npos);            // 超 ring 丢弃
    CPP109_ASSERT(content.find("small-after=2") != std::string::npos);    // 降级后批次状态正常

    std::remove(filename);
}

// 回归：同一线程交替使用两个各绑独立 AsyncSink 的 logger，不得串扰。
// 这是"约束②"（一线程一批周期内只用一个 async logger）被违反时的兜底：
// 换 sink 前必须先用旧 sink 提交旧数据，否则先写者的记录会落进后写者的文件。
CPP109_TEST(async_multi_logger_no_crosstalk)
{
    const char* fa = "test_crosstalk_a.log";
    const char* fb = "test_crosstalk_b.log";
    std::remove(fa); std::remove(fb);

    {
        // logger 名不含 '.' → 无 parent → 走异步快路径（batch）
        auto la = std::make_shared<cpp109::Logger>("net");
        auto lb = std::make_shared<cpp109::Logger>("db");
        auto sa = std::make_shared<cpp109::AsyncSink<>>(
            std::make_shared<cpp109::FileSink>(fa, true));
        auto sb = std::make_shared<cpp109::AsyncSink<>>(
            std::make_shared<cpp109::FileSink>(fb, true));
        la->add_sink(sa);
        lb->add_sink(sb);
        la->set_level(cpp109::LogLevel::TRACE);
        lb->set_level(cpp109::LogLevel::TRACE);

        // 交替写，确保落在同一个 batch 周期内（总量远小于 16KB 批容量）
        for (int i = 0; i < 20; ++i) {
            LOG_INFO_TO(la, "AAA-{}", i);
            LOG_INFO_TO(lb, "BBB-{}", i);
        }
        la->flush();
        lb->flush();
    }

    auto read_all = [](const char* p) {
        std::ifstream f(p);
        return std::string((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    };
    const std::string ca = read_all(fa);
    const std::string cb = read_all(fb);

    CPP109_ASSERT(ca.find("AAA-") != std::string::npos);
    CPP109_ASSERT(ca.find("BBB-") == std::string::npos);   // A 文件不得含 B
    CPP109_ASSERT(cb.find("BBB-") != std::string::npos);
    CPP109_ASSERT(cb.find("AAA-") == std::string::npos);   // B 文件不得含 A

    std::remove(fa); std::remove(fb);
}

// 回归：单条记录超过 ring 容量 → dropped_oversize 计数递增，且不崩溃、不静默。
CPP109_TEST(async_drop_stats)
{
    const char* fn = "test_drop_stats.log";
    std::remove(fn);
    {
        auto logger = std::make_shared<cpp109::Logger>("dropstats");
        auto async = std::make_shared<cpp109::AsyncSink<1024>>(   // 1KB ring
            std::make_shared<cpp109::FileSink>(fn, true));
        logger->add_sink(async);
        logger->set_level(cpp109::LogLevel::TRACE);

        std::string big(2048, 'X');                 // 单条 > ring 容量
        LOG_INFO_TO(logger, "big={}", big);         // 触发 dropped_oversize
        LOG_INFO_TO(logger, "small-ok");            // 正常一条
        logger->flush();

        const auto st = async->stats();
        CPP109_ASSERT(st.dropped_oversize >= 1);
        CPP109_ASSERT(st.dropped_ring_full == 0);
    }
    std::remove(fn);
}

// 回归：异步快路径也要输出 logger 名（%n）。此前 worker 解码时 logger_name
// 传空串，%n 恒为空；现由调用点 TinyMeta 惰性携带。
CPP109_TEST(async_logger_name)
{
    const char* fn = "test_async_name.log";
    std::remove(fn);
    {
        auto logger = std::make_shared<cpp109::Logger>("myname");   // 名不含 '.' → 快路径
        auto async = std::make_shared<cpp109::AsyncSink<>>(
            std::make_shared<cpp109::FileSink>(fn, true));
        logger->add_sink(async);
        logger->set_level(cpp109::LogLevel::TRACE);
        async->set_pattern("[%n] %v");   // 转发到内部 FileSink 的 formatter

        LOG_INFO_TO(logger, "hello-name");
        logger->flush();
    }
    std::ifstream ifs(fn);
    const std::string content((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());
    CPP109_ASSERT(content.find("[myname]") != std::string::npos);
    CPP109_ASSERT(content.find("hello-name") != std::string::npos);
    std::remove(fn);
}

// 回归：线程退出后其分片应被回收（stats().shards 降回 0）。
// 机制：~BatchBuffer（线程退出）→ retire_shard → worker drain 后 reap。
CPP109_TEST(async_shard_reclaim)
{
    const char* fn = "test_reclaim.log";
    std::remove(fn);
    auto async = std::make_shared<cpp109::AsyncSink<>>(
        std::make_shared<cpp109::FileSink>(fn, true));

    // 主线程不写该 sink，分片只属于子线程
    {
        std::thread t([async]() {
            auto lg = std::make_shared<cpp109::Logger>("reclaim");   // 名不含 '.' → 快路径
            lg->add_sink(async);
            lg->set_level(cpp109::LogLevel::TRACE);
            LOG_INFO_TO(lg, "from ephemeral thread");
            lg->flush();
        });
        t.join();   // 子线程退出：flush batch + retire_shard(tid)
    }

    // 触发 worker 排空 + 回收（flush → drain_one → reap_retired）
    async->flush();
    const auto st = async->stats();
    CPP109_ASSERT(st.shards == 0);   // 子线程分片已回收

    std::remove(fn);
}

// 压测：多轮短命线程共享同一 logger/AsyncSink 频繁起停，验证分片回收在并发下
// 正确（不崩溃），且分片数不随线程起停总数线性增长。
CPP109_TEST(async_shard_reclaim_stress)
{
    const char* fn = "test_reclaim_stress.log";
    std::remove(fn);
    auto logger = std::make_shared<cpp109::Logger>("stress");   // 共享，名不含 '.'
    auto async = std::make_shared<cpp109::AsyncSink<>>(
        std::make_shared<cpp109::FileSink>(fn, true));
    logger->add_sink(async);
    logger->set_level(cpp109::LogLevel::TRACE);

    constexpr int kRounds = 2, kThreads = 3;   // 累计 6 个短命线程
    for (int round = 0; round < kRounds; ++round) {
        std::vector<std::thread> ts;
        for (int i = 0; i < kThreads; ++i)
            ts.emplace_back([logger, round, i]() {
                LOG_INFO_TO(logger, "r{} t{}", round, i);   // 退出靠 TLS 析构 flush + retire
            });
        for (auto& t : ts) t.join();
        async->flush();   // 触发 drain + reap_retired
    }

    const auto st = async->stats();
    // 回收有效则分片数停留在峰值并发放量级，而非累计 48
    CPP109_ASSERT(st.shards <= static_cast<std::size_t>(kThreads) * 2);

    std::remove(fn);
}

} // anonymous namespace

int main() {
    test_async_sink_basic();
    test_async_multithread();
    test_async_codec_string_family();
    test_async_large_string_fallback();
    test_async_multi_logger_no_crosstalk();
    test_async_drop_stats();
    test_async_logger_name();
    test_async_shard_reclaim();
    test_async_shard_reclaim_stress();

    if (cpp109_test::failures() == 0) {
        fprintf(stdout, "test_async.cpp: all tests passed\n");
    } else {
        fprintf(stderr, "test_async.cpp: %d failure(s)\n", cpp109_test::failures());
    }
    return CPP109_TEST_RESULT();
}
