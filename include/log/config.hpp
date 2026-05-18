#pragma once

#include "log_level.hpp"
#include "registry.hpp"
#include "sinks/console_sink.hpp"
#include "sinks/file_sink.hpp"
#include "sinks/rotating_file_sink.hpp"
#include "sinks/daily_file_sink.hpp"
#include "sinks/callback_sink.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace cpp109 {

class Logger;
class Sink;

// ──────────────────────────────────────────────────────────
// 模块九：Config（C++ 程序化配置 API）
//
//
// 用法示例：
//   Config cfg;
//   cfg.add_sink("console")
//      .set_class<ConsoleSink>()
//      .set_level(LogLevel::DEBUG);
//   cfg.add_logger("admin")
//      .set_sinks({"file", "console"})
//      .set_level(LogLevel::DEBUG);
//   cfg.apply();   // 应用到全局 Registry
// ──────────────────────────────────────────────────────────

class Config {
public:
    // ── Sink 定义 ────────────────────────────────────
    struct SinkDef {
        std::string name;
        std::string class_name;        // "ConsoleSink" / "RotatingFileSink" ...
        LogLevel    level = LogLevel::INFO;
        std::string formatter;
        // 通用键值属性（filename、max_size_mb 等）
        std::unordered_map<std::string, std::string> properties;

        // 链式构建器
        template<typename SinkType>
        SinkDef& set_class() {
            if constexpr (std::is_same_v<SinkType, ConsoleSink>) {
                class_name = "ConsoleSink";
            } else if constexpr (std::is_same_v<SinkType, FileSink>) {
                class_name = "FileSink";
            } else if constexpr (std::is_same_v<SinkType, RotatingFileSink>) {
                class_name = "RotatingFileSink";
            } else if constexpr (std::is_same_v<SinkType, DailyFileSink>) {
                class_name = "DailyFileSink";
            } else if constexpr (std::is_same_v<SinkType, CallbackSink>) {
                class_name = "CallbackSink";
            }
            return *this;
        }

        SinkDef& set_level(LogLevel level) { level = level; return *this; }
        SinkDef& set_formatter(std::string pattern) { formatter = std::move(pattern); return *this; }
        SinkDef& set_property(std::string key, std::string value) {
            properties[std::move(key)] = std::move(value);
            return *this;
        }
    };

    // ── Logger 定义 ──────────────────────────────────
    struct LoggerDef {
        std::string              name;
        LogLevel                 level = LogLevel::INFO;
        std::vector<std::string> sinks;
        bool                     propagate = true;

        // 链式构建器
        LoggerDef& set_level(LogLevel lvl) { level = lvl; return *this; }
        LoggerDef& set_sinks(std::vector<std::string> s) { sinks = std::move(s); return *this; }
        LoggerDef& set_propagate(bool p) { propagate = p; return *this; }
    };

    // ── Formatter 定义 ───────────────────────────────
    struct FormatterDef {
        std::string name;
        std::string pattern;

        FormatterDef& set_pattern(std::string p) { pattern = std::move(p); return *this; }
    };

    Config();

    // ── Sink 构建器 ──────────────────────────────────
    SinkDef& add_sink(const std::string& name);
    SinkDef& sink(const std::string& name);  // 获取已存在的引用

    // ── Logger 构建器 ────────────────────────────────
    LoggerDef& add_logger(const std::string& name);
    LoggerDef& logger(const std::string& name);

    // ── Formatter 构建器 ─────────────────────────────
    FormatterDef& add_formatter(const std::string& name);
    FormatterDef& formatter(const std::string& name);

    // ── Root 配置 ────────────────────────────────────
    void set_root_level(LogLevel level) { root_level_ = level; }
    void set_root_sinks(std::vector<std::string> sinks) { root_sinks_ = std::move(sinks); }

    // ── 全局 ─────────────────────────────────────────
    void set_disable_existing(bool disable) { disable_existing_ = disable; }

    // ── 应用配置到 Registry ──────────────────────────
    // 调用后 Registry 中的 Logger 将按此配置初始化
    void apply() const;

    // 从环境变量读取字符串，若不存在则使用默认值
    static std::string env(const std::string& key, const std::string& default_val = "");

private:
    friend class ConfigLoader;

    std::shared_ptr<Sink> create_sink_from_def(const SinkDef& def) const;

    std::unordered_map<std::string, SinkDef>      sinks_;
    std::unordered_map<std::string, LoggerDef>    loggers_;
    std::unordered_map<std::string, FormatterDef> formatters_;
    LogLevel                                      root_level_ = LogLevel::INFO;
    std::vector<std::string>                      root_sinks_;
    bool                                          disable_existing_ = false;
};

// ──────────────────────────────────────────────────────────
// 模板实现
// ──────────────────────────────────────────────────────────

inline Config::Config() = default;

inline Config::SinkDef& Config::add_sink(const std::string& name) {
    return sinks_.emplace(name, SinkDef{name}).first->second;
}

inline Config::SinkDef& Config::sink(const std::string& name) {
    return sinks_.at(name);
}

inline Config::LoggerDef& Config::add_logger(const std::string& name) {
    return loggers_.emplace(name, LoggerDef{name}).first->second;
}

