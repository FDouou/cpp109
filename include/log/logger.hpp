#pragma once

#include "log_level.hpp"
#include "log_event.hpp"
#include "sink.hpp"
#include "rdtsc_clock.hpp"

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
#define CPP109_BATCH_CAPACITY 16384
#endif
    static constexpr std::size_t BATCH_CAPACITY = CPP109_BATCH_CAPACITY;

    // 默认启用批量缓冲：未满时不触碰 ring、不唤醒 worker，低压场景
    // P50/P99 显著优于逐条入队（见 bench/results/batch16k_vs_direct.txt）。
    // 定义 CPP109_USE_BATCH=0 切回 direct（逐条入队）。
#ifndef CPP109_USE_BATCH
#define CPP109_USE_BATCH 1
#endif

    // 批量缓冲的定时提交阈值（TSC 周期，约 60~100ms 视 TSC 频率）。
    // 稀疏日志不会填满批缓冲，靠该阈值保证最迟提交间隔；阈值偏小时
    // （如 2ms）低速率多线程下每批条数不足 100，flush 样本占比会超过
    // 1% 并推高 P99（实测 x32@0.5M 由 278ns 恶化到 3.1us）。默认取
    // 100ms 量级：间歇式日志可攒数条再提交（实测 P50 再降约 40%），
    // 代价是极稀疏日志落盘延迟上限相应变大。
    // 定时提交不唤醒 worker（由 worker 轮询兜底），避免稀疏日志每条
    // 触发 futex 唤醒。
#ifndef CPP109_BATCH_FLUSH_INTERVAL_TSC
#define CPP109_BATCH_FLUSH_INTERVAL_TSC 250000000ULL
#endif

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
        std::uint64_t  last_flush_tsc = rdtsc_ns();   // 上次提交的 TSC

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

        // notify=false：定时兜底提交不唤醒 worker，由 worker 轮询周期消费。
        // 否则稀疏日志（间隔 > 阈值）每条都触发一次 futex 唤醒。
        void flush(bool notify = true) {
            if (size > 0 && sink) {
                sink->log_encoded_batch(data, size, notify);
                size = 0;
            }
            last_flush_tsc = rdtsc_ns();
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
        parent_set_.store(static_cast<bool>(parent), std::memory_order_release);
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
        if (fast && !parent_set_.load(std::memory_order_acquire)) {
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
            if (!parent) parent_set_.store(false, std::memory_order_relaxed);

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
    std::atomic<bool> parent_set_{false};   // parent_ 是否设置（免 weak_ptr::expired）
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
//   1. Async single sink → 编码后逐条入队（默认 direct）
//      （定义 CPP109_USE_BATCH=1 可改用 4KB 批量缓冲提交）
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

    // fast path 1: async sink -> encode + enqueue
    const bool no_parent = !parent_set_.load(std::memory_order_acquire);
    auto* abase = cached_async_sink_;
    if (abase && no_parent) {
        std::uint32_t args_size = 0;
        if constexpr (sizeof...(Args) > 0) {
            args_size = static_cast<std::uint32_t>(
                detail::compute_encoded_size(args...));
        }

#if CPP109_USE_BATCH
        // 批量缓冲：写 TLS，满 / 超时后一次提交。记录直接编码进批缓冲
        // payload（省去中转拷贝）；本条 TSC 同时用于定时提交判断与时间戳。
        const std::uint64_t now_tsc = rdtsc_ns();
        std::byte* batch_data = detail::tl_batch.data;
        std::size_t& batch_sz = detail::tl_batch.size;
        detail::tl_batch.sink = abase;

        const std::size_t needed = sizeof(TinyHeader) + args_size;
        if (needed > detail::BATCH_CAPACITY) {
            // 单条记录超过批缓冲容量（大字符串等）：批缓冲放不下，先提交
            // 已攒批次，本条降级为 direct 单条入队。编码缓冲超 1KB 自动
            // 回退堆分配；记录若再超 ring 容量则由入队侧按策略丢弃。
            detail::tl_batch.flush();
            std::byte* enc_buf = nullptr;
            if (args_size > 0) {
                enc_buf = detail::get_encode_buffer(args_size);
                detail::encode_args(enc_buf, args...);
            }
            abase->log_encoded(cs, level, tl_tid, now_tsc, enc_buf, args_size);
        } else {
            if (batch_sz + needed > detail::BATCH_CAPACITY && batch_sz > 0) {
                detail::tl_batch.flush();
            } else if (batch_sz > 0 &&
                       now_tsc - detail::tl_batch.last_flush_tsc >=
                           CPP109_BATCH_FLUSH_INTERVAL_TSC) {
                // 稀疏日志兜底：距上次提交超过阈值即提交（不唤醒 worker，
                // 由 worker 轮询兜底消费）。避免长期滞留 TLS。
                detail::tl_batch.flush(/*notify=*/false);
            }

            const std::size_t off = batch_sz;
            batch_sz = off + needed;

            TinyHeader* hdr = reinterpret_cast<TinyHeader*>(batch_data + off);
            hdr->timestamp_tsc = now_tsc;
            hdr->meta           = cs;
            hdr->thread_id      = tl_tid;
            hdr->args_size      = args_size;
            hdr->level          = static_cast<uint8_t>(level);
            hdr->flags          = 0;

            if (args_size > 0) {
                detail::encode_args(batch_data + off + sizeof(TinyHeader), args...);
            }
        }

        if (level == LogLevel::FATAL) {
            detail::flush_deferred_batch();
            abase->flush();
            std::abort();
        }
#else
        // direct：每条立即入队（ring + notify），写入延迟可控
        std::byte* enc_buf = nullptr;
        if (args_size > 0) {
            enc_buf = detail::get_encode_buffer(args_size);
            detail::encode_args(enc_buf, args...);
        }
        abase->log_encoded(cs, level, tl_tid, rdtsc_ns(), enc_buf, args_size);

        if (level == LogLevel::FATAL) {
            abase->flush();
            std::abort();
        }
#endif
        return;
    }

    // fast path 2: sync single sink -> LogEvent + log_move
    auto* fast_sync = cached_async_sink_ ? nullptr
        : fast_sink_.load(std::memory_order_acquire).get();
    if (fast_sync && no_parent) {
        LogEvent event(name_, level, cs->fmt,
                       SourceLoc{cs->file, cs->line, cs->func}, tl_tid,
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
    const SourceLoc loc{cs->file, cs->line, cs->func};
    try {
        log_impl(level, std::vformat(cs->fmt, std::make_format_args(args...)), loc);
    } catch (const std::format_error&) {
        log_impl(level, "[FORMAT_ERROR] fallback", loc);
    }
}

} // namespace cpp109
