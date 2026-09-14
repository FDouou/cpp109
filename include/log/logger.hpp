#pragma once

#include "log_level.hpp"
#include "log_event.hpp"
#include "sink.hpp"

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <format>
#include <source_location>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace cpp109 {

// Batch buffer for foreground thread: accumulate multiple records and submit
// as one batch to reduce atomic RMW on the ring buffer.
namespace detail {
    // 前台线程批量缓冲容量；可用 CPP109_BATCH_CAPACITY 覆盖（基准实验用）
#ifndef CPP109_BATCH_CAPACITY
#define CPP109_BATCH_CAPACITY 4096
#endif
    static constexpr std::size_t BATCH_CAPACITY = CPP109_BATCH_CAPACITY;

    struct BatchBuffer;

    // 全局活跃 batch 注册表：用于进程退出前的"全线程 flush"。
    // 只在 batch 构造/析构时加锁（每线程各一次），热路径无锁。
    inline std::mutex& batch_registry_mutex() {
        static std::mutex m;
        return m;
    }
    inline std::vector<BatchBuffer*>& active_batches() {
        static std::vector<BatchBuffer*> v;
        return v;
    }

    // 前台线程的批量提交缓冲区。线程退出（thread_local 析构）时自动提交
    // 未满批次，避免程序退出时最后一批日志残留在 thread_local 中丢失。
    struct BatchBuffer {
        std::byte      data[BATCH_CAPACITY];
        std::size_t    size = 0;
        AsyncSinkBase* sink = nullptr;

        BatchBuffer() {
            std::lock_guard<std::mutex> lk(batch_registry_mutex());
            active_batches().push_back(this);
        }
        ~BatchBuffer() {
            flush();
            std::lock_guard<std::mutex> lk(batch_registry_mutex());
            auto& v = active_batches();
            v.erase(std::remove(v.begin(), v.end(), this), v.end());
        }

        void flush() {
            if (size > 0 && sink) {
                sink->log_encoded_batch(data, size);
                size = 0;
            }
        }
    };

    inline thread_local BatchBuffer tl_batch{};

    inline void flush_deferred_batch() {
        tl_batch.flush();
    }

    // 提交所有存活线程的未满批次。
    // 约束：须在全部日志写入线程已停止写入后调用（如进程退出前、线程池 drained 后），
    // 否则会与正在写入的线程产生数据竞争。
    inline void flush_all_batches() {
        std::lock_guard<std::mutex> lk(batch_registry_mutex());
        for (auto* b : active_batches()) b->flush();
    }
} // namespace detail

class Logger : public std::enable_shared_from_this<Logger> {
public:
    explicit Logger(std::string name) : name_(std::move(name)){}

    // 唯一写入入口，由 LOG_* / LOG_*_TO 宏在调用点生成 static TinyMeta 后调用。
    // 直接调用会被编译期格式串校验通过，但 file/line/func 需要调用方自备。
    template<typename... Args>
    void log_at(LogLevel level, TinyMeta* cs,
                std::format_string<Args...> fmt, Args&&... args);

    void log(LogLevel level, std::string formatted_msg,
             std::source_location loc){
                log_impl(level, std::move(formatted_msg),
                         SourceLoc{loc.file_name(), static_cast<int>(loc.line()),
                                   loc.function_name()});
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
            fast_sink_.store(std::shared_ptr<Sink>{nullptr}, std::memory_order_release);
        cached_async_sink_ = nullptr;
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
        detail::flush_deferred_batch();
        std::lock_guard<std::mutex> lock(mutex_);
        for(const auto& sink : sinks_){
            sink->flush();
        }
    }

    void set_propagate(bool propagate) noexcept { propagate_.store(propagate, std::memory_order_release); }
    bool propagate() const noexcept { return propagate_.load(std::memory_order_acquire); }

private:
    // 快路径核心：三个入口共享
    //   log_at（宏）已做编译期校验；直接 API 由 LocFmt 构造时校验
    template<typename... Args>
    void log_dispatch(LogLevel level, TinyMeta* cs, Args&&... args);

    void log_impl(LogLevel level, std::string message, SourceLoc loc){
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
            parent = parent_.lock();

            // 无 sink 且不会 propagate 给 parent：日志必然丢失，一次性警告
            if (sinks_.empty() && !(propagate() && parent)) {
                warn_no_sink_once();
            }

            if(level == LogLevel::FATAL){
                dispatch_to_sinks(event);
                flush_unlocked();
                std::abort();
            }

            dispatch_to_sinks(event);
        }

