// ==========================================================================
// platform_win.cpp — Windows 平台实现
//
// TODO: 实现以下函数
//   - platform::current_thread_id()   → GetCurrentThreadId()
//   - platform::set_console_color()   → SetConsoleTextAttribute()
//   - platform::reset_console_color() → 恢复默认颜色
// ==========================================================================

#include "../include/log/platform.hpp"

#ifdef CPP109_PLATFORM_WINDOWS

#include <windows.h>

namespace cpp109 {
namespace platform {

std::uint64_t current_thread_id() noexcept
{
    return ::GetCurrentThreadId();
}

namespace {

WORD map_color_to_windows_attr(ConsoleColor color) noexcept
{
    switch (color) {
        case ConsoleColor::DEFAULT:    return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        case ConsoleColor::WHITE:      return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        case ConsoleColor::GRAY:       return FOREGROUND_INTENSITY;
        case ConsoleColor::GREEN:      return FOREGROUND_GREEN;
        case ConsoleColor::YELLOW:     return FOREGROUND_RED | FOREGROUND_GREEN;
        case ConsoleColor::RED:        return FOREGROUND_RED;
        case ConsoleColor::RED_BOLD:   return FOREGROUND_RED | FOREGROUND_INTENSITY;
        default:                       return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    }
}

} // anonymous namespace

void set_console_color(ConsoleColor color)
{
    HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
    WORD attr = map_color_to_windows_attr(color);
    ::SetConsoleTextAttribute(h, attr);
}

void reset_console_color()
{
    HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
    ::SetConsoleTextAttribute(h, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

const char* filename_from_path(const char* path) noexcept
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

#endif // CPP109_PLATFORM_WINDOWS
