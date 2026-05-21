#pragma once

#include "log_event.hpp"
#include "formatter.hpp"
#include "log_level.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace cpp109 {

enum class OverflowPolicy;

template<std::size_t, OverflowPolicy>
class AsyncSink;

class Sink {
    template<std::size_t, OverflowPolicy>
    friend class AsyncSink;

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
        static thread_local std::string tl_buf;
        formatter_->format(event, tl_buf);
        write(tl_buf, event);
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
    void log_unlock(const LogEvent& event) {
        if (event.level() < level_) return;
        static thread_local std::string tl_buf;
        formatter_->format(event, tl_buf);
        write(tl_buf, event);
    }

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
