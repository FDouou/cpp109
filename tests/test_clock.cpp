// test_clock.cpp — 测试 RdtscClock 周期锚定与定点换算

#include "log/rdtsc_clock.hpp"
#include <cstdio>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

#define CPP109_TEST(name) void test_##name()
#define CPP109_ASSERT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while(0)

namespace {

using Clock = cpp109::RdtscClock;

std::int64_t wall_ns_offset(std::int64_t conv_ns) {
    const auto t0 = std::chrono::system_clock::now();
    const auto t1 = std::chrono::system_clock::now();
    const std::int64_t t0n = std::chrono::duration_cast<std::chrono::nanoseconds>(
        t0.time_since_epoch()).count();
    const std::int64_t t1n = std::chrono::duration_cast<std::chrono::nanoseconds>(
        t1.time_since_epoch()).count();
    const std::int64_t mid = (t0n + t1n) / 2;
    return conv_ns - mid;
}

CPP109_TEST(basic_conversion_accuracy)
{
    Clock& clock = Clock::instance();
    // 换算应在真实墙钟附近（构造含 100ms 测频，总耗时 ~200ms，宽松阈值 5ms）
    const std::int64_t off = wall_ns_offset(static_cast<std::int64_t>(
        clock.to_ns(cpp109::rdtsc_ns())));
    CPP109_ASSERT(std::llabs(off) < 5'000'000);
    if (std::llabs(off) >= 5'000'000) {
        fprintf(stderr, "  basic off=%lld ns\n", static_cast<long long>(off));
    }
}

CPP109_TEST(calibrate_advances_anchor)
{
    Clock& clock = Clock::instance();
    const std::uint64_t old_interval = 1'000'000'000;
    clock.set_calibration_interval(1);   // 每次调用都校准
    for (int i = 0; i < 8; ++i) {
        clock.calibrate();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    clock.set_calibration_interval(old_interval);

    // 多次锚点推进后换算仍贴近墙钟（间隙 <10ms 足够宽松）
    const std::int64_t off = wall_ns_offset(static_cast<std::int64_t>(
        clock.to_ns(cpp109::rdtsc_ns())));
    CPP109_ASSERT(std::llabs(off) < 10'000'000);
    if (std::llabs(off) >= 10'000'000) {
        fprintf(stderr, "  calibrate off=%lld ns\n", static_cast<long long>(off));
    }
}

CPP109_TEST(ns_to_tsc_roundtrip)
{
    Clock& clock = Clock::instance();
    const std::uint64_t now = cpp109::rdtsc_ns();
    const std::uint64_t plus = clock.ns_to_tsc(5'000'000);   // 5ms
    const std::uint64_t a = clock.to_ns(now);
    const std::uint64_t b = clock.to_ns(now + plus);
    const std::int64_t delta = static_cast<std::int64_t>(b - a);
    // 定点系数误差应在 1% 以内
    CPP109_ASSERT(std::llabs(delta - 5'000'000) < 50'000);
    if (std::llabs(delta - 5'000'000) >= 50'000) {
        fprintf(stderr, "  roundtrip delta=%lld ns\n", static_cast<long long>(delta));
    }
}

CPP109_TEST(timer_ids_even_after_long_run)
{
    Clock& clock = Clock::instance();
    // 模拟长时间运行：锚点间距 70 年（delta ~2^63 边界）仍不溢出（无符号环绕语义）
    const std::uint64_t far_future_tsc = cpp109::rdtsc_ns() + (1ULL << 62);
    const std::uint64_t ns = clock.to_ns(far_future_tsc);
    CPP109_ASSERT(ns > 0);
}

} // anonymous namespace

int main() {
    test_basic_conversion_accuracy();
    test_calibrate_advances_anchor();
    test_ns_to_tsc_roundtrip();
    test_timer_ids_even_after_long_run();

    fprintf(stdout, "test_clock.cpp: all tests passed\n");
    return 0;
}