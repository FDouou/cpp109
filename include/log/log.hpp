#pragma once

#include "log_level.hpp"
#include "platform.hpp"
#include "timestamp.hpp"
#include "log_event.hpp"
#include "formatter.hpp"
#include "sink.hpp"
#include "logger.hpp"
#include "registry.hpp"
#include "ring_buffer.hpp"
#include "async_sink.hpp"
#include "config.hpp"
//#include "config_loader.hpp"

#include "sinks/console_sink.hpp"
#include "sinks/file_sink.hpp"
#include "sinks/rotating_file_sink.hpp"
#include "sinks/daily_file_sink.hpp"
#include "sinks/callback_sink.hpp"
#include "sinks/null_sink.hpp"

#include <cassert>
#include <cstdio>

#define CPP109_DEFAULT_LOGGER() \
    cpp109::Registry::instance().default_logger()

#define CPP109_DEFAULT_LOGGER_REF() \
    cpp109::Registry::instance().default_logger_ref()

// ── 编译期级别过滤（可选）──
// 定义 LOG109CPP_ACTIVE_LEVEL 为 0..6，低于该级别的宏调用在编译期整体
// 消失（连参数求值都省掉）。0=TRACE（全开），6=OFF（全关）。
#ifndef LOG109CPP_ACTIVE_LEVEL
#define LOG109CPP_ACTIVE_LEVEL 0
#endif

namespace cpp109 {

inline constexpr int active_level = LOG109CPP_ACTIVE_LEVEL;

namespace detail {

// 从裸指针 / shared_ptr 中取出 Logger*，供 LOG_* 宏统一处理
template<typename T>
inline T* logger_ptr(T* p) noexcept { return p; }

template<typename T>
inline T* logger_ptr(const std::shared_ptr<T>& p) noexcept { return p.get(); }

} // namespace detail

// 进程退出前冲刷全部日志（提交各线程未满 batch + flush 所有 sink）。
// 应在所有日志写入线程停止后调用。
inline void flush_all_logs() {
    Registry::instance().flush_all();
}

} // namespace cpp109

// ── 唯一写入入口：CPP109_LOG_AT ─────────────────────────────────
// 宏在【调用点】（用户文件）展开，生成 static TinyMeta：
//   file/line/func/fmt 为编译期常量，对象地址也是编译期常量，
//   后台 worker 直接解引用，无查找、无分配、无类型擦除开销。
// lg 必须为 Logger 指针 / shared_ptr / 引用（使用 -> 调用）。
// 空 logger：Debug（NDEBUG 未定义）下 assert 报错；
//           Release 下整条日志安全跳过（日志基础设施不应拖垮业务）。
#define CPP109_LOG_AT(lg, level, fmt, ...)                                      \
    do {                                                                        \
        if constexpr (::cpp109::active_level <= static_cast<int>(level)) {      \
            auto* _cpp109_lg = ::cpp109::detail::logger_ptr(lg);                \
            assert(_cpp109_lg != nullptr && "cpp109: null logger in LOG macro");\
            if (_cpp109_lg) {                                                   \
                static ::cpp109::TinyMeta _cpp109_cs{__FILE__, __LINE__,        \
                                                     __func__, fmt};            \
                _cpp109_lg->log_at(level, &_cpp109_cs, fmt, ##__VA_ARGS__);     \
            }                                                                   \
        }                                                                       \
    } while (0)

// ── 默认 logger 版本：LOG_<LEVEL>(fmt, ...) ──
#define LOG_TRACE(fmt, ...) CPP109_LOG_AT(&CPP109_DEFAULT_LOGGER_REF(), cpp109::LogLevel::TRACE, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) CPP109_LOG_AT(&CPP109_DEFAULT_LOGGER_REF(), cpp109::LogLevel::DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  CPP109_LOG_AT(&CPP109_DEFAULT_LOGGER_REF(), cpp109::LogLevel::INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  CPP109_LOG_AT(&CPP109_DEFAULT_LOGGER_REF(), cpp109::LogLevel::WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) CPP109_LOG_AT(&CPP109_DEFAULT_LOGGER_REF(), cpp109::LogLevel::ERROR, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...) CPP109_LOG_AT(&CPP109_DEFAULT_LOGGER_REF(), cpp109::LogLevel::FATAL, fmt, ##__VA_ARGS__)

// ── 指定 logger 版本：LOG_<LEVEL>_TO(logger, fmt, ...) ──
#define LOG_TRACE_TO(lg, fmt, ...) CPP109_LOG_AT(lg, cpp109::LogLevel::TRACE, fmt, ##__VA_ARGS__)
#define LOG_DEBUG_TO(lg, fmt, ...) CPP109_LOG_AT(lg, cpp109::LogLevel::DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO_TO(lg, fmt, ...)  CPP109_LOG_AT(lg, cpp109::LogLevel::INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN_TO(lg, fmt, ...)  CPP109_LOG_AT(lg, cpp109::LogLevel::WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR_TO(lg, fmt, ...) CPP109_LOG_AT(lg, cpp109::LogLevel::ERROR, fmt, ##__VA_ARGS__)
#define LOG_FATAL_TO(lg, fmt, ...) CPP109_LOG_AT(lg, cpp109::LogLevel::FATAL, fmt, ##__VA_ARGS__)

// ── 条件宏（默认 logger）──
#define LOG_INFO_IF(cond, fmt, ...)  if (cond) LOG_INFO(fmt,  ##__VA_ARGS__)
#define LOG_WARN_IF(cond, fmt, ...)  if (cond) LOG_WARN(fmt,  ##__VA_ARGS__)
#define LOG_ERROR_IF(cond, fmt, ...) if (cond) LOG_ERROR(fmt, ##__VA_ARGS__)

// ── 限频宏（默认 logger）──
#define LOG_INFO_EVERY_N(n, fmt, ...) \
    do { \
        static unsigned int _counter = 0; \
        if (++_counter % (n) == 1) \
            LOG_INFO(fmt, ##__VA_ARGS__); \
    } while(0)

#define LOG_WARN_EVERY_N(n, fmt, ...) \
    do { \
        static unsigned int _counter = 0; \
        if (++_counter % (n) == 1) \
            LOG_WARN(fmt, ##__VA_ARGS__); \
    } while(0)

#define LOG_ERROR_EVERY_N(n, fmt, ...) \
    do { \
        static unsigned int _counter = 0; \
        if (++_counter % (n) == 1) \
            LOG_ERROR(fmt, ##__VA_ARGS__); \
    } while(0)

#define LOG_INFO_FIRST_N(n, fmt, ...) \
    do { \
        static unsigned int _counter = 0; \
        if (++_counter <= static_cast<unsigned int>(n)) \
            LOG_INFO(fmt, ##__VA_ARGS__); \
    } while(0)
