#pragma once

#include "../sink.hpp"

#include <cstdio>
#include <string>

namespace cpp109 {

class ConsoleSink : public Sink {
public:
    // use_stderr: true 则输出到 stderr，否则 stdout
    // colored:    是否启用 ANSI 颜色
    explicit ConsoleSink(bool use_stderr = false, bool colored = true){
        target_ = use_stderr ? stderr : stdout;
        colored_ = colored;
    }

    void set_colored(bool colored) noexcept { colored_ = colored; }
    bool colored() const noexcept { return colored_; }

protected:
    void write(const std::string& formatted_msg, const LogEvent& event) override{
        if(colored_){
            platform::set_console_color(level_to_console_color(event.level()));
            fputs(formatted_msg.c_str(), target_);
            fputc('\n', target_);
            platform::reset_console_color();
        }else{
            fputs(formatted_msg.c_str(), target_);
            fputc('\n', target_);
        }
    }
    void flush_impl() override{
        fflush(target_);
    }

private:

    static platform::ConsoleColor level_to_console_color(LogLevel level) noexcept {
        switch (level) {
            case LogLevel::TRACE: return platform::ConsoleColor::GRAY;
            case LogLevel::DEBUG: return platform::ConsoleColor::WHITE;
            case LogLevel::INFO:  return platform::ConsoleColor::GREEN;
            case LogLevel::WARN:  return platform::ConsoleColor::YELLOW;
            case LogLevel::ERROR: return platform::ConsoleColor::RED;
            case LogLevel::FATAL: return platform::ConsoleColor::RED_BOLD;
            default:              return platform::ConsoleColor::DEFAULT;
        }
    }

    std::FILE* target_;
    bool       colored_;
};

} // namespace cpp109
