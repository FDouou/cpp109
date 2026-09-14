#pragma once

#include "sink.hpp"
#include "log_event.hpp"
#include "ring_buffer.hpp"
#include "platform.hpp"
#include "backend.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace cpp109 {

// ── RdtscClock：将 rdtsc 值转换为 wall-clock 纳秒（全局单例，只校准一次）──
class RdtscClock {
public:
    static const RdtscClock& instance() noexcept {
        static const RdtscClock clock;
        return clock;
    }

    std::uint64_t to_ns(std::uint64_t tsc) const noexcept {
        auto delta = tsc - base_tsc_;
#ifdef _MSC_VER
        // MSVC: 用 _umul128 + _udiv128 做 128 位精确除法
        unsigned __int64 hi;
        unsigned __int64 lo = _umul128(delta, 1000000000ULL, &hi);
        unsigned __int64 rem;
        unsigned __int64 quot = _udiv128(hi, lo, tsc_freq_int_, &rem);
        return base_ns_ + static_cast<std::uint64_t>(quot);
#else
        return base_ns_ + static_cast<std::uint64_t>(
            static_cast<double>(delta) * 1e9 / static_cast<double>(tsc_freq_int_));
#endif
    }

private:
    RdtscClock() noexcept {
        base_tsc_  = rdtsc_ns();
        auto bt = std::chrono::system_clock::now();
        base_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
            bt.time_since_epoch()).count();

        auto t1 = std::chrono::steady_clock::now();
        auto c1 = rdtsc_ns();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto c2 = rdtsc_ns();
        auto t2 = std::chrono::steady_clock::now();

        double sec = std::chrono::duration<double>(t2 - t1).count();
        tsc_freq_ = static_cast<double>(c2 - c1) / sec;
        tsc_freq_int_ = static_cast<std::uint64_t>(tsc_freq_);
    }

    std::uint64_t base_tsc_ = 0;
    std::uint64_t base_ns_  = 0;
    double        tsc_freq_ = 1.0;
    std::uint64_t tsc_freq_int_ = 1;
};

// ── AsyncSink：纯队列包装器（不再持有线程）──
// 后台线程由全局 LogBackend 统一提供（默认 1 个），本类只负责：
//   1. 前台写入（log_encoded / log_encoded_batch，无锁 SPSC）
//   2. 被 LogBackend worker 调用的消费接口（has_pending / drain_one / flush_wrapped）
//
// 多线程安全（场景2，2026-09 重构）：
//   目标：多个前台线程共享同一 AsyncSink（同一文件）并发写入不卡死。
//   原实现单 ring 会被多线程并发写坏（SPSC 不变量破坏）。
//   现改为**线程分片**：每个前台线程首次写入时懒创建自己的 SPSC ring。
//      - slots_ 表以 thread_id 为键持有各线程分片（unique_ptr）；
//      - 写者恒为该线程自身 → 每个分片仍是无锁 SPSC；
//      - worker 单后台线程逐分片轮转排空 → 文件只有 worker 一个写者。
//   回收：分片不做线程退出回收（日志线程通常长命），AsyncSink 析构时整表释放。
template<std::size_t QueueCapacity = 1 << 20, OverflowPolicy Policy = OverflowPolicy::BLOCK>
class AsyncSink final : public AsyncSinkBase {
    static_assert((QueueCapacity & (QueueCapacity - 1)) == 0, "capacity must be power of 2");

    struct ThreadSlot {
        ByteRingBuffer<QueueCapacity, Policy> ring;
        // 背压控制：ring 满时 producer 在 cv 上等待（不空转），
        // worker 消费腾出空间后 notify。仅 BLOCK 策略使用。
        std::mutex               cv_mtx;
        std::condition_variable  cv;
        bool                     waiting = false;   // 受 cv_mtx 保护
    };

public:
    explicit AsyncSink(std::shared_ptr<Sink> wrapped)
        : wrapped_(std::move(wrapped))
        , shard_index_(LogBackend::instance().register_sink(this))
    {}

    ~AsyncSink() override {
        LogBackend::instance().unregister_sink(shard_index_, this);
    }

    // ── AsyncSinkBase 接口（前台写入）──
    void log_encoded(const TinyMeta* meta,
                     LogLevel level,
                     std::uint64_t thread_id,
                     std::uint64_t timestamp_tsc,
                     const std::byte* encoded_args, std::uint32_t args_size) override
    {
        enqueue_record(meta, level, thread_id, timestamp_tsc,
                       encoded_args, args_size, true /* notify */);
    }

