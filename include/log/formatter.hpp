#pragma once

#include "log_event.hpp"
#include "platform.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace cpp109 {

enum class FmtOp : unsigned char {
    Literal,
    Year4, Month2, Day2, Hour2, Minute2, Second2,
    Milli3, Micro6,
    LevelShort, LevelCaps,
    Name,
    ThreadId,
    FileBase, FileFull,
    LineNum, FuncName,
    Message,
    Percent,
};

struct FmtToken {
    FmtOp       op;
    std::string literal;
};

template<int N>
void append_int(std::string& out, int val) {
    char buf[N];
    for (int i = N - 1; i >= 0; --i) {
        buf[i] = '0' + (val % 10);
        val /= 10;
    }
    out.append(buf, N);
}

class Formatter {
public:
    explicit Formatter(std::string pattern = default_pattern()) { compile(pattern); }

    virtual ~Formatter() = default;

    virtual void format(const LogEvent& event, std::string& output) {
        output.clear();
        output.reserve(output_size_estimate_);

        const auto& ts = event.timestamp();
        for (const auto& tok : tokens_) {
            switch (tok.op) {
            case FmtOp::Literal:    output += tok.literal;                break;
            case FmtOp::Year4:      append_int<4>(output, ts.year());     break;
            case FmtOp::Month2:     append_int<2>(output, ts.month());    break;
            case FmtOp::Day2:       append_int<2>(output, ts.day());      break;
            case FmtOp::Hour2:      append_int<2>(output, ts.hour());     break;
            case FmtOp::Minute2:    append_int<2>(output, ts.minute());   break;
            case FmtOp::Second2:    append_int<2>(output, ts.second());   break;
            case FmtOp::Milli3:     append_int<3>(output, ts.millisecond()); break;
            case FmtOp::Micro6:     append_int<6>(output, ts.microsecond()); break;
            case FmtOp::LevelShort: output += level_to_string(event.level());       break;
            case FmtOp::LevelCaps:  output += level_to_string_caps(event.level());  break;
            case FmtOp::Name:       output += event.logger_name();                  break;
            case FmtOp::ThreadId:   output += std::to_string(event.thread_id());    break;
            case FmtOp::FileBase:   output += platform::filename_from_path(event.file().data()); break;
            case FmtOp::FileFull:   output += event.file();                         break;
            case FmtOp::LineNum:    output += std::to_string(event.line());         break;
            case FmtOp::FuncName:   output += event.func();                         break;
            case FmtOp::Message:    output += event.message();                      break;
            case FmtOp::Percent:    output += '%';                                  break;
            }
        }
    }

    virtual void format_message_only(const LogEvent& event, std::string& output) {
        output = event.message();
    }

    void set_pattern(const std::string& pattern) { compile(pattern); }
    const std::string& pattern() const noexcept { return pattern_; }

    static const char* default_pattern() noexcept {
        return "[%Y-%m-%d %H:%M:%S.%f] [%L] [%t] [%g:%#] %v";
    }

    static std::unique_ptr<Formatter> colored(std::string pattern = default_pattern());

protected:
    virtual bool parse_next_placeholder(const char*& p, const LogEvent& event, std::string& out) {
        return false;
    }

private:
    void compile(const std::string& pattern) {
        pattern_ = pattern;
        tokens_.clear();
        output_size_estimate_ = 0;

        const char* p = pattern_.data();
        const char* literal_start = p;

        while (*p) {
            if (*p != '%') {
                ++p;
                continue;
            }

            if (literal_start != p) {
                tokens_.push_back({FmtOp::Literal, std::string(literal_start, p)});
                output_size_estimate_ += (size_t)(p - literal_start);
            }

            ++p;
            if (*p == '\0') break;

            switch (*p) {
            case 'Y': tokens_.push_back({FmtOp::Year4, {}});  output_size_estimate_ += 4;  break;
            case 'm': tokens_.push_back({FmtOp::Month2, {}}); output_size_estimate_ += 2;  break;
            case 'd': tokens_.push_back({FmtOp::Day2, {}});   output_size_estimate_ += 2;  break;
            case 'H': tokens_.push_back({FmtOp::Hour2, {}});  output_size_estimate_ += 2;  break;
            case 'M': tokens_.push_back({FmtOp::Minute2, {}});output_size_estimate_ += 2;  break;
            case 'S': tokens_.push_back({FmtOp::Second2, {}});output_size_estimate_ += 2;  break;
            case 'f': tokens_.push_back({FmtOp::Milli3, {}}); output_size_estimate_ += 3;  break;
            case 'F': tokens_.push_back({FmtOp::Micro6, {}}); output_size_estimate_ += 6;  break;
            case 'l': tokens_.push_back({FmtOp::LevelShort, {}}); output_size_estimate_ += 5; break;
            case 'L': tokens_.push_back({FmtOp::LevelCaps, {}});  output_size_estimate_ += 5; break;
            case 'n': tokens_.push_back({FmtOp::Name, {}});       output_size_estimate_ += 8; break;
            case 't': tokens_.push_back({FmtOp::ThreadId, {}});   output_size_estimate_ += 6; break;
            case 'G': tokens_.push_back({FmtOp::FileFull, {}});   output_size_estimate_ += 20; break;
            case 'g': tokens_.push_back({FmtOp::FileBase, {}});   output_size_estimate_ += 20; break;
            case '#': tokens_.push_back({FmtOp::LineNum, {}});    output_size_estimate_ += 5;  break;
            case '!': tokens_.push_back({FmtOp::FuncName, {}});   output_size_estimate_ += 20; break;
            case 'v': tokens_.push_back({FmtOp::Message, {}});    output_size_estimate_ += 64; break;
            case '%': tokens_.push_back({FmtOp::Percent, {}});    output_size_estimate_ += 1;  break;
            default:
                tokens_.push_back({FmtOp::Literal, std::string(1, *p)});
                output_size_estimate_ += 1;
                break;
            }
            ++p;
            literal_start = p;
        }

        if (literal_start != p) {
            tokens_.push_back({FmtOp::Literal, std::string(literal_start, p)});
            output_size_estimate_ += (size_t)(p - literal_start);
        }
    }

    std::string pattern_;
    std::vector<FmtToken> tokens_;
    size_t output_size_estimate_ = 0;
};

class ColoredFormatter : public Formatter {
public:
    explicit ColoredFormatter(std::string pattern = default_pattern())
        : Formatter(std::move(pattern)) {}

    void format(const LogEvent& event, std::string& output) override {
        Formatter::format(event, output);

        const char* color_code = get_ansi_color(event.level());
        if (color_code) {
            output.insert(0, color_code);
            output += "\033[0m";
        }
    }

private:
    static const char* get_ansi_color(LogLevel level) noexcept {
        switch (level) {
            case LogLevel::TRACE: return "\033[37m";
            case LogLevel::DEBUG: return "\033[36m";
            case LogLevel::INFO:  return "\033[32m";
            case LogLevel::WARN:  return "\033[33m";
            case LogLevel::ERROR: return "\033[31m";
            case LogLevel::FATAL: return "\033[1;31m";
            default:              return nullptr;
        }
    }
};

inline std::unique_ptr<Formatter> Formatter::colored(std::string pattern) {
    return std::make_unique<ColoredFormatter>(std::move(pattern));
}

} // namespace cpp109
