#pragma once

#include <cstdint>

// ── 平台检测 ──────────────────────────────────────────────
#if defined(_WIN32) || defined(_WIN64)
    #define CPP109_PLATFORM_WINDOWS 1
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #undef ERROR
    #undef NO_ERROR
#elif defined(__linux__)
    #define CPP109_PLATFORM_LINUX 1
    #include <sys/syscall.h>
    #include <unistd.h>
#elif defined(__APPLE__)
    #define CPP109_PLATFORM_MACOS 1
    #include <pthread.h>
#endif

#include <cstdio>
#include <cstring>

namespace cpp109 {
namespace platform {

// ── 控制台颜色 ────────────────────────────────────────────
enum class ConsoleColor {
    DEFAULT,
    WHITE,
    GRAY,
    GREEN,
    YELLOW,
    RED,
    RED_BOLD,
};

// ── 线程 ID ───────────────────────────────────────────────
inline std::uint64_t current_thread_id() noexcept
{
#ifdef CPP109_PLATFORM_WINDOWS
    return ::GetCurrentThreadId();
#elif defined(CPP109_PLATFORM_LINUX)
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#elif defined(CPP109_PLATFORM_MACOS)
    std::uint64_t tid = 0;
    ::pthread_threadid_np(nullptr, &tid);
    return tid;
#else
    return 0;
#endif
}

// ── 控制台颜色输出 ───────────────────────────────────────
#ifdef CPP109_PLATFORM_WINDOWS
namespace {
inline WORD map_color_to_windows_attr(ConsoleColor color) noexcept
{
    switch (color) {
        case ConsoleColor::DEFAULT:   return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        case ConsoleColor::WHITE:     return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        case ConsoleColor::GRAY:      return FOREGROUND_INTENSITY;
        case ConsoleColor::GREEN:     return FOREGROUND_GREEN;
        case ConsoleColor::YELLOW:    return FOREGROUND_RED | FOREGROUND_GREEN;
        case ConsoleColor::RED:       return FOREGROUND_RED;
        case ConsoleColor::RED_BOLD:  return FOREGROUND_RED | FOREGROUND_INTENSITY;
        default:                      return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    }
}
} // anonymous namespace
#endif

inline void set_console_color(ConsoleColor color)
{
#ifdef CPP109_PLATFORM_WINDOWS
    HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
    WORD attr = map_color_to_windows_attr(color);
    ::SetConsoleTextAttribute(h, attr);
#elif defined(CPP109_PLATFORM_LINUX) || defined(CPP109_PLATFORM_MACOS)
    const char* code;
    switch (color) {
        case ConsoleColor::DEFAULT:   code = "0";    break;
        case ConsoleColor::WHITE:     code = "37";   break;
        case ConsoleColor::GRAY:      code = "90";   break;
        case ConsoleColor::GREEN:     code = "32";   break;
        case ConsoleColor::YELLOW:    code = "33";   break;
        case ConsoleColor::RED:       code = "31";   break;
        case ConsoleColor::RED_BOLD:  code = "1;31"; break;
        default:                      code = "0";    break;
    }
    std::fprintf(stdout, "\033[%sm", code);
#endif
}

inline void reset_console_color()
{
#ifdef CPP109_PLATFORM_WINDOWS
    HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
    ::SetConsoleTextAttribute(h, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
#elif defined(CPP109_PLATFORM_LINUX) || defined(CPP109_PLATFORM_MACOS)
    std::fprintf(stdout, "\033[0m");
#endif
}

// ── 文件名提取 ────────────────────────────────────────────
inline const char* filename_from_path(const char* path) noexcept
{
    if (!path || *path == '\0') return path;
    const char* last = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '\\' || *p == '/') last = p + 1;
    }
    return last;
}

} // namespace platform
} // namespace cpp109
