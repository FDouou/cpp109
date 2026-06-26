#pragma once

#include "log_event.hpp"
#include "formatter.hpp"
#include "log_level.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

namespace cpp109 {

enum class OverflowPolicy;

template<OverflowPolicy>
class AsyncSink;


class Sink {
    template<OverflowPolicy>
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
        // sync 路径：前台立即格式化
        static thread_local std::string tl_msg;
        event.format_message(tl_msg);
        static thread_local std::string tl_buf;
        formatter_->format(event, tl_buf, tl_msg);
        write(tl_buf, event);
    }
    // 移动版入队——默认 fallback 到 const& 版本，允许子类（AsyncSink）直接 move 入队避免深拷贝
    virtual void log_move(LogEvent&& event) {
        log(event);  // 默认实现：move 退化为 const&
    }
    void flush(){
        std::lock_guard<std::mutex> lock(mutex_);
        flush_impl();
    }

    virtual void set_formatter(std::unique_ptr<Formatter> fmt){
        std::lock_guard<std::mutex> lock(mutex_);
        formatter_ = std::move(fmt);
    }
    virtual Formatter* formatter() noexcept { return formatter_.get(); }

    virtual void set_level(LogLevel level) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        level_ = level; 
    }
    LogLevel level() const noexcept { return level_; }

    virtual void set_pattern(const std::string& pattern){
        std::lock_guard<std::mutex> lock(mutex_);
        formatter_->set_pattern(pattern);
    }
protected:
    // 旧版 log_unlock（向后兼容：内部立即格式化）
    void log_unlock(const LogEvent& event) {
        if (event.level() < level_) return;
        static thread_local std::string tl_msg;
        event.format_message(tl_msg);
        static thread_local std::string tl_buf;
        formatter_->format(event, tl_buf, tl_msg);
        write(tl_buf, event);
    }
    // 新版 log_unlock：后台线程已格式化好 msg，直接给 formatter
    void log_unlock(const LogEvent& event, const std::string& formatted_msg) {
        if (event.level() < level_) return;
        static thread_local std::string tl_buf;
        formatter_->format(event, tl_buf, formatted_msg);
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

// ── AsyncSink 的非模板基类（用于 Logger 做 dynamic_cast 检测）──
// 在 Logger 的快速路径中，若 dynamic_cast<AsyncSinkBase*> 成功，
// 则直接调 log_encoded() 编码入队（零堆分配、rdtsc 时间戳）
class AsyncSinkBase : public Sink {
public:
    virtual ~AsyncSinkBase() = default;

    // 类型擦除的编码入队接口，由 AsyncSink 模板特化实现
    // 前台线程已编码好 args，后台 worker 通过 meta->decode_fn 解码并格式化
    // meta 指向 static const TinyMeta 实例（生命周期永久），包含 file/func/fmt/decode_fn
    virtual void log_encoded(const TinyMeta* meta,
                             LogLevel level,
                             std::uint64_t thread_id,
                             std::uint64_t timestamp_tsc,
                             const std::byte* encoded_args, std::uint32_t args_size) = 0;

    // 批量提交预序列化的日志数据。data 指向连续的多条 (LogRecordHeader + args) 记录，
    // total_bytes 是总字节数。AsyncSink 一次性 prepare_write + memcpy + commit_write，
    // 将多次原子操作合并为一次，大幅降低入队延迟。
    virtual void log_encoded_batch(const std::byte* data, std::size_t total_bytes) = 0;
};

} // namespace cpp109
