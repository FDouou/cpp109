#include "log/log.hpp"
#include <cstdio>
#include <string>
#include <vector>

#define CPP109_TEST(name) void test_##name()
#define CPP109_ASSERT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while(0)
#define CPP109_ASSERT_EQ(a, b) do { if ((a) != (b)) { fprintf(stderr, "FAIL: %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while(0)

namespace {

struct Capture {
    std::vector<std::string> msgs;
    std::vector<cpp109::LogLevel> lvls;

    void reset() { msgs.clear(); lvls.clear(); }
    size_t size() const { return msgs.size(); }

    bool has_msg(const std::string& s) const {
        for (const auto& m : msgs) {
            if (m.find(s) != std::string::npos) return true;
        }
        return false;
    }

    bool has_level(cpp109::LogLevel lv) const {
        for (const auto& l : lvls) {
            if (l == lv) return true;
        }
        return false;
    }

    int count_msg(const std::string& s) const {
        int n = 0;
        for (const auto& m : msgs) {
            if (m.find(s) != std::string::npos) ++n;
        }
        return n;
    }
};

struct CapSink : cpp109::Sink {
    Capture& cap;
    explicit CapSink(Capture& c) : cap(c) {}
protected:
    void write(const std::string&, const cpp109::LogEvent& event) override {
        cap.msgs.push_back(std::string(event.message()));
        cap.lvls.push_back(event.level());
    }
};

std::shared_ptr<cpp109::Logger> setup_capture(Capture& cap) {
    auto logger = cpp109::Registry::instance().default_logger();
    logger->clear_sinks();
    auto sink = std::make_shared<CapSink>(cap);
    sink->set_level(cpp109::LogLevel::TRACE);
    logger->add_sink(sink);
    return logger;
}

} // anonymous namespace

CPP109_TEST(basic_macros_all_levels)
{
    Capture cap;
    auto logger = setup_capture(cap);
    logger->set_level(cpp109::LogLevel::TRACE);

    LOG_TRACE("trace_msg");
    LOG_DEBUG("debug_msg");
    LOG_INFO("info_msg");
    LOG_WARN("warn_msg");
    LOG_ERROR("error_msg");

    CPP109_ASSERT(cap.has_msg("trace_msg"));
    CPP109_ASSERT(cap.has_msg("debug_msg"));
    CPP109_ASSERT(cap.has_msg("info_msg"));
    CPP109_ASSERT(cap.has_msg("warn_msg"));
    CPP109_ASSERT(cap.has_msg("error_msg"));
    CPP109_ASSERT(cap.has_level(cpp109::LogLevel::TRACE));
    CPP109_ASSERT(cap.has_level(cpp109::LogLevel::DEBUG));
    CPP109_ASSERT(cap.has_level(cpp109::LogLevel::INFO));
    CPP109_ASSERT(cap.has_level(cpp109::LogLevel::WARN));
    CPP109_ASSERT(cap.has_level(cpp109::LogLevel::ERROR));
}

CPP109_TEST(level_filtering_in_macro)
{
    Capture cap;
    auto logger = setup_capture(cap);
    logger->set_level(cpp109::LogLevel::WARN);

    LOG_TRACE("no_trace");
    LOG_DEBUG("no_debug");
    LOG_INFO("no_info");
    LOG_WARN("yes_warn");
    LOG_ERROR("yes_error");

    CPP109_ASSERT(!cap.has_msg("no_trace"));
    CPP109_ASSERT(!cap.has_msg("no_debug"));
    CPP109_ASSERT(!cap.has_msg("no_info"));
    CPP109_ASSERT(cap.has_msg("yes_warn"));
    CPP109_ASSERT(cap.has_msg("yes_error"));
    CPP109_ASSERT(cap.has_level(cpp109::LogLevel::WARN));
    CPP109_ASSERT(cap.has_level(cpp109::LogLevel::ERROR));
}

CPP109_TEST(macro_format_args)
{
    Capture cap;
    auto logger = setup_capture(cap);
    logger->set_level(cpp109::LogLevel::TRACE);

    LOG_INFO("int:{}", 42);
    LOG_INFO("str:{}", "hello");
    LOG_INFO("multi:{} and {}", 1, 3.14);

    CPP109_ASSERT(cap.has_msg("int:42"));
    CPP109_ASSERT(cap.has_msg("str:hello"));
    CPP109_ASSERT(cap.has_msg("multi:1 and 3.14"));
}

CPP109_TEST(macro_no_args)
{
    Capture cap;
    auto logger = setup_capture(cap);
    logger->set_level(cpp109::LogLevel::TRACE);

    LOG_INFO("no args at all");

    CPP109_ASSERT(cap.has_msg("no args at all"));
}

CPP109_TEST(info_if_true_and_false)
{
    Capture cap;
    auto logger = setup_capture(cap);
    logger->set_level(cpp109::LogLevel::TRACE);

    LOG_INFO_IF(true, "if_true");
    LOG_INFO_IF(false, "if_false");

    CPP109_ASSERT(cap.has_msg("if_true"));
    CPP109_ASSERT(!cap.has_msg("if_false"));
}

