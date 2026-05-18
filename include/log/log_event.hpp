#pragma once

#include "log_level.hpp"
#include "timestamp.hpp"
#include "platform.hpp"

#include <source_location>
#include <string>
#include <string_view>
#include <cstdint>

namespace cpp109 {

// ──────────────────────────────────────────────────────────
// 模块二：日志事件
//
// 封装一条日志的完整上下文，是日志链路中的数据载体。
// 设计要点：
//   - 使用 string_view 减少拷贝
//   - 时间戳在构造时立即获取
//   - C++20 std::source_location 自动捕获调用点
// ──────────────────────────────────────────────────────────

class LogEvent {
public:
    LogEvent() = default;

    // 完整构造（由 Logger::log_impl 内部调用）
    LogEvent(std::string_view logger_name,
             LogLevel level,
             std::string      message,
             Timestamp        ts,
             std::source_location loc,
             std::uint64_t    thread_id)
        : logger_name_(logger_name),
          level_(level),
          message_(std::move(message)),
          timestamp_(ts),
          file_(loc.file_name()),
          line_(loc.line()),
          func_(loc.function_name()),
          thread_id_(thread_id)
    {
    }

    // ── 只读访问 ──
    const std::string&    message()     const noexcept { return message_; }
    LogLevel              level()       const noexcept { return level_; }
    const std::string&    logger_name() const noexcept { return logger_name_; }
    const Timestamp&      timestamp()   const noexcept { return timestamp_; }
    std::string_view      file()        const noexcept { return file_; }
    int                   line()        const noexcept { return line_; }
    std::string_view      func()        const noexcept { return func_; }
    std::uint64_t         thread_id()   const noexcept { return thread_id_; }

private:
    std::string      logger_name_;
    LogLevel         level_ = LogLevel::OFF;
    std::string      message_;
    Timestamp        timestamp_;
    std::string_view file_;
    int              line_ = 0;
    std::string_view func_;
    std::uint64_t    thread_id_ = 0;
};

} // namespace cpp109