inline Config::LoggerDef& Config::logger(const std::string& name) {
    return loggers_.at(name);
}

inline Config::FormatterDef& Config::add_formatter(const std::string& name) {
    return formatters_.emplace(name, FormatterDef{name}).first->second;
}

inline Config::FormatterDef& Config::formatter(const std::string& name) {
    return formatters_.at(name);
}

inline std::string Config::env(const std::string& key, const std::string& default_val) {
    const char* val = std::getenv(key.c_str());
    return val ? std::string(val) : default_val;
}

inline std::shared_ptr<Sink> Config::create_sink_from_def(const SinkDef& def) const {
    if (def.class_name == "ConsoleSink") {
        bool use_stderr = false;
        bool colored = true;
        auto it = def.properties.find("use_stderr");
        if (it != def.properties.end()) use_stderr = (it->second == "true");
        it = def.properties.find("colored");
        if (it != def.properties.end()) colored = (it->second == "true");
        auto sink = std::make_shared<ConsoleSink>(use_stderr, colored);
        if (!def.formatter.empty()) sink->set_pattern(def.formatter);
        sink->set_level(def.level);
        return sink;
    }
    else if (def.class_name == "FileSink") {
        std::string filename;
        bool truncate = false;
        auto it = def.properties.find("filename");
        if (it != def.properties.end()) filename = it->second;
        it = def.properties.find("truncate");
        if (it != def.properties.end()) truncate = (it->second == "true");
        auto sink = std::make_shared<FileSink>(filename, truncate);
        if (!def.formatter.empty()) sink->set_pattern(def.formatter);
        sink->set_level(def.level);
        return sink;
    }
    else if (def.class_name == "RotatingFileSink") {
        std::string filename;
        std::uint64_t max_size_mb = 10;
        int max_files = 5;
        auto it = def.properties.find("filename");
        if (it != def.properties.end()) filename = it->second;
        it = def.properties.find("max_size_mb");
        if (it != def.properties.end()) max_size_mb = std::stoull(it->second);
        it = def.properties.find("max_files");
        if (it != def.properties.end()) max_files = std::stoi(it->second);
        auto sink = std::make_shared<RotatingFileSink>(filename, max_size_mb, max_files);
        if (!def.formatter.empty()) sink->set_pattern(def.formatter);
        sink->set_level(def.level);
        return sink;
    }
    else if (def.class_name == "DailyFileSink") {
        std::string filename_pattern;
        DailyFileSink::RotationPeriod period = DailyFileSink::RotationPeriod::DAY;
        int max_files = 0;
        auto it = def.properties.find("filename_pattern");
        if (it != def.properties.end()) filename_pattern = it->second;
        it = def.properties.find("period");
        if (it != def.properties.end()) {
            if (it->second == "minute") period = DailyFileSink::RotationPeriod::MINUTE;
            else if (it->second == "hour") period = DailyFileSink::RotationPeriod::HOUR;
            else period = DailyFileSink::RotationPeriod::DAY;
        }
        it = def.properties.find("max_files");
        if (it != def.properties.end()) max_files = std::stoi(it->second);
        auto sink = std::make_shared<DailyFileSink>(filename_pattern, period, max_files);
        if (!def.formatter.empty()) sink->set_pattern(def.formatter);
        sink->set_level(def.level);
        return sink;
    }
    else if (def.class_name == "CallbackSink") {
        throw std::runtime_error("CallbackSink cannot be created via Config, use programmatic API instead");
    }
    else {
        throw std::runtime_error("Unknown Sink class: " + def.class_name);
    }
}

inline void Config::apply() const {
    auto& registry = Registry::instance();

    // 按需创建 Sink 缓存
    std::unordered_map<std::string, std::shared_ptr<Sink>> sink_cache;

    // 遍历 loggers_ 表，应用配置
    for (const auto& [name, def] : loggers_) {
        auto logger = registry.get_or_create(name);
        logger->set_level(def.level);
        logger->set_propagate(def.propagate);
        logger->clear_sinks();

        for (const auto& sink_name : def.sinks) {
            // 按需创建 Sink
            if (sink_cache.find(sink_name) == sink_cache.end()) {
                auto it = sinks_.find(sink_name);
                if (it != sinks_.end()) {
                    sink_cache[sink_name] = create_sink_from_def(it->second);
                }
            }
            auto sit = sink_cache.find(sink_name);
            if (sit != sink_cache.end()) {
                logger->add_sink(sit->second);
            }
        }
    }

    // 处理 root logger
    if (!root_sinks_.empty()) {
        auto root = registry.get_or_create("root");
        root->set_level(root_level_);
        root->clear_sinks();

        for (const auto& sink_name : root_sinks_) {
            if (sink_cache.find(sink_name) == sink_cache.end()) {
                auto it = sinks_.find(sink_name);
                if (it != sinks_.end()) {
                    sink_cache[sink_name] = create_sink_from_def(it->second);
                }
            }
            auto sit = sink_cache.find(sink_name);
            if (sit != sink_cache.end()) {
                root->add_sink(sit->second);
            }
        }
    }
}

} // namespace cpp109
