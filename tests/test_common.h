// test_common.h — 测试断言与失败统计。
//
// 断言失败会递增计数器，各测试 main 据此返回非零退出码，使 ctest 能真正
// 捕获断言失败。早期实现只把 FAIL 打印到 stderr 却始终 exit 0，导致 ctest
// 报 "Passed" 而断言实际已失败——回归测试形同虚设。
#pragma once

#include <cstdio>

namespace cpp109_test {
inline int& failures() noexcept {
    static int n = 0;
    return n;
}
} // namespace cpp109_test

#define CPP109_TEST(name) void test_##name()

#define CPP109_ASSERT(cond)                                                       \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++::cpp109_test::failures();                                          \
        }                                                                         \
    } while (0)

#define CPP109_ASSERT_EQ(a, b)                                                    \
    do {                                                                          \
        if ((a) != (b)) {                                                         \
            std::fprintf(stderr, "FAIL: %s:%d: %s == %s\n", __FILE__, __LINE__,   \
                         #a, #b);                                                 \
            ++::cpp109_test::failures();                                          \
        }                                                                         \
    } while (0)

// main 结尾统一使用：return CPP109_TEST_RESULT();
#define CPP109_TEST_RESULT() (::cpp109_test::failures() ? 1 : 0)
