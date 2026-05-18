#pragma once

#include "log_event.hpp"
#include "formatter.hpp"
#include "log_level.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace cpp109 {

class Sink {
public:
    Sink(){
        formatter_ = std::make_unique<Formatter>();
    }
    virtual ~Sink() = default;

    virtual void log(const LogEvent& event){
        std::lock_guard<std::mutex> lock(mutex_);
        if(event.level() < level_){
            return;
        }
        std::string formatted_msg;
        formatter_->format(event, formatted_msg);
        write(formatted_msg, event);
    }
    void flush(){
        std::lock_guard<std::mutex> lock(mutex_);
        flush_impl();
    }

    void set_formatter(std::unique_ptr<Formatter> fmt){
        std::lock_guard<std::mutex> lock(mutex_);
        formatter_ = std::move(fmt);
    }
    Formatter* formatter() noexcept { return formatter_.get(); }

    void set_level(LogLevel level) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        level_ = level; 
    }
    LogLevel level() const noexcept { return level_; }

    void set_pattern(const std::string& pattern){
        std::lock_guard<std::mutex> lock(mutex_);
        formatter_->set_pattern(pattern);
    }
protected:
    // 子类实现：将格式化后的字符串写入目标
    virtual void write(const std::string& formatted_msg, const LogEvent& event) = 0;
    // 子类实现：刷新缓冲区
    virtual void flush_impl(){
        // 默认实现：空操作
    }

    std::mutex              mutex_;
    std::unique_ptr<Formatter> formatter_;
    LogLevel                level_ = LogLevel::TRACE;
};

} // namespace cpp109
