// test_callsite.cpp — 调用点元数据（file/line/func/fmt）正确性回归
//
// 历史缺陷：快路径曾把 TinyMeta 做成「每个模板实例一份 static」，导致同一
// 参数类型的多个调用点共享第一次调用的 file/line/fmt。本测试确保：
//   1. LOG_*_TO 宏在调用点生成 static TinyMeta，独立且正确
//   2. LOG_* 宏（默认 logger）同样正确
//   3. 慢路径（多 sink）分发的 LogEvent 也携带正确位置
//   4. 相同参数类型组合、不同调用点、不同格式串互不串扰

#include "log/log.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

struct Captured {
    std::string msg;
    std::string file;
    int         line = 0;
    std::string func;
};

static std::vector<Captured> g_cap;

static std::shared_ptr<cpp109::CallbackSink> make_capture_sink() {
    return std::make_shared<cpp109::CallbackSink>(
        [](const std::string& msg, const cpp109::LogEvent& ev) {
            g_cap.push_back(Captured{msg, std::string(ev.file()), ev.line(),
                                     std::string(ev.func())});
        });
}

static bool has_file(const Captured& c) {
    return c.file.find("test_callsite.cpp") != std::string::npos;
}

static bool has_msg(const Captured& c, const char* text) {
    return c.msg.find(text) != std::string::npos;
}

// ── LOG_*_TO 宏：同一模板实例（int 参数）多个调用点 ──
static void to_macro_callsite() {
    g_cap.clear();
    auto logger = cpp109::get_logger("callsite_to");
    logger->clear_sinks();
    logger->add_sink(std::make_shared<cpp109::AsyncSink<1024>>(make_capture_sink()));

    const int line_a = __LINE__ + 1;
    LOG_INFO_TO(logger, "to_a {}", 1);
    const int line_b = __LINE__ + 1;
    LOG_INFO_TO(logger, "to_b {}", 2);
    const int line_c = __LINE__ + 1;
    LOG_INFO_TO(logger, "to_noargs");

    logger->flush();

    CHECK(g_cap.size() == 3);
    if (g_cap.size() == 3) {
        CHECK(has_msg(g_cap[0], "to_a 1"));
        CHECK(has_msg(g_cap[1], "to_b 2"));
        CHECK(has_msg(g_cap[2], "to_noargs"));
        CHECK(g_cap[0].line == line_a);
        CHECK(g_cap[1].line == line_b);
        CHECK(g_cap[2].line == line_c);
        CHECK(has_file(g_cap[0]) && has_file(g_cap[1]) && has_file(g_cap[2]));
        CHECK(g_cap[0].func.find("to_macro_callsite") != std::string::npos);
    }
}

// ── LOG_* 宏：static TinyMeta 生成于本文件调用点 ──
static void macro_callsite() {
    g_cap.clear();
    auto logger = cpp109::Registry::instance().default_logger();
    logger->clear_sinks();
    logger->add_sink(std::make_shared<cpp109::AsyncSink<1024>>(make_capture_sink()));

    const int line_a = __LINE__ + 1;
    LOG_INFO("macro_a {}", 1);
    const int line_b = __LINE__ + 1;
    LOG_INFO("macro_b {}", 2);
    const int line_c = __LINE__ + 1;
    LOG_INFO("macro_noargs");

    logger->flush();

    CHECK(g_cap.size() == 3);
    if (g_cap.size() == 3) {
        CHECK(has_msg(g_cap[0], "macro_a 1"));
        CHECK(has_msg(g_cap[1], "macro_b 2"));
        CHECK(has_msg(g_cap[2], "macro_noargs"));
        CHECK(g_cap[0].line == line_a);
        CHECK(g_cap[1].line == line_b);
        CHECK(g_cap[2].line == line_c);
        CHECK(has_file(g_cap[0]) && has_file(g_cap[1]) && has_file(g_cap[2]));
    }
}

// ── 慢路径：多 sink 时 log_impl 分发，LogEvent 位置来自调用点 ──
static void slow_path_callsite() {
    g_cap.clear();
    auto logger = cpp109::get_logger("callsite_slow");
    logger->clear_sinks();
    logger->add_sink(make_capture_sink());
    logger->add_sink(make_capture_sink());   // 两个 sink → 走慢路径

    const int line_a = __LINE__ + 1;
    LOG_INFO_TO(logger, "slow_a {}", 1);
    const int line_b = __LINE__ + 1;
    LOG_INFO_TO(logger, "slow_b {}", 2);

    logger->flush();

    CHECK(g_cap.size() == 4);
    if (g_cap.size() == 4) {
        CHECK(has_msg(g_cap[0], "slow_a 1"));
        CHECK(has_msg(g_cap[1], "slow_a 1"));
        CHECK(has_msg(g_cap[2], "slow_b 2"));
        CHECK(has_msg(g_cap[3], "slow_b 2"));
        CHECK(g_cap[0].line == line_a && g_cap[2].line == line_b);
        CHECK(has_file(g_cap[0]) && has_file(g_cap[2]));
    }
}

// ── 同步单 sink 快路径（LogEvent + SBO）位置同样正确 ──
static void sync_fast_path_callsite() {
    g_cap.clear();
    auto logger = cpp109::get_logger("callsite_sync");
    logger->clear_sinks();
    logger->add_sink(make_capture_sink());

    const int line_a = __LINE__ + 1;
    LOG_INFO_TO(logger, "sync_a {}", 7);

    logger->flush();

    CHECK(g_cap.size() == 1);
    if (g_cap.size() == 1) {
        CHECK(has_msg(g_cap[0], "sync_a 7"));
        CHECK(g_cap[0].line == line_a);
        CHECK(has_file(g_cap[0]));
    }
}

// ── 无 sink：丢弃但不崩溃；空 logger（Release）安全跳过 ──
static void no_sink_drops_safely() {
    auto logger = std::make_shared<cpp109::Logger>("no_sink");
    // 无 sink：走慢路径，一次性 stderr 警告，不崩溃、不影响后续
    LOG_INFO_TO(logger, "dropped {}", 1);
    LOG_INFO_TO(logger, "dropped {}", 2);
    logger->flush();

#ifdef NDEBUG
    // Release：空 logger 整条日志安全跳过（Debug 下 assert 报错，故不在此测）
    cpp109::Logger* null_logger = nullptr;
    LOG_INFO_TO(null_logger, "dropped {}", 3);
#endif
}

int main() {
    to_macro_callsite();
    macro_callsite();
    slow_path_callsite();
    sync_fast_path_callsite();
    no_sink_drops_safely();

    cpp109::LogBackend::instance().stop();

    if (g_failures == 0) {
        fprintf(stdout, "test_callsite.cpp: all tests passed\n");
        return 0;
    }
    fprintf(stderr, "test_callsite.cpp: %d failure(s)\n", g_failures);
    return 1;
}
