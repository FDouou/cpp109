#pragma once

#include "logger.hpp"
#include "sinks/console_sink.hpp"

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

    // ── Logger 生命周期管理 ──────────────────────────────
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
        std::lock_guard<std::mutex> lock(mutex_);
        default_logger_ = logger;
        return logger;
    }

    std::shared_ptr<Logger> default_logger(){
        std::lock_guard<std::mutex> lock(mutex_);
        if(!default_logger_){
            default_logger_ = std::make_shared<Logger>("default");
            auto console = std::make_shared<ConsoleSink>();
            default_logger_->add_sink(console);
            default_sinks_.push_back(console);
        }
        return default_logger_;
    }

    // ── 全局快捷配置 ─────────────────────────────────────
    void set_default_level(LogLevel level){
        std::lock_guard<std::mutex> lock(mutex_);
        if(default_logger_){
            default_logger_->set_level(level);
        }
    }
    void add_default_sink(std::shared_ptr<Sink> sink){
        std::lock_guard<std::mutex> lock(mutex_);
        default_sinks_.push_back(sink);
        if(default_logger_){
            default_logger_->add_sink(sink);
        }
    }
    void set_pattern(const std::string& pattern){
        std::lock_guard<std::mutex> lock(mutex_);
        for(const auto& sink : default_sinks_){
            sink->set_pattern(pattern);
        }
    }
           

    // ── 清空所有 logger ──────────────────────────────────
    void remove_all(){
        std::lock_guard<std::mutex> lock(mutex_);
        loggers_.clear();
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
    std::shared_ptr<Logger> default_logger_;
    std::vector<std::shared_ptr<Sink>> default_sinks_;
};

inline std::shared_ptr<Logger> get_logger(const std::string& name) {
    return Registry::instance().get_or_create(name);
}

} // namespace cpp109
