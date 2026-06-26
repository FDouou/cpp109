#pragma once

#include "log_level.hpp"
#include "timestamp.hpp"   // 向后兼容：旧构造函数和 timestamp() 方法需要

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <new>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// ── 平台相关的 RDTSC ──
#if defined(_MSC_VER)
    #include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
    #include <x86intrin.h>
#endif

namespace cpp109 {

// ── SourceLoc：编译期零开销的源码位置（替代 std::source_location） ──
struct SourceLoc {
    const char* file;
    int         line;
    const char* func;
};

// ── RDTSC 时钟周期（前台时间戳，开销 ~10ns，远快于 system_clock） ──
inline std::uint64_t rdtsc_ns() noexcept {
#if defined(_MSC_VER)
    return __rdtsc();
#elif defined(__x86_64__) || defined(__i386__)
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
#else
    // 非 x86 回退到 steady_clock
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

// ── SourceMeta：每个日志调用点的源位置信息 ──
struct SourceMeta {
    const char* file;
    int         line;
    const char* func;
};

// ── TinyMeta：编译期常量打包（每个模板实例化一个 static 实例，在 .data 段）──
// 后台 worker 通过该指针获取 file/func/fmt/decode_fn，使 header 只需 8B 指针。
struct TinyMeta {
    const char* file;                                                      // 8B
    int         line;                                                      // 4B (+4B padding)
    const char* func;                                                      // 8B
    const char* fmt;                                                       // 8B
    void (*decode_fn)(const std::byte*, const char*, std::string&);        // 8B
};
// total = 40B (static instance, so no per-record cost)

// ── TinyHeader：32B 超轻量 header（对齐 quill，替代 CompactLogHeader 的 60B）──
struct alignas(8) TinyHeader {
    std::uint64_t      timestamp_tsc;                                      // 8B, offset  0
    const TinyMeta*    meta;                                               // 8B, offset  8
    std::uint64_t      thread_id;                                          // 8B, offset 16
    std::uint32_t      args_size;                                          // 4B, offset 24
    std::uint8_t       level;                                              // 1B, offset 28
    std::uint8_t       flags;                                              // 1B, offset 29
    std::uint16_t      padding;                                            // 2B, offset 30
};                                                                         // total 32B
static_assert(sizeof(TinyHeader) == 32, "TinyHeader must be exactly 32 bytes");

// ── CompactLogHeader：保留向后兼容（旧测试/二进制使用 60B 格式）──
struct CompactLogHeader {
    std::uint64_t timestamp_tsc;                                           // 8B, offset  0
    const char*   fmt;                                                     // 8B, offset  8
    void (*decode_fn)(const std::byte*, const char*, std::string&);        // 8B, offset 16
    std::uint64_t thread_id;                                               // 8B, offset 24
    const char*   file;                                                    // 8B, offset 32
    const char*   func;                                                    // 8B, offset 40
    std::uint32_t line;                                                    // 4B, offset 48
    std::uint32_t args_size;                                               // 4B, offset 52
    std::uint8_t  level;                                                   // 1B, offset 56
    std::uint8_t  flags;                                                   // 1B, offset 57
    std::uint16_t padding;                                                 // 2B, offset 58
};                                                                         // total 60B
static_assert(sizeof(CompactLogHeader) <= 64, "CompactLogHeader should be <=64 bytes");

// ── LogRecordHeader：保留向下兼容（旧路径使用）──
struct LogRecordHeader {
    std::uint64_t timestamp_tsc;
    const char*   fmt;
    const char*   file;
    const char*   func;
    const char*   logger_name;
    void (*decode_fn)(const std::byte*, const char*, std::string&);
    std::uint64_t thread_id;
    std::uint32_t line;
    LogLevel      level;
    std::uint32_t args_size;
};
static_assert(sizeof(LogRecordHeader) <= 72, "LogRecordHeader should be ≤72 bytes");

// ── Codec：参数编解码（类型擦除，编译期生成 decode_fn）──
namespace detail {

// 计算单个参数编码大小
template<typename T>
std::size_t encoded_size(const T& arg) {
    using DT = std::decay_t<T>;
    if constexpr (std::is_trivially_copyable_v<DT>) {
        return sizeof(DT);
    } else if constexpr (std::is_same_v<DT, std::string>) {
        return sizeof(std::uint32_t) + arg.size();
    } else if constexpr (std::is_convertible_v<T, const char*>) {
        const char* s = static_cast<const char*>(arg);
        return sizeof(std::uint32_t) + std::strlen(s);
    } else if constexpr (std::is_same_v<DT, std::string_view>) {
        return sizeof(std::uint32_t) + arg.size();
    } else {
        static_assert(sizeof(T) == 0, "Unsupported argument type for codec");
        return 0;
    }
}

// 计算所有参数编码总大小
template<typename... Args>
std::size_t compute_encoded_size(const Args&... args) {
    std::size_t total = 0;
    ((total += encoded_size(args)), ...);
    return total;
}

// 编码单个参数
template<typename T>
void encode_one(std::byte*& ptr, const T& arg) {
    using DT = std::decay_t<T>;
    if constexpr (std::is_trivially_copyable_v<DT>) {
        std::memcpy(ptr, &arg, sizeof(DT));
        ptr += sizeof(DT);
    } else if constexpr (std::is_same_v<DT, std::string>) {
        std::uint32_t len = static_cast<std::uint32_t>(arg.size());
        std::memcpy(ptr, &len, sizeof(len));
        ptr += sizeof(len);
        std::memcpy(ptr, arg.data(), len);
        ptr += len;
    } else if constexpr (std::is_convertible_v<T, const char*>) {
        const char* s = static_cast<const char*>(arg);
        std::uint32_t len = static_cast<std::uint32_t>(std::strlen(s));
        std::memcpy(ptr, &len, sizeof(len));
        ptr += sizeof(len);
        std::memcpy(ptr, s, len);
        ptr += len;
    } else if constexpr (std::is_same_v<DT, std::string_view>) {
        std::uint32_t len = static_cast<std::uint32_t>(arg.size());
        std::memcpy(ptr, &len, sizeof(len));
        ptr += sizeof(len);
        std::memcpy(ptr, arg.data(), len);
        ptr += len;
    }
}

// 编码所有参数
template<typename... Args>
void encode_args(std::byte* ptr, const Args&... args) {
    ((encode_one(ptr, args)), ...);
}

// 解码单个参数（后台线程调用）
template<typename T>
T decode_one(const std::byte*& ptr) {
    using DT = std::decay_t<T>;
    if constexpr (std::is_trivially_copyable_v<DT>) {
        DT val;
        std::memcpy(&val, ptr, sizeof(DT));
        ptr += sizeof(DT);
        return val;
    } else if constexpr (std::is_same_v<DT, std::string>) {
        std::uint32_t len;
        std::memcpy(&len, ptr, sizeof(len));
        ptr += sizeof(len);
        std::string s(reinterpret_cast<const char*>(ptr), len);
        ptr += len;
        return s;
    } else {
        static_assert(sizeof(T) == 0, "Unsupported argument type for codec (decode)");
        return DT{};
    }
}

// 类型擦除的 decode + format 函数（编译期为每套 Args 生成）
template<typename... Args>
void decode_and_format(const std::byte* ptr, const char* fmt, std::string& out) {
    if constexpr (sizeof...(Args) == 0) {
        out = fmt ? fmt : "";
    } else {
        const std::byte* p = ptr;
        auto args = std::make_tuple(decode_one<std::decay_t<Args>>(p)...);
        std::apply([&](const auto&... elems) {
            out = std::vformat(fmt, std::make_format_args(elems...));
        }, args);
    }
}

// 前台线程的编码缓冲区（thread_local，首次调用分配后不再 realloc）
inline std::byte* get_encode_buffer(std::size_t needed) {
    thread_local static std::vector<std::byte> buf;
    if (buf.size() < needed) {
        buf.resize(needed + 64);  // 少量超额分配避免频繁 resizing
    }
    return buf.data();
}

} // namespace detail

// ── 延迟格式化 LogEvent —— 前台只传格式串指针 + 参数二进制拷贝 ────
// 后台线程调 format_message() 完成 std::vformat，再从 timestamp_ns 构造时间字段。
// 体积从 160B 降至 104B（实测），减少 ring buffer cache miss。
class LogEvent {
public:
    LogEvent() = default;

    // ─── 旧构造（兼容已有测试，直接传入格式化后的消息和 Timestamp）───
    LogEvent(std::string_view logger_name, LogLevel level,
             std::string message, const Timestamp& ts,
             SourceLoc loc, std::uint64_t thread_id)
        : logger_name_(logger_name),
          fmt_(nullptr),
          format_fn_(&format_preformatted),
          destruct_fn_(&destruct_preformatted),
          move_fn_(nullptr),
          level_(level),
          line_(static_cast<int>(loc.line)),
          thread_id_(thread_id),
          timestamp_ns_(std::chrono::duration_cast<std::chrono::nanoseconds>(
              ts.get().time_since_epoch()).count()),
          file_(loc.file),
          func_(loc.func),
          args_(new std::string(std::move(message)))
    {}

    // ─── 新构造（模板，延迟格式化：前台只拷贝参数，后台 vformat）───
    // 支持 SBO：sizeof(TupleT) <= 64B 时零堆分配
    template<typename... Args>
    LogEvent(std::string_view logger_name, LogLevel level,
             const char* fmt, SourceLoc loc,
             std::uint64_t thread_id, Args&&... args)
        : logger_name_(logger_name),
          fmt_(fmt),
          level_(level),
          line_(static_cast<int>(loc.line)),
          thread_id_(thread_id),
          timestamp_ns_(now_ns()),
          file_(loc.file),
          func_(loc.func)
    {
        using TupleT = std::tuple<std::decay_t<Args>...>;
        format_fn_ = &format_impl<TupleT>;

        if constexpr (sizeof(TupleT) <= SBO_SIZE) {
            // SBO 路径：placement new 进内联 buffer，零堆分配
            move_fn_ = &move_sbo<TupleT>;
            destruct_fn_ = &destruct_sbo<TupleT>;
            args_ = new (sbo_buffer_) TupleT(std::forward<Args>(args)...);
        } else {
            // Heap 路径：大参数集退化为堆分配
            move_fn_ = nullptr;
            destruct_fn_ = &destruct_impl<TupleT>;
            args_ = new TupleT(std::forward<Args>(args)...);
        }
    }

    ~LogEvent() {
        if (args_ && destruct_fn_) {
            destruct_fn_(args_);
        }
    }

    // 拷贝构造（深拷贝：格式化为预格式化消息存储）
    LogEvent(const LogEvent& other)
        : logger_name_(other.logger_name_),
          fmt_(nullptr),
          format_fn_(&format_preformatted),
          destruct_fn_(&destruct_preformatted),
          move_fn_(nullptr),
          level_(other.level_),
          line_(other.line_),
          thread_id_(other.thread_id_),
          timestamp_ns_(other.timestamp_ns_),
          file_(other.file_),
          func_(other.func_)
    {
        std::string msg;
        other.format_message(msg);
        args_ = new std::string(std::move(msg));
    }

    // 移动语义（转移 args_ 所有权；SBO 时 placement new move 到目标 buffer）
    LogEvent(LogEvent&& other) noexcept
        : logger_name_(other.logger_name_),
          fmt_(other.fmt_),
          format_fn_(other.format_fn_),
          destruct_fn_(other.destruct_fn_),
          move_fn_(other.move_fn_),
          level_(other.level_),
          line_(other.line_),
          thread_id_(other.thread_id_),
          timestamp_ns_(other.timestamp_ns_),
          file_(other.file_),
          func_(other.func_)
    {
        if (other.args_ == other.sbo_buffer_) {
            if (move_fn_) {
                // SBO 带 move 支持（tuple 路径）：placement new + 析构
                move_fn_(sbo_buffer_, other.args_);
            } else {
                // SBO 无 move 支持（worker 路径，存储的是 string_view）
                // string_view 是平凡可拷贝的，直接 memcpy 拷贝 SBO 内容
                std::memcpy(sbo_buffer_, other.sbo_buffer_, SBO_SIZE);
            }
            args_ = sbo_buffer_;
        } else {
            // heap：转移指针
            args_ = other.args_;
        }
        other.args_ = nullptr;
        other.destruct_fn_ = nullptr;
        other.move_fn_ = nullptr;
    }

    LogEvent& operator=(LogEvent&& other) noexcept {
        if (this != &other) {
            if (args_ && destruct_fn_) destruct_fn_(args_);
            logger_name_ = other.logger_name_;
            fmt_ = other.fmt_;
            format_fn_ = other.format_fn_;
            destruct_fn_ = other.destruct_fn_;
            move_fn_ = other.move_fn_;
            level_ = other.level_;
            line_ = other.line_;
            thread_id_ = other.thread_id_;
            timestamp_ns_ = other.timestamp_ns_;
            file_ = other.file_;
            func_ = other.func_;

            if (other.args_ == other.sbo_buffer_) {
                if (move_fn_) {
                    move_fn_(sbo_buffer_, other.args_);
                } else {
                    std::memcpy(sbo_buffer_, other.sbo_buffer_, SBO_SIZE);
                }
                args_ = sbo_buffer_;
            } else {
                args_ = other.args_;
            }
            other.args_ = nullptr;
            other.destruct_fn_ = nullptr;
            other.move_fn_ = nullptr;
        }
        return *this;
    }

    // 禁止拷贝赋值（使用移动赋值代替）
    LogEvent& operator=(const LogEvent&) = delete;

    // ── 后台线程调用：格式化消息 ──
    void format_message(std::string& out) const {
        if (format_fn_ && args_) {
            format_fn_(args_, fmt_ ? fmt_ : "", out);
        } else if (fmt_) {
            out = fmt_;
        } else {
            out.clear();
        }
    }

    // ── 只读访问 ──
    std::string_view logger_name() const noexcept { return logger_name_; }
    LogLevel         level()       const noexcept { return level_; }
    std::uint64_t    timestamp_ns() const noexcept { return timestamp_ns_; }
    std::string_view file()        const noexcept { return file_; }
    int              line()        const noexcept { return line_; }
    std::string_view func()        const noexcept { return func_; }
    std::uint64_t    thread_id()   const noexcept { return thread_id_; }

    // ── Worker 构造（后台线程从 ByteRingBuffer 解码后使用）──
    // 消息已格式化好存于 message，string_view 指向 worker 的 thread_local 存储。
    // 无堆分配、无 system_clock 调用（timestamp_ns 已由 RdtscClock 转换）。
    LogEvent(std::string_view logger_name, LogLevel level,
             std::uint64_t timestamp_ns,
             const char* file, int line, const char* func,
             std::uint64_t thread_id, std::string_view message)
        : logger_name_(logger_name),
          fmt_(nullptr),
          format_fn_(&format_message_view),
          destruct_fn_(nullptr),
          move_fn_(nullptr),
          level_(level),
          line_(line),
          thread_id_(thread_id),
          timestamp_ns_(timestamp_ns),
          file_(file),
          func_(func),
          args_(sbo_buffer_) // 指向 SBO buffer，存储 string_view
    {
        // 在 SBO buffer 中 placement-new 一个 string_view
        ::new (sbo_buffer_) std::string_view(message);
    }

    // ── 向后兼容接口（用于老版 formatter / test）──
    const std::string& message() const {
        static thread_local std::string tl_msg;
        format_message(tl_msg);
        return tl_msg;
    }
    Timestamp timestamp() const {
        // 从纳秒正确转换为 system_clock::time_point
        // system_clock::duration 精度平台相关（MSVC 100ns, Linux 1ns）
        using namespace std::chrono;
        auto d = duration_cast<Timestamp::clock::duration>(nanoseconds(timestamp_ns_));
        return Timestamp(Timestamp::time_point(d));
    }

private:
    // 预格式化消息（旧构造使用）：直接返回 args_ 中存储的 std::string
    static void format_preformatted(const void* args, const char*, std::string& out) {
        out = *static_cast<const std::string*>(args);
    }
    static void destruct_preformatted(void* args) {
        delete static_cast<std::string*>(args);
    }

    // 模板参数包的格式化（新构造使用）
    template<typename TupleT>
    static void format_impl(const void* args, const char* fmt, std::string& out) {
        const auto* tuple = static_cast<const TupleT*>(args);
        std::apply([&](const auto&... elems) {
            out = std::vformat(fmt, std::make_format_args(elems...));
        }, *tuple);
    }
    template<typename TupleT>
    static void destruct_impl(void* args) {
        delete static_cast<TupleT*>(args);
    }

    // SBO 移动：placement new move 到 dst 内存，析构 src
    template<typename TupleT>
    static void move_sbo(void* dst, void* src) {
        new (dst) TupleT(std::move(*static_cast<TupleT*>(src)));
        static_cast<TupleT*>(src)->~TupleT();
    }
    // SBO 析构：只调析构，不释放内存（内联 buffer 是类成员）
    template<typename TupleT>
    static void destruct_sbo(void* args) {
        static_cast<TupleT*>(args)->~TupleT();
    }

    // Worker 路径：从 SBO buffer 中读取 string_view
    static void format_message_view(const void* args, const char*, std::string& out) {
        out = *static_cast<const std::string_view*>(args);
    }

    // 前台只取 system_clock 纳秒，不调 localtime_r
    static std::uint64_t now_ns() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // ── 字段布局（约 176B）─────────────────────────
    // 所有指针和函数指针必须初始化（nullptr），否则 RingBuffer 默认构造后
    // move assignment 中检查 destruct_fn_ 时会使用未初始化的垃圾值导致堆损坏。
    std::string_view logger_name_{};                                        // 16B
    const char*      fmt_            = nullptr;                             // 8B
    void (*format_fn_)(const void*, const char*, std::string&) = nullptr;   // 8B
    void (*destruct_fn_)(void*)      = nullptr;                             // 8B
    void (*move_fn_)(void*, void*)   = nullptr;                             // 8B
    LogLevel         level_         = LogLevel::OFF;                        // 4B
    int              line_           = 0;                                   // 4B
    std::uint64_t    thread_id_      = 0;                                   // 8B
    std::uint64_t    timestamp_ns_   = 0;                                   // 8B
    std::string_view file_{};                                               // 16B
    std::string_view func_{};                                               // 16B
    void*            args_           = nullptr;                             // 8B
    // SBO 内联 buffer（必须放在最后，用于地址比较判断 SBO）
    static constexpr std::size_t SBO_SIZE = 64;
    alignas(std::max_align_t) std::byte sbo_buffer_[SBO_SIZE];              // 64B
};

} // namespace cpp109
