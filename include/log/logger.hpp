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
    // 前台线程批量缓冲容量；可用 CPP109_BATCH_CAPACITY 覆盖（基准实验用）。
    // 16KB ≈ 100~160 条典型日志行（~100-200B），在"减少原子操作/唤醒次数"
    // 与"线程局部内存占用 / 突发可见延迟"之间取平衡；早期为 4KB。
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

    // 批量缓冲的【惰性】定时提交阈值（TSC 周期，约 100ms 视 TSC 频率）。
    //
    // 重要：这个阈值不是"落盘延迟上界"，而是"下次写入时的最小提交间隔"。
    // 阈值判定写在 log_dispatch 内部，只有该线程【下一次写日志】时才执行；
    // 库内没有独立定时器，worker 也不碰 TLS。因此线程停写（长计算/阻塞
    // IO/idle/**假死**）时，批次里的日志会无限期滞留到"下次写入或线程
    // 退出"——落盘延迟没有上限。这是批量提交的固有属性：持久化承诺从
    // "进入 ring"起算，TLS batch 内不在承诺范围。
    //
    // 需要低延迟请改用 CPP109_USE_BATCH=0（direct，延迟 ≤ worker 轮询周期）
    // 或在检查点手动 logger->flush()。不为该滞留引入跨线程抢救（watchdog），
    // 理由见 QA.md 1.11。
    //
    // 阈值偏小时（如 2ms）低速率多线程下每批条数不足 100，flush 样本占比
    // 会超过 1% 并推高 P99（实测 x32@0.5M 由 278ns 恶化到 3.1us）。默认取
    // 100ms 量级。定时提交不唤醒 worker（由 worker 50μs 轮询兜底），避免
    // 稀疏日志每条触发 futex 唤醒。
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
    //
    // ── 设计前提（务必看清）──
    // tl_batch 是【线程私有、跨所有 logger 共享】的，而 sink 字段是【单指针】。
    // 这依赖两个前提，快路径只强制了第一个：
    //   约束① 一个 logger 只挂一个 async sink —— 由 Logger::update_fast_path
    //         强制（多 sink 直接退化慢路径，正确但慢）；
    //   约束② 一个线程在一个批周期内只用一个 async logger —— 无任何机制。
    // 违反约束②（同一线程交替写两个各绑独立 AsyncSink 的 logger）时，先写者
    // 的记录会在提交时被整批交给后写者的 sink → 落进错误的文件。修法见
    // log_dispatch：换 sink 时先用旧 sink 提交旧数据（优雅降级）。
    struct BatchBuffer {
        std::byte      data[BATCH_CAPACITY];
        std::size_t    size = 0;
        AsyncSinkBase* sink = nullptr;
        std::uint64_t  last_flush_tsc = rdtsc_ns();   // 上次提交的 TSC

        // 本线程写过的 async sink（**强引用**，保证 ~BatchBuffer 上报时 sink 仍存活，
        // 避免"线程独占 logger、logger 先于 thread_local 析构"导致的悬垂）。
        // 线程退出时据此通知各 sink 回收本线程分片。仅在换 sink 时登记（稳态零开销）；
        // 超过上限后不再追踪（对应分片不回收，退化为随 AsyncSink 析构整体释放）。
        static constexpr std::size_t kMaxTrackedSinks = 8;
        std::shared_ptr<AsyncSinkBase> tracked[kMaxTrackedSinks];
        std::size_t    tracked_count = 0;
        std::uint64_t  tid = platform::current_thread_id();   // 本线程 id（= 分片 key）

        BatchBuffer() {
            std::lock_guard<std::mutex> lk(batch_registry_mutex());
            active_batches().push_back(this);
        }
        ~BatchBuffer() {
            flush();
            // 线程退出上报：通知所有写过的 sink——本线程分片已无写者，worker 排空后
            // 可回收。tracked 是强引用，此处 sink 保证存活。信号来自线程自身析构，
            // 不需要 worker 去猜线程死活，也天然规避 tid 复用（上报发生在 OS 释放
            // tid 之前）。
            for (std::size_t i = 0; i < tracked_count; ++i)
                if (tracked[i]) tracked[i]->retire_shard(tid);
            std::lock_guard<std::mutex> lk(batch_registry_mutex());
            auto& v = active_batches();
            v.erase(std::remove(v.begin(), v.end(), this), v.end());
        }

        // 登记本线程写过的 async sink（幂等）。仅换 sink 时调用，热路径稳态不触碰。
        void track_sink(const std::shared_ptr<Sink>& s) {
            auto* base = dynamic_cast<AsyncSinkBase*>(s.get());
            if (!base) return;
            for (std::size_t i = 0; i < tracked_count; ++i)
                if (tracked[i].get() == base) return;
            if (tracked_count < kMaxTrackedSinks)
                tracked[tracked_count++] = std::static_pointer_cast<AsyncSinkBase>(s);
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

        // 首次检测到"一个线程在一个批周期内使用多个 async logger"时提示一次。
        // 该用法已由 log_dispatch 优雅降级（切换时先用旧 sink 提交旧数据），
        // 功能正确，只是批提交收益下降。用一次性告警而非 assert abort——
        // 偶发交替是合理用法，不应打断。
        void warn_sink_switch_once() {
            if (!warned_sink_switch) {
                warned_sink_switch = true;
                std::fprintf(stderr,
                    "[cpp109] a thread wrote to multiple async loggers within one "
                    "batch cycle; the pending batch was flushed on sink switch "
                    "(output correct, throughput lower). Prefer one async logger "
                    "per thread, or share a single AsyncSink.\n");
            }
        }

        bool warned_sink_switch = false;
    };

    inline thread_local BatchBuffer tl_batch{};

    inline void flush_deferred_batch() {
        tl_batch.flush();
    }

    // 提交所有存活线程的未满批次。
    // 约束：须在全部日志写入线程已停止写入后调用（如进程退出前、线程池 drained 后），
    // 否则会与正在写入的线程产生数据竞争（size 非原子，且批内记录可能半写）。
    // 注意：调用者线程替其他线程提交时，会经由 AsyncSink::current_slot 的 TLS
    // 缓存——缓存 key 是 (sink 实例)，不含 thread_id，故同一次 flush_all 中多个
    // 线程的记录会被路由到同一分片。遵守上述约束时数据不丢（header 内 thread_id
    // 正确），但分片归属与落盘顺序会变化。
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

    // %n 需要 logger 名：调用点首次经过时惰性写入到 TinyMeta（per-callsite
    // 静态存储、进程级存活，异步 worker 解码时读取）。稳态只做一次 relaxed
    // load 判定，近零开销。前提：同一调用点固定同一 logger。
    if (!cs->logger_name_set.load(std::memory_order_relaxed)) {
        bool expected = false;
        if (cs->logger_name_set.compare_exchange_strong(
                expected, true, std::memory_order_release,
                std::memory_order_relaxed)) {
            std::snprintf(cs->logger_name, TinyMeta::kLoggerNameCap,
                          "%s", name_.c_str());
        }
    }

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

        // 约束②（见 BatchBuffer）：tl_batch 跨所有 logger 共享、sink 为单指针。
        // 若本线程在一个批周期内切到另一个 async sink，必须先用【旧 sink】提交
        // 缓冲里的旧数据，否则旧记录会被后面整批交给新 sink，落进错误的文件。
        // 正常用法（一线程一 async logger）下 sink 恒等于 abase，仅一次指针比较。
        // 换 sink 时顺带登记该 sink（供线程退出时上报回收分片）。
        if (detail::tl_batch.sink != abase) {
            if (batch_sz > 0) {
                detail::tl_batch.flush();
                detail::tl_batch.warn_sink_switch_once();
            }
            detail::tl_batch.track_sink(fast_sink_.load(std::memory_order_acquire));
            detail::tl_batch.sink = abase;
        }

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
        // direct：每条立即入队（ring + notify），写入延迟可控。
        // 换 sink 时登记（供线程退出回收分片）。
        if (detail::tl_batch.sink != abase) {
            detail::tl_batch.track_sink(fast_sink_.load(std::memory_order_acquire));
            detail::tl_batch.sink = abase;
        }
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
