#pragma once

#include "log_level.hpp"
#include "log_event.hpp"
#include "sink.hpp"

#include <atomic>
#include <format>
#include <source_location>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace cpp109 {

class Logger : public std::enable_shared_from_this<Logger> {
public:
    explicit Logger(std::string name) : name_(std::move(name)){}

    template<typename... Args>
    void trace(std::format_string<Args...> fmt, Args&&... args);

    template<typename... Args>
    void debug(std::format_string<Args...> fmt, Args&&... args);

    template<typename... Args>
    void info(std::format_string<Args...> fmt, Args&&... args);

    template<typename... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args);

    template<typename... Args>
    void error(std::format_string<Args...> fmt, Args&&... args);

    template<typename... Args>
    void fatal(std::format_string<Args...> fmt, Args&&... args);

    void log(LogLevel level, std::string formatted_msg,
             std::source_location loc){
                log_impl(level, std::move(formatted_msg), loc);
             }

    void add_sink(std::shared_ptr<Sink> sink){
        std::lock_guard<std::mutex> lock(mutex_);
        sinks_.push_back(sink);
        update_fast_path_unlocked();
    }
    void remove_sink(std::shared_ptr<Sink> sink){
        std::lock_guard<std::mutex> lock(mutex_);
        sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), sink), sinks_.end());
        update_fast_path_unlocked();
    }
    void clear_sinks(){
        std::lock_guard<std::mutex> lock(mutex_);
        sinks_.clear();
        fast_sink_.store(nullptr, std::memory_order_release);
    }

    void set_level(LogLevel level) { level_.store(level, std::memory_order_release); }
    LogLevel level() const noexcept { return level_.load(std::memory_order_acquire); }

    const std::string& name() const noexcept { return name_; }
    void set_name(const std::string& name) { name_ = name; }

    void set_parent(const std::shared_ptr<Logger>& parent) {
        std::lock_guard<std::mutex> lock(mutex_);
        parent_ = parent;
    }
    std::shared_ptr<Logger> parent() const noexcept { return parent_.lock(); }

    void flush(){
        std::lock_guard<std::mutex> lock(mutex_);
        for(const auto& sink : sinks_){
            sink->flush();
        }
    }

    void set_propagate(bool propagate) noexcept { propagate_.store(propagate, std::memory_order_release); }
    bool propagate() const noexcept { return propagate_.load(std::memory_order_acquire); }

private:
    void log_impl(LogLevel level, std::string message, std::source_location loc){
        if(level < this->level()) return;

        static thread_local std::uint64_t tl_tid = platform::current_thread_id();
        LogEvent event = {name_, level, std::move(message), Timestamp(), loc, tl_tid};

        auto fast = fast_sink_.load(std::memory_order_acquire);
        if (fast && parent_.expired()) {
            if (level == LogLevel::FATAL) {
                fast->log(event);
                fast->flush();
                std::abort();
            }
            fast->log(event);
            return;
        }

        std::shared_ptr<Logger> parent;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(level == LogLevel::FATAL){
                dispatch_to_sinks(event);
                flush_unlocked();
                std::abort();
            }

            dispatch_to_sinks(event);

            parent = parent_.lock();
        }

        if(this->propagate() && parent){
            parent->log_impl(level, event.message(), loc);
        }
    }
    void dispatch_to_sinks(const LogEvent& event){
        for(const auto& sink : sinks_){
            sink->log(event);
        }
    }
    void flush_unlocked(){
        for(const auto& sink : sinks_){
            sink->flush();
        }
    }

    void update_fast_path_unlocked(){
        if (sinks_.size() == 1)
            fast_sink_.store(sinks_[0], std::memory_order_release);
        else
            fast_sink_.store(nullptr, std::memory_order_release);
    }

    std::string       name_;
    std::atomic<LogLevel> level_ = LogLevel::INFO;
    std::atomic<bool> propagate_ = true;
    std::weak_ptr<Logger> parent_;
    std::vector<std::shared_ptr<Sink>> sinks_;
    std::atomic<std::shared_ptr<Sink>> fast_sink_;
    std::mutex        mutex_;
};

#define CPP109_LOGGER_METHOD_IMPL(method_name, level_enum)                      \
    template<typename... Args>                                                 \
    void Logger::method_name(std::format_string<Args...> fmt, Args&&... args)  \
    {                                                                          \
        if (level_enum < level_) return;                                       \
        auto loc = std::source_location::current();                            \
        try {                                                                  \
            log_impl(level_enum, std::format(fmt, std::forward<Args>(args)...), loc); \
        } catch (const std::format_error&) {                                   \
            log_impl(level_enum, "[FORMAT_ERROR] fallback", loc);              \
        }                                                                      \
    }

CPP109_LOGGER_METHOD_IMPL(trace, LogLevel::TRACE)
CPP109_LOGGER_METHOD_IMPL(debug, LogLevel::DEBUG)
CPP109_LOGGER_METHOD_IMPL(info,  LogLevel::INFO)
CPP109_LOGGER_METHOD_IMPL(warn,  LogLevel::WARN)
CPP109_LOGGER_METHOD_IMPL(error, LogLevel::ERROR)
CPP109_LOGGER_METHOD_IMPL(fatal, LogLevel::FATAL)

#undef CPP109_LOGGER_METHOD_IMPL

} // namespace cpp109
