#include "../include/log/platform.hpp"

#if defined(CPP109_PLATFORM_LINUX) || defined(CPP109_PLATFORM_MACOS)

#include <pthread.h>
#include <cstdio>
#include <cstring>

namespace cpp109 {
namespace platform {

std::uint64_t current_thread_id() noexcept
{
#if defined(__linux__)
    return static_cast<std::uint64_t>(::gettid());
#elif defined(__APPLE__)
    std::uint64_t tid = 0;
    ::pthread_threadid_np(nullptr, &tid);
    return tid;
#else
    return 0;
#endif
}

void set_console_color(ConsoleColor color)
{
    const char* code;
    switch (color) {
        case ConsoleColor::DEFAULT:    code = "0";    break;
        case ConsoleColor::WHITE:      code = "37";   break;
        case ConsoleColor::GRAY:       code = "90";   break;
        case ConsoleColor::GREEN:      code = "32";   break;
        case ConsoleColor::YELLOW:     code = "33";   break;
        case ConsoleColor::RED:        code = "31";   break;
        case ConsoleColor::RED_BOLD:   code = "1;31"; break;
        default:                       code = "0";    break;
    }
    fprintf(stdout, "\033[%sm", code);
}

void reset_console_color()
{
    fprintf(stdout, "\033[0m");
}

const char* filename_from_path(const char* path) noexcept
{
    if (!path || *path == '\0') return path;
    const char* last = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/') last = p + 1;
    }
    return last;
}

} // namespace platform
} // namespace cpp109

#endif // CPP109_PLATFORM_LINUX || CPP109_PLATFORM_MACOS
