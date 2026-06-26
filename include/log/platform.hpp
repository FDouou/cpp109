#pragma once

#include <cstddef>
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
    #ifndef _GNU_SOURCE
        #define _GNU_SOURCE
    #endif
    #include <sys/syscall.h>
    #include <unistd.h>
    #include <pthread.h>
#elif defined(__APPLE__)
    #define CPP109_PLATFORM_MACOS 1
    #include <pthread.h>
    #include <unistd.h>
#endif

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#ifdef CPP109_PLATFORM_LINUX
#include <map>
#endif

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

// ── 线程亲和性 ────────────────────────────────────────────
// native_handle() 返回值类型别名
#ifdef CPP109_PLATFORM_LINUX
    using thread_handle = pthread_t;
#elif defined(CPP109_PLATFORM_WINDOWS)
    using thread_handle = HANDLE;
#else
    using thread_handle = void*;
#endif

// 设置线程亲和到指定 CPU 核集合。
//   handle: std::thread::native_handle() 返回值
//   cpu_ids: CPU 核编号数组（从 0 开始），count 为数组长度
// 返回 true 成功；false 表示平台不支持或调用失败（不抛异常）。
// cpus 为空（count==0）时返回 true 且不修改亲和性。
inline bool set_thread_affinity(thread_handle handle,
                                const int* cpu_ids,
                                std::size_t count) noexcept
{
    if (count == 0) return true;

#ifdef CPP109_PLATFORM_LINUX
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    std::size_t valid_count = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (cpu_ids[i] < 0 || cpu_ids[i] >= CPU_SETSIZE)
            continue;
        CPU_SET(cpu_ids[i], &cpuset);
        ++valid_count;
    }
    if (valid_count == 0) return false;
    return ::pthread_setaffinity_np(handle, sizeof(cpu_set_t), &cpuset) == 0;
#elif defined(CPP109_PLATFORM_WINDOWS)
    DWORD_PTR mask = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (cpu_ids[i] < 0 || cpu_ids[i] >= static_cast<int>(8 * sizeof(DWORD_PTR)))
            continue;
        mask |= static_cast<DWORD_PTR>(1) << cpu_ids[i];
    }
    if (mask == 0) return false;
    return ::SetThreadAffinityMask(handle, mask) != 0;
#else
    // macOS: 不支持线程亲和性设置
    (void)handle; (void)cpu_ids;
    return false;
#endif
}

// 返回系统可用逻辑 CPU 核数。
inline unsigned int cpu_count() noexcept
{
#if defined(CPP109_PLATFORM_LINUX) || defined(CPP109_PLATFORM_MACOS)
    long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<unsigned int>(n) : 1;
#elif defined(CPP109_PLATFORM_WINDOWS)
    SYSTEM_INFO info;
    ::GetSystemInfo(&info);
    return info.dwNumberOfProcessors;
#else
    return 1;
#endif
}

// ── CPU 拓扑 ──────────────────────────────────────────────
// 物理核信息
struct PhysicalCore {
    std::vector<int> logical_cpus;  // 该物理核包含的逻辑核编号
    bool has_smt = false;           // 是否有超线程（logical_cpus.size() > 1）
};

// CPU 拓扑
struct CpuTopology {
    std::vector<PhysicalCore> physical_cores;

    std::size_t physical_count() const noexcept { return physical_cores.size(); }

    // 每个物理核的第一个逻辑核（用于工作线程独占物理核）
    std::vector<int> first_logical_of_each() const {
        std::vector<int> v;
        for (const auto& c : physical_cores)
            if (!c.logical_cpus.empty())
                v.push_back(c.logical_cpus[0]);
        return v;
    }

    // 有超线程的物理核的第二个逻辑核（用于后台线程，IO 阻塞不争执行单元）
    std::vector<int> second_logical_of_smt() const {
        std::vector<int> v;
        for (const auto& c : physical_cores)
            if (c.logical_cpus.size() >= 2)
                v.push_back(c.logical_cpus[1]);
        return v;
    }
};

// 获取 CPU 拓扑（失败时返回空拓扑，不抛异常）
#ifdef CPP109_PLATFORM_WINDOWS
// 使用 GetLogicalProcessorInformation（非 Ex），在 hybrid 架构上也能正确枚举物理核。
// 注意：不支持 >64 逻辑核（多 group），但 24 逻辑核在单 group 内，没问题。
inline CpuTopology get_cpu_topology() noexcept {
    CpuTopology topo;
    DWORD len = 0;
    ::GetLogicalProcessorInformation(nullptr, &len);
    if (len == 0) return topo;

    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> infos(
        len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!::GetLogicalProcessorInformation(infos.data(), &len))
        return topo;

    for (const auto& info : infos) {
        if (info.Relationship == RelationProcessorCore) {
            PhysicalCore pc;
            // ProcessorMask 是位掩码，每位代表一个逻辑核的 APIC ID
            for (int bit = 0; bit < static_cast<int>(8 * sizeof(DWORD_PTR)); ++bit) {
                if (info.ProcessorMask & (static_cast<DWORD_PTR>(1) << bit)) {
                    pc.logical_cpus.push_back(bit);
                }
            }
            pc.has_smt = pc.logical_cpus.size() > 1;
            topo.physical_cores.push_back(std::move(pc));
        }
    }
    return topo;
}
#elif defined(CPP109_PLATFORM_LINUX)
inline CpuTopology get_cpu_topology() noexcept {
    CpuTopology topo;
    // 从 /sys/devices/system/cpu/cpu*/topology/core_id 读取物理核分组
    std::map<int, std::vector<int>> core_to_logicals;
    unsigned int n = cpu_count();
    for (unsigned int cpu = 0; cpu < n; ++cpu) {
        std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/core_id";
        FILE* f = fopen(path.c_str(), "r");
        if (!f) continue;
        int core_id = -1;
        if (fscanf(f, "%d", &core_id) == 1 && core_id >= 0) {
            core_to_logicals[core_id].push_back(static_cast<int>(cpu));
        }
        fclose(f);
    }
    for (auto& [cid, logicals] : core_to_logicals) {
        PhysicalCore pc;
        pc.logical_cpus = std::move(logicals);
        pc.has_smt = pc.logical_cpus.size() > 1;
        topo.physical_cores.push_back(std::move(pc));
    }
    return topo;
}
#else
// macOS / 其他平台：返回空拓扑（不报错，降级）
inline CpuTopology get_cpu_topology() noexcept {
    return {};
}
#endif

} // namespace platform
} // namespace cpp109
