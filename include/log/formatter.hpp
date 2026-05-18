#pragma once

#include "log_event.hpp"

#include <memory>
#include <string>
#include <format>

namespace cpp109 {

// ──────────────────────────────────────────────────────────
// 基于模式字符串（类似 strftime 风格）将 LogEvent 转为最终输出字符串。
//
// 占位符
//   %Y %m %d %H %M %S ── 日期时间各字段
//   %f / %F           ── 毫秒(3位) / 微秒(6位)
//   %l / %L           ── 级别缩写 / 级别 [ALL_CAPS]
//   %n                ── logger 名称
//   %t                ── 线程 ID
//   %g / %G           ── 文件 basename / 完整路径
//   %#                ── 行号
//   %!                ── 函数名
//   %v                ── 日志消息正文
//   %%                ── 字面量百分号
//
// 默认模式：
//   "[%Y-%m-%d %H:%M:%S.%f] [%L] [%t] [%g:%#] %v"
// ──────────────────────────────────────────────────────────

class Formatter {
public:
    explicit Formatter(std::string pattern = default_pattern()){
        pattern_ = pattern;
    }

    virtual ~Formatter() = default;

    // 格式化单条日志事件，输出到 output
    virtual void format(const LogEvent& event, std::string& output){
        output.clear();
        const char* p = pattern_.data();
        while (parse_next_placeholder(p, event, output));
    }

    // 仅格式化消息正文（不含时间戳/级别等），用于带色输出等场景
    virtual void format_message_only(const LogEvent& event, std::string& output){
        output = event.message();
    }

    void set_pattern(const std::string& pattern){
        pattern_ = pattern;
    }
    const std::string& pattern() const noexcept { return pattern_; }

    static const char* default_pattern() noexcept{
        //return "[%Y-%m-%d %H:%M:%S.%f] [%L] [%t] [%g:%#] [%!] %v";
        return "[%Y-%m-%d %H:%M:%S.%f] [%L] [%t] [%g:%#] %v";
    }

    static std::unique_ptr<Formatter> colored(std::string pattern = default_pattern());

protected:
    virtual bool parse_next_placeholder(const char*& p, const LogEvent& event, std::string& out){
        if (*p == '\0') return false;

        if (*p != '%'){
            out += *p;
        }else{
            p++;
            if (*p == '\0') return false;
            switch (*p){
                case 'Y':
                    out += std::format("{:04d}", event.timestamp().year());
                    break;
                case 'm':
                    out += std::format("{:02d}", event.timestamp().month());
                    break;
                case 'd':
                    out += std::format("{:02d}", event.timestamp().day());
                    break;
                case 'H':
                    out += std::format("{:02d}", event.timestamp().hour());
                    break;
                case 'M':
                    out += std::format("{:02d}", event.timestamp().minute());
                    break;
                case 'S':
                    out += std::format("{:02d}", event.timestamp().second());
                    break;
                case 'f':
                    out += std::format("{:03d}", event.timestamp().millisecond());
                    break;
                case 'F':
                    out += std::format("{:06d}", event.timestamp().microsecond());
                    break;
                case 'l':
                    out += cpp109::level_to_string(event.level());
                    break;
                case 'L':
                    out += cpp109::level_to_string_caps(event.level());
                    break;
                case 'n':
                    out += event.logger_name();
                    break;
                case 't':
                    out += std::to_string(event.thread_id());
                    break;
                case 'G':
                    out += event.file();
                    break;
                case 'g':
                    out += platform::filename_from_path(event.file().data());
                    break;
                case '#':
                    out += std::to_string(event.line());
                    break;
                case '!':
                    out += event.func();
                    break;
                case 'v':
                    out += event.message();
                    break;
                case '%':
                    out += '%';
                    break;
                default:
                    out += *p;
                    break;
            }
        }
        p++;
        return true;
    }

    std::string pattern_;
};

// ──────────────────────────────────────────────────────────
// 带颜色的格式化器（ANSI 转义码）
// ──────────────────────────────────────────────────────────
class ColoredFormatter : public Formatter {
public:
    explicit ColoredFormatter(std::string pattern = default_pattern())
        : Formatter(std::move(pattern)) {}

    void format(const LogEvent& event, std::string& output) override {
        Formatter::format(event, output);
        
        const char* color_code = get_ansi_color(event.level());
        if (color_code) {
            output.insert(0, color_code);      // 头部插入颜色
            output += "\033[0m";               // 尾部重置颜色
        }
    }

private:
    static const char* get_ansi_color(LogLevel level) noexcept {
        switch (level) {
            case LogLevel::TRACE: return "\033[37m";  // 浅灰
            case LogLevel::DEBUG: return "\033[36m";  // 青色
            case LogLevel::INFO:  return "\033[32m";  // 绿色
            case LogLevel::WARN:  return "\033[33m";  // 黄色
            case LogLevel::ERROR: return "\033[31m";  // 红色
            case LogLevel::FATAL: return "\033[1;31m"; // 红色加粗
            default:              return nullptr;
        }
    }
};

inline std::unique_ptr<Formatter> Formatter::colored(std::string pattern) {
    return std::make_unique<ColoredFormatter>(std::move(pattern));
}

} // namespace cpp109