        if(this->propagate() && parent){
            parent->log_impl(level, event.message(), loc);
        }
    }
    // 无 sink 警告只输出一次，避免刷屏；仅慢路径（已注定要格式化）检查
    void warn_no_sink_once(){
        bool expected = false;
        if (warned_no_sink_.compare_exchange_strong(expected, true,
                                                    std::memory_order_relaxed)) {
            std::fprintf(stderr,
                         "[cpp109] logger '%s' has no sinks and no parent; "
                         "messages are dropped. Call add_sink() or set_parent().\n",
                         name_.c_str());
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
        if (sinks_.size() == 1) {
            fast_sink_.store(sinks_[0], std::memory_order_release);
            cached_async_sink_ = dynamic_cast<AsyncSinkBase*>(sinks_[0].get());
        } else {
        fast_sink_.store(std::shared_ptr<Sink>{nullptr}, std::memory_order_release);
            cached_async_sink_ = nullptr;
        }
    }

    std::string       name_;
    std::atomic<LogLevel> level_ = LogLevel::INFO;
    std::atomic<bool> propagate_ = true;
    std::atomic<bool> warned_no_sink_{false};
    std::weak_ptr<Logger> parent_;
    std::vector<std::shared_ptr<Sink>> sinks_;
    std::atomic<std::shared_ptr<Sink>> fast_sink_{nullptr};
    AsyncSinkBase* cached_async_sink_ = nullptr;
    std::mutex        mutex_;
};

// ─── LOGGER FAST PATH ────────────────────────────────────────────
// 写入唯一入口为 LOG_* / LOG_*_TO 宏：宏在用户文件生成 static TinyMeta
// （编译期常量地址），无需查找。Logger::log_at 校验编译期格式串后转发。
// log_at 内分三级：
//   1. Async single sink → TinyHeader + 批量缓冲（32B header，零堆分配）
//   2. Sync single sink  → LogEvent + log_move（超 SBO 才堆分配）
//   3. Fallback（多 sink / 有 parent）→ log_impl（格式化后分发）

template<typename... Args>
void Logger::log_at(LogLevel level, TinyMeta* cs,
                    std::format_string<Args...> fmt, Args&&... args)
{
    (void)fmt;   // 仅用于编译期格式串校验；运行时数据全部来自 cs
    log_dispatch(level, cs, std::forward<Args>(args)...);
}

template<typename... Args>
void Logger::log_dispatch(LogLevel level, TinyMeta* cs, Args&&... args)
{
    if (level < level_.load(std::memory_order_acquire)) return;

    // decode_fn 只依赖参数类型组合，每个调用点恒定：首次调用惰性写入。
    // 同一调用点的所有线程写入相同的值，atomic 消除数据竞争；
    // worker 经 ring 的 release/acquire 同步后读取。
    if constexpr (sizeof...(Args) > 0) {
        if (cs->decode_fn.load(std::memory_order_relaxed) == nullptr) {
            cs->decode_fn.store(&detail::decode_and_format<std::decay_t<Args>...>,
                                std::memory_order_relaxed);
        }
    }

    static thread_local std::uint64_t tl_tid = platform::current_thread_id();
    const char* fmt_str = cs->fmt;
    SourceLoc loc{cs->file, cs->line, cs->func};

    // fast path 1: async sink -> TinyHeader + direct log_encoded
    auto* abase = cached_async_sink_;
    if (abase && parent_.expired()) {
        std::uint32_t args_size = 0;
        if constexpr (sizeof...(Args) > 0) {
            args_size = static_cast<std::uint32_t>(
                detail::compute_encoded_size(args...));
        }
        std::byte* enc_buf = nullptr;
        if (args_size > 0) {
            enc_buf = detail::get_encode_buffer(args_size);
            detail::encode_args(enc_buf, args...);
        }
        // 批量缓冲区：积累到 4KB 后一次提交（线程退出时自动提交剩余）
        const std::size_t needed = sizeof(TinyHeader) + args_size;
        std::byte* batch_data = detail::tl_batch.data;
        std::size_t& batch_sz = detail::tl_batch.size;
        detail::tl_batch.sink = abase;

        if (batch_sz + needed > detail::BATCH_CAPACITY && batch_sz > 0) {
            detail::tl_batch.flush();
        }

        const std::size_t off = batch_sz;
        batch_sz = off + needed;

        TinyHeader* hdr = reinterpret_cast<TinyHeader*>(batch_data + off);
        hdr->timestamp_tsc = rdtsc_ns();
        hdr->meta           = cs;
        hdr->thread_id      = tl_tid;
        hdr->args_size      = args_size;
        hdr->level          = static_cast<uint8_t>(level);
        hdr->flags          = 0;

        if (args_size > 0) {
            std::memcpy(batch_data + off + sizeof(TinyHeader), enc_buf, args_size);
        }

        if (level == LogLevel::FATAL) {
            detail::flush_deferred_batch();
            abase->flush();
            std::abort();
        }
        return;
    }

    // fast path 2: sync single sink -> LogEvent + log_move
    auto* fast_sync = cached_async_sink_ ? nullptr
        : fast_sink_.load(std::memory_order_acquire).get();
    if (fast_sync && parent_.expired()) {
        LogEvent event(name_, level, fmt_str, loc, tl_tid,
                       std::forward<Args>(args)...);
        fast_sync->log_move(std::move(event));
        if (level == LogLevel::FATAL) {
            fast_sync->flush();
            std::abort();
        }
        return;
    }

    // slow path: multi sink or with parent -> log_impl
    // 编译期校验已由入口完成，这里用 vformat（运行期格式串）
    try {
        log_impl(level, std::vformat(fmt_str, std::make_format_args(args...)), loc);
    } catch (const std::format_error&) {
        log_impl(level, "[FORMAT_ERROR] fallback", loc);
    }
}

} // namespace cpp109