    void log_encoded_batch(const std::byte* data, std::size_t total_bytes) override {
        if (total_bytes == 0) return;

        // 批内记录可能来自不同前台线程（flush_all_batches 跨线程提交），
        // 不能整批写入同一分片——逐条按其 thread_id 归属分片。
        // 相比单条路径仅省去 sink 级过滤重复判断与 per-record notify（batch 末尾统一一次）。
        std::size_t pos = 0;
        const std::size_t batch_size = total_bytes;
        while (pos < batch_size) {
            const TinyHeader* hdr = reinterpret_cast<const TinyHeader*>(data + pos);
            const std::size_t rec_size = sizeof(TinyHeader) + hdr->args_size;
            enqueue_record(hdr->meta,
                           static_cast<LogLevel>(hdr->level),
                           hdr->thread_id,
                           hdr->timestamp_tsc,
                           data + pos + sizeof(TinyHeader),
                           hdr->args_size,
                           false /* 延迟 notify */);
            pos += rec_size;
        }
        LogBackend::instance().notify(shard_index_);
    }

    // ── Sink 接口（慢路径：按来源位置查持久调用点条目）──
    void log(const LogEvent& event) override {
        if (event.level() < this->level()) return;
        TinyMeta* cs = detail::find_or_create_callsite(
            event.file().data(), event.line(), event.func().data(), nullptr);
        log_encoded(cs, event.level(), event.thread_id(),
                    rdtsc_ns(), nullptr, 0);
    }

    void log_move(LogEvent&& event) override {
        if (event.level() < this->level()) return;
        TinyMeta* cs = detail::find_or_create_callsite(
            event.file().data(), event.line(), event.func().data(), nullptr);
        log_encoded(cs, event.level(), event.thread_id(),
                    rdtsc_ns(), nullptr, 0);
    }

    void flush_impl() override {
        LogBackend::instance().flush_sink(shard_index_, this);
    }

    // 兼容旧接口：原语义为停止独立 worker 线程；全局线程无法单独停止，
    // 退化为"同步排空 ring 并 flush 底层 Sink"。
    void stop() { flush_impl(); }

    // 设置所在后台线程（shard）的 CPU 亲和性
    void set_affinity(std::vector<int> cpu_ids) {
        if (!cpu_ids.empty()) {
            LogBackend::instance().set_shard_affinity(
                shard_index_, cpu_ids.data(), cpu_ids.size());
        }
    }

    void set_formatter(std::unique_ptr<Formatter> fmt) override {
        wrapped_->set_formatter(std::move(fmt));
    }
    void set_level(LogLevel level) noexcept override {
        level_ = level;
        wrapped_->set_level(level);
    }
    void set_pattern(const std::string& pattern) override {
        wrapped_->set_pattern(pattern);
    }
    Formatter* formatter() noexcept override {
        return wrapped_->formatter();
    }

protected:
    void write(const std::string&, const LogEvent&) override {}

private:
    // ── 单条入队（log_encoded 与 batch 共享；do_notify 允许批量合并唤醒）──
    void enqueue_record(const TinyMeta* meta,
                        LogLevel level,
                        std::uint64_t thread_id,
                        std::uint64_t timestamp_tsc,
                        const std::byte* encoded_args, std::uint32_t args_size,
                        bool do_notify)
    {
        if (level < this->level()) return;
        ThreadSlot* slot = current_slot(thread_id);
        const std::size_t total = sizeof(TinyHeader) + args_size;

        std::byte* ptr = nullptr;
        if constexpr (Policy == OverflowPolicy::DROP_NEWEST) {
            ptr = slot->ring.prepare_write(total);
            if (!ptr) return;   // 满则丢弃（DROP 策略）
        } else {
            // BLOCK：ring 满时等待 worker 腾出空间（条件变量，避免 yield 空转）
            while (true) {
                ptr = slot->ring.prepare_write(total);
                if (ptr) break;
                std::unique_lock<std::mutex> lk(slot->cv_mtx);
                slot->waiting = true;
                slot->cv.wait_for(lk, std::chrono::microseconds(100), [&] {
                    return slot->ring.prepare_write(total) != nullptr;
                });
                slot->waiting = false;
            }
        }

        // 直接在目标地址构造 TinyHeader（无栈上临时 header，减少拷贝）
        TinyHeader* hdr = reinterpret_cast<TinyHeader*>(ptr);
        hdr->timestamp_tsc = timestamp_tsc;
        hdr->meta          = meta;
        hdr->thread_id     = thread_id;
        hdr->args_size     = args_size;
        hdr->level         = static_cast<std::uint8_t>(level);
        hdr->flags         = 0;

        if (args_size > 0 && encoded_args) {
            std::memcpy(ptr + sizeof(TinyHeader), encoded_args, args_size);
        }

        slot->ring.commit_write(total);
        if (do_notify) LogBackend::instance().notify(shard_index_);
    }

