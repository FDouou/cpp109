#pragma once

#include "logger.hpp"
#include "sinks/console_sink.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace cpp109 {

class Registry {
public:
    static Registry& instance() {
        static Registry inst;
        return inst;
    }

    std::shared_ptr<Logger> get(const std::string& name){
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = loggers_.find(name);
        if(it != loggers_.end()){
            return it->second;
        }
        return nullptr;
    }

    std::shared_ptr<Logger> get_or_create(const std::string& name){
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = loggers_.find(name);
        if(it != loggers_.end()){
            return it->second;
        }
        auto logger = std::make_shared<Logger>(name);
        loggers_[name] = logger;
        setup_hierarchy_unlocked(name, logger);
        return logger;
    }

    std::shared_ptr<Logger> set_default_logger(std::shared_ptr<Logger> logger){
        default_logger_.store(logger, std::memory_order_release);
        default_logger_ptr_.store(logger.get(), std::memory_order_release);
        return logger;
    }

    std::shared_ptr<Logger> default_logger(){
        auto d = default_logger_.load(std::memory_order_acquire);
        if (d) return d;

        std::lock_guard<std::mutex> lock(mutex_);
        d = default_logger_.load(std::memory_order_relaxed);
        if (d) return d;

        d = std::make_shared<Logger>("default");
        auto console = std::make_shared<ConsoleSink>();
        d->add_sink(console);
        default_sinks_.push_back(console);
        default_logger_.store(d, std::memory_order_release);
        default_logger_ptr_.store(d.get(), std::memory_order_release);
        return d;
    }

    // 热路径入口（LOG_* 宏使用）：一次 acquire 裸指针加载，避免
    // atomic<shared_ptr>::load 的引用计数/内部同步开销。
    // 指向单例持有的 Logger，生命周期与进程相同。
    Logger& default_logger_ref(){
        auto* p = default_logger_ptr_.load(std::memory_order_acquire);
        if (p) return *p;
        auto d = default_logger();   // 首次创建，慢路径
        default_logger_ptr_.store(d.get(), std::memory_order_release);
        return *d;
    }

    void set_default_level(LogLevel level){
        auto d = default_logger_.load(std::memory_order_acquire);
        if (d) d->set_level(level);
    }
    void add_default_sink(std::shared_ptr<Sink> sink){
        std::lock_guard<std::mutex> lock(mutex_);
        default_sinks_.push_back(sink);
        auto d = default_logger_.load(std::memory_order_relaxed);
        if (d) d->add_sink(sink);
    }
    void set_pattern(const std::string& pattern){
        std::lock_guard<std::mutex> lock(mutex_);
        for(const auto& sink : default_sinks_){
            sink->set_pattern(pattern);
        }
    }

    void remove_all(){
        std::lock_guard<std::mutex> lock(mutex_);
        loggers_.clear();
    }

    // 全局 flush：提交所有存活线程的未满 batch，并 flush 全部 logger 的 sink。
    // 约束：须在日志写入线程已停止写入后调用（典型场景：进程退出前、线程池 drained 后）。
    void flush_all(){
        detail::flush_all_batches();
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [name, logger] : loggers_) {
            logger->flush();
        }
        if (auto d = default_logger_.load(std::memory_order_acquire)) {
            d->flush();
        }
    }

private:
    Registry() = default;
    ~Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    void setup_hierarchy_unlocked(const std::string& name, std::shared_ptr<Logger> logger){
        auto dot_pos = name.find_last_of('.');
        if(dot_pos != std::string::npos){
            auto parent_name = name.substr(0, dot_pos);
            auto it = loggers_.find(parent_name);
            if(it != loggers_.end()){
                logger->set_parent(it->second);
            }
        }
    }

    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Logger>> loggers_;
    std::atomic<std::shared_ptr<Logger>> default_logger_{nullptr};
    std::atomic<Logger*> default_logger_ptr_{nullptr};   // default_logger_ref 热路径缓存
    std::vector<std::shared_ptr<Sink>> default_sinks_;
};

inline std::shared_ptr<Logger> get_logger(const std::string& name) {
    return Registry::instance().get_or_create(name);
}

} // namespace cpp109
