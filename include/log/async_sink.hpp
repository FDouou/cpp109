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

    // 纳秒 → TSC 周期（用于唤醒限流阈值换算）
    std::uint64_t ns_to_tsc(std::uint64_t ns) const noexcept {
        return ns * tsc_freq_int_ / 1000000000ULL;
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
        ByteRingBuffer<QueueCapacity> ring;
        // 背压控制：ring 满时 producer 在 cv 上等待（不空转），
        // worker 消费腾出空间后 notify。仅 BLOCK 策略使用。
        std::mutex               cv_mtx;
        std::condition_variable  cv;
        bool                     waiting = false;   // 受 cv_mtx 保护
        // 唤醒限流：该分片上次 notify worker 的 TSC（仅本分片生产者线程写）
        std::uint64_t            last_notify_tsc = 0;
        // worker 轮询提示：生产者提交后置位（relaxed）；worker 排空分片后
        // clear-then-check 清除。使 has_pending / pick_slot 不必扫描 ring
        // 状态行（减少低压场景 worker 轮询对生产者 cache line 的干扰）。
        std::atomic<bool>        pending{false};
    };

public:
    explicit AsyncSink(std::shared_ptr<Sink> wrapped)
        : wrapped_(std::move(wrapped))
        , shard_index_(LogBackend::instance().register_sink(this))
    {
        // 唤醒限流阈值：默认 1ms。direct 模式下每条日志都可能 notify；
        // worker 睡眠时一次 futex wake 约 1~2us，低频日志会被显著放大。
        // 只有「ring 空→非空 且 距上次唤醒 >= 阈值」才真正唤醒；
        // 中间的日志由 worker 的 50us 睡眠超时轮询兜底（落盘延迟上限
        // 一个轮询周期），生产者侧避免每条 futex 唤醒。
        notify_interval_tsc_ = RdtscClock::instance().ns_to_tsc(1'000'000);
    }

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

    void log_encoded_batch(const std::byte* data, std::size_t total_bytes,
                           bool notify) override {
        if (total_bytes == 0) return;

        // 常规路径（tl_batch 单线程）：整批记录同 thread_id → 一次性整块提交
        // （一次 prepare + 一次 memcpy + 一次 commit，替代逐条 N 次原子操作）。
        // flush_all_batches 跨线程批的 thread_id 不同，回退逐条入队。
        std::uint64_t tid = 0;
        bool same_thread = true;
        const bool too_large = (total_bytes > QueueCapacity);
        if (!too_large) {
            std::size_t pos = 0;
            bool first = true;
            while (pos < total_bytes) {
                const TinyHeader* hdr = reinterpret_cast<const TinyHeader*>(data + pos);
                if (first) { tid = hdr->thread_id; first = false; }
                else if (hdr->thread_id != tid) { same_thread = false; break; }
                pos += sizeof(TinyHeader) + hdr->args_size;
            }
        }

        if (too_large || !same_thread) {
            std::size_t pos = 0;
            while (pos < total_bytes) {
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
            if (notify) LogBackend::instance().notify(shard_index_);
            return;
        }

        ThreadSlot* slot = current_slot(tid);
        Chunk c = prepare_slot(slot, total_bytes);
        if (!c.ptr) return;   // DROP_NEWEST：整批丢弃
        slot->ring.write_chunk(c, data, total_bytes);
        slot->ring.commit_write(total_bytes);
        slot->pending.store(true, std::memory_order_relaxed);
        // 满批/显式提交立即唤醒；定时兜底提交由 worker 轮询消费
        if (notify) LogBackend::instance().notify(shard_index_);
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
        // flush 是确定性排空：先把各分片 pending 置位，避免与 producer 的
        // 置位/清除竞态导致漏排。
        {
            std::lock_guard<std::mutex> lk(slots_mtx_);
            for (auto& kv : slots_)
                kv.second->pending.store(true, std::memory_order_relaxed);
        }
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
    using Chunk = typename ByteRingBuffer<QueueCapacity>::Chunk;

    // 空间准备：BLOCK 策略等待 worker 腾空间；DROP_NEWEST 直接失败。
    // 记录/整批大于队列容量时无法入队（返回空 Chunk，调用者丢弃）。
    Chunk prepare_slot(ThreadSlot* slot, std::size_t total) {
        if (total > slot->ring.capacity()) return {};
        if constexpr (Policy == OverflowPolicy::DROP_NEWEST) {
            return slot->ring.prepare_write(total);
        } else {
            auto c = slot->ring.prepare_write(total);
            while (!c.ptr) {
                std::unique_lock<std::mutex> lk(slot->cv_mtx);
                slot->waiting = true;
                slot->cv.wait_for(lk, std::chrono::microseconds(100), [&] {
                    return slot->ring.prepare_write(total).ptr != nullptr;
                });
                c = slot->ring.prepare_write(total);
                slot->waiting = false;
            }
            return c;
        }
    }

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
        const bool was_empty = slot->ring.empty();

        Chunk c = prepare_slot(slot, total);
        if (!c.ptr) return;   // DROP_NEWEST：满则丢弃

        // header 在栈上组装后分段写入（跨尾由 scatter_write 处理）
        TinyHeader hdr;
        hdr.timestamp_tsc = timestamp_tsc;
        hdr.meta          = meta;
        hdr.thread_id     = thread_id;
        hdr.args_size     = args_size;
        hdr.level         = static_cast<std::uint8_t>(level);
        hdr.flags         = 0;

        slot->ring.scatter_write(c, 0,
            reinterpret_cast<const std::byte*>(&hdr), sizeof(TinyHeader));
        if (args_size > 0 && encoded_args) {
            slot->ring.scatter_write(c, sizeof(TinyHeader), encoded_args, args_size);
        }

        slot->ring.commit_write(total);
        slot->pending.store(true, std::memory_order_relaxed);

        if (do_notify) {
            // 唤醒限流：ring 空→非空 且距上次唤醒 >= 阈值才 notify。
            // 高频连续写入时 ring 非空，无需唤醒（worker 在消费）；
            // 中低频把多次唤醒合并，积压由 worker 睡眠超时轮询兜底。
            const std::uint64_t now = rdtsc_ns();
            if (was_empty && (now - slot->last_notify_tsc) >= notify_interval_tsc_) {
                slot->last_notify_tsc = now;
                LogBackend::instance().notify(shard_index_);
            }
        }
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
            if (kv.second->pending.load(std::memory_order_relaxed)) return true;
        return false;
    }

    // worker 排空循环中逐分片消费：一次调用处理完一个分片的所有积压记录。
    // 持锁只做"选片"，解码与写入在锁外。记录跨尾时先线性化到 thread_local
    // 缓冲（worker 非热路径，一次拷贝可忽略），保证解码端数据连续。
    bool drain_one() override {
        ThreadSlot* slot = pick_slot();
        if (!slot) return false;

        bool any = false;
        thread_local static std::vector<std::byte> tl_rec;

        while (true) {
            const std::size_t avail = slot->ring.readable();
            if (avail < sizeof(TinyHeader)) break;

            TinyHeader hdr;
            slot->ring.copy_from(0, &hdr, sizeof(TinyHeader));
            const std::size_t rec = sizeof(TinyHeader) + hdr.args_size;
            if (rec > slot->ring.capacity() || avail < rec) break;  // 防御：不完整/损坏

            tl_rec.resize(rec);
            slot->ring.copy_from(0, tl_rec.data(), rec);
            decode_and_write(tl_rec.data());
            slot->ring.commit_read(rec);
            any = true;
        }

        // clear-then-check：清除后若仍有数据则重新置位，
        // 避免「清标志」与「生产者提交并置位」之间的竞态丢标志。
        slot->pending.store(false, std::memory_order_relaxed);
        if (slot->ring.readable() > 0) {
            slot->pending.store(true, std::memory_order_relaxed);
        }

        // 排空后若生产者正等待空间则唤醒
        {
            std::lock_guard<std::mutex> lk(slot->cv_mtx);
            if (slot->waiting) slot->cv.notify_one();
        }
        return any;
    }

    // 在锁内选一个有 pending 的分片（轮转），返回裸指针（本对象存活期内有效）
    ThreadSlot* pick_slot() {
        std::lock_guard<std::mutex> lk(slots_mtx_);
        if (slots_.empty()) return nullptr;
        auto it = slots_.find(scan_cursor_);
        if (it == slots_.end()) it = slots_.begin();
        for (std::size_t i = 0; i < slots_.size(); ++i, ++it) {
            if (it == slots_.end()) it = slots_.begin();
            if (it->second->pending.load(std::memory_order_relaxed)) {
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
    std::uint64_t notify_interval_tsc_ = 0;   // 唤醒限流阈值（TSC 周期）

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