    // ── 线程分片管理 ──
    // 热路径：每线程 TLS 缓存"本线程的 AsyncSink 分片 + AsyncSink*"，
    // 命中缓存（owner==this）即无锁直达分片 ring；未命中则查表（懒创建）。
    ThreadSlot* current_slot(std::uint64_t thread_id) {
        thread_local struct SlotCache {
            AsyncSink* owner = nullptr;
            ThreadSlot* slot = nullptr;
        } tls;

        if (tls.owner == this && tls.slot) return tls.slot;

        std::lock_guard<std::mutex> lk(slots_mtx_);
        auto it = slots_.find(thread_id);
        if (it == slots_.end())
            it = slots_.emplace(thread_id, std::make_unique<ThreadSlot>()).first;
        tls.owner = this;
        tls.slot = it->second.get();
        return tls.slot;
    }

    // ── LogBackend worker 消费接口 ──
    bool has_pending() const override {
        std::lock_guard<std::mutex> lk(slots_mtx_);
        for (const auto& kv : slots_)
            if (!kv.second->ring.empty()) return true;
        return false;
    }

    // worker 排空循环中逐分片消费：一次调用处理完一个分片的所有积压记录。
    // 持锁只做"选片"，解码与写入在锁外。
    bool drain_one() override {
        ThreadSlot* slot = pick_slot();
        if (!slot) return false;

        bool any = false;
        std::byte* ptr = nullptr;
        while ((ptr = slot->ring.prepare_read()) != nullptr) {
            const TinyHeader* hdr = reinterpret_cast<const TinyHeader*>(ptr);
            decode_and_write(ptr);
            slot->ring.commit_read(sizeof(TinyHeader) + hdr->args_size);
            any = true;
        }
        // 排空后若生产者正等待空间则唤醒
        {
            std::lock_guard<std::mutex> lk(slot->cv_mtx);
            if (slot->waiting) slot->cv.notify_one();
        }
        return any;
    }

    // 在锁内选一个非空分片（轮转），返回裸指针（本对象存活期内有效）
    ThreadSlot* pick_slot() {
        std::lock_guard<std::mutex> lk(slots_mtx_);
        if (slots_.empty()) return nullptr;
        auto it = slots_.find(scan_cursor_);
        if (it == slots_.end()) it = slots_.begin();
        for (std::size_t i = 0; i < slots_.size(); ++i, ++it) {
            if (it == slots_.end()) it = slots_.begin();
            if (!it->second->ring.empty()) {
                scan_cursor_ = it->first;
                return it->second.get();
            }
        }
        return nullptr;
    }

    void decode_and_write(const std::byte* ptr) {
        const TinyHeader* hdr = reinterpret_cast<const TinyHeader*>(ptr);

        thread_local std::string tl_msg;
        tl_msg.clear();
        const TinyMeta* meta = hdr->meta;
        if (meta) {
            DecodeFn fn = meta->decode_fn.load(std::memory_order_relaxed);
            if (fn) {
                const std::byte* args_start = ptr + sizeof(TinyHeader);
                fn(args_start, meta->fmt, tl_msg);
            } else if (meta->fmt) {
                tl_msg = meta->fmt;
            }
        }

        std::uint64_t timestamp_ns = RdtscClock::instance().to_ns(hdr->timestamp_tsc);
        LogEvent event(
            "" /* logger_name */,
            static_cast<LogLevel>(hdr->level),
            timestamp_ns,
            meta ? (meta->file ? meta->file : "") : "",
            meta ? meta->line : 0,
            meta ? (meta->func ? meta->func : "") : "",
            hdr->thread_id,
            tl_msg
        );

        try {
            wrapped_->log_unlock(event, tl_msg);
        } catch (...) {}
    }

    void flush_wrapped() override { wrapped_->flush(); }

    std::shared_ptr<Sink> wrapped_;
    std::size_t shard_index_;

    mutable std::mutex slots_mtx_;
    std::unordered_map<std::uint64_t, std::unique_ptr<ThreadSlot>> slots_;
    std::uint64_t scan_cursor_ = 0;
};

// ── 工厂函数 ──
template<typename SinkType,
         std::size_t QueueCapacity = 1 << 20,
         OverflowPolicy Policy = OverflowPolicy::BLOCK,
         typename... Args>
std::shared_ptr<AsyncSink<QueueCapacity, Policy>> make_async_sink(Args&&... sink_args)
{
    auto inner = std::make_shared<SinkType>(std::forward<Args>(sink_args)...);
    return std::make_shared<AsyncSink<QueueCapacity, Policy>>(std::move(inner));
}

} // namespace cpp109