CPP109_TEST(warn_if_with_expression)
{
    Capture cap;
    auto logger = setup_capture(cap);
    logger->set_level(cpp109::LogLevel::TRACE);

    int x = 10;
    LOG_WARN_IF(x > 5, "x_too_big x={}", x);
    LOG_WARN_IF(x > 20, "x_very_big");

    CPP109_ASSERT(cap.has_msg("x_too_big x=10"));
    CPP109_ASSERT(!cap.has_msg("x_very_big"));
}

CPP109_TEST(error_if_with_expression)
{
    Capture cap;
    auto logger = setup_capture(cap);
    logger->set_level(cpp109::LogLevel::TRACE);

    LOG_ERROR_IF(1 + 1 == 2, "math_works");
    LOG_ERROR_IF(1 + 1 == 3, "math_broken");

    CPP109_ASSERT(cap.has_msg("math_works"));
    CPP109_ASSERT(!cap.has_msg("math_broken"));
}

CPP109_TEST(info_every_n)
{
    Capture cap;
    auto logger = setup_capture(cap);
    logger->set_level(cpp109::LogLevel::TRACE);

    for (int i = 0; i < 9; ++i) {
        LOG_INFO_EVERY_N(3, "every_3");
    }
    CPP109_ASSERT_EQ((int)cap.size(), 3);

    cap.reset();

    for (int i = 0; i < 5; ++i) {
        LOG_INFO_EVERY_N(2, "every_2");
    }
    CPP109_ASSERT_EQ((int)cap.size(), 3);
}

CPP109_TEST(warn_every_n)
{
    Capture cap;
    auto logger = setup_capture(cap);
    logger->set_level(cpp109::LogLevel::TRACE);

    for (int i = 0; i < 4; ++i) {
        LOG_WARN_EVERY_N(2, "warn_every_2");
    }
    CPP109_ASSERT_EQ((int)cap.size(), 2);
    CPP109_ASSERT(cap.has_level(cpp109::LogLevel::WARN));
}

CPP109_TEST(error_every_n)
{
    Capture cap;
    auto logger = setup_capture(cap);
    logger->set_level(cpp109::LogLevel::TRACE);

    for (int i = 0; i < 7; ++i) {
        LOG_ERROR_EVERY_N(4, "err_every_4");
    }
    CPP109_ASSERT_EQ((int)cap.size(), 2);
    CPP109_ASSERT(cap.has_level(cpp109::LogLevel::ERROR));
}

CPP109_TEST(every_n_with_format)
{
    Capture cap;
    auto logger = setup_capture(cap);
    logger->set_level(cpp109::LogLevel::TRACE);

    for (int i = 0; i < 5; ++i) {
        LOG_INFO_EVERY_N(2, "iter {}", i);
    }
    CPP109_ASSERT(cap.has_msg("iter 0"));
    CPP109_ASSERT(cap.has_msg("iter 2"));
    CPP109_ASSERT(cap.has_msg("iter 4"));
}

CPP109_TEST(info_first_n)
{
    Capture cap;
    auto logger = setup_capture(cap);
    logger->set_level(cpp109::LogLevel::TRACE);

    for (int i = 0; i < 10; ++i) {
        LOG_INFO_FIRST_N(3, "first_3");
    }
    CPP109_ASSERT_EQ((int)cap.size(), 3);

    cap.reset();

    for (int i = 0; i < 5; ++i) {
        LOG_INFO_FIRST_N(5, "first_5");
    }
    CPP109_ASSERT_EQ((int)cap.size(), 5);
}

CPP109_TEST(first_n_stops_exactly)
{
    Capture cap;
    auto logger = setup_capture(cap);
    logger->set_level(cpp109::LogLevel::TRACE);

    for (int i = 0; i < 20; ++i) {
        LOG_INFO_FIRST_N(2, "first_2_msg");
    }
    CPP109_ASSERT_EQ((int)cap.size(), 2);
}

CPP109_TEST(macro_combined_with_if)
{
    Capture cap;
    auto logger = setup_capture(cap);
    logger->set_level(cpp109::LogLevel::TRACE);

    LOG_INFO_IF(3 > 1, "cond_with_fmt x={}", 99);
    CPP109_ASSERT(cap.has_msg("cond_with_fmt x=99"));
}

CPP109_TEST(default_logger_default_level)
{
    Capture cap;
    auto logger = cpp109::Registry::instance().default_logger();
    logger->clear_sinks();
    auto sink = std::make_shared<CapSink>(cap);
    sink->set_level(cpp109::LogLevel::TRACE);
    logger->add_sink(sink);

    LOG_TRACE("below_default_level");
    LOG_INFO("at_default_level");

    CPP109_ASSERT(!cap.has_msg("below_default_level"));
    CPP109_ASSERT(cap.has_msg("at_default_level"));
}

int main() {
    test_basic_macros_all_levels();
    test_level_filtering_in_macro();
    test_macro_format_args();
    test_macro_no_args();
    test_info_if_true_and_false();
    test_warn_if_with_expression();
    test_error_if_with_expression();
    test_info_every_n();
    test_warn_every_n();
    test_error_every_n();
    test_every_n_with_format();
    test_info_first_n();
    test_first_n_stops_exactly();
    test_macro_combined_with_if();
    test_default_logger_default_level();

    fprintf(stdout, "test_macros.cpp: all tests passed\n");
    return 0;
}
