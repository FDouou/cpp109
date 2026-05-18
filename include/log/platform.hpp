#pragma once

#include <cstdint>

// ──────────────────────────────────────────────────────────
// 模块：平台适配层
//
// 封装 Windows 与 Linux/macOS 差异：
//   - 线程 ID 获取
//   - 控制台颜色输出
//   - 文件名提取（从完整路径中取 basename）
//
// 注意：本项目强制 C++20，source_location 直接使用标准库
// ──────────────────────────────────────────────────────────

// ───────────────────── 平台检测 ─────────────────────
#if defined(_WIN32) || defined(_WIN64)
    #define CPP109_PLATFORM_WINDOWS 1
#elif defined(__linux__)
    #define CPP109_PLATFORM_LINUX 1
#elif defined(__APPLE__)
    #define CPP109_PLATFORM_MACOS 1
#endif

namespace cpp109 {
namespace platform {

// ── 线程 ID ─────────────────────────────────────────
// Windows: GetCurrentThreadId()
// Linux:   gettid() 或 pthread_self()
// macOS:   pthread_threadid_np()
std::uint64_t current_thread_id() noexcept;

// ── 控制台颜色 ──────────────────────────────────────
enum class ConsoleColor {
    DEFAULT,
    WHITE,
    GRAY,
    GREEN,
    YELLOW,
    RED,
    RED_BOLD,
};

void set_console_color(ConsoleColor color);
void reset_console_color();

// ── 文件名提取 ──────────────────────────────────────
// 从完整路径中提取 basename（如 "/a/b/c.cpp" → "c.cpp"）
const char* filename_from_path(const char* path) noexcept;

} // namespace platform
} // namespace cpp109
