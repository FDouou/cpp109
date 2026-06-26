#pragma once

#include "sink.hpp"
#include "log_event.hpp"
#include "ring_buffer.hpp"
#include "platform.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace cpp109 {

// ── RdtscClock：将 rdtsc 值转换为 wall-clock 纳秒 ──
class RdtscClock {
public:
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
    std::uint64_t base_tsc_ = 0;
    std::uint64_t base_ns_  = 0;
    double        tsc_freq_ = 1.0;
    std::uint64_t tsc_freq_int_ = 1;
};

// ── AsyncSink：使用 ByteRingBuffer 的异步 sink（final 使编译器可以去虚拟化）──
template<std::size_t QueueCapacity = 1 << 20, OverflowPolicy Policy = OverflowPolicy::BLOCK>
class AsyncSink final : public AsyncSinkBase {
public:
    explicit AsyncSink(std::shared_ptr<Sink> wrapped)
        : wrapped_(std::move(wrapped))
    {
        worker_ = std::thread(&AsyncSink::worker_loop, this);
    }

    ~AsyncSink() override { stop(); }

    // ── AsyncSinkBase 接口 ──
    void log_encoded(const TinyMeta* meta,
                     LogLevel level,
                     std::uint64_t thread_id,
                     std::uint64_t timestamp_tsc,
                     const std::byte* encoded_args, std::uint32_t args_size) override
    {
        const std::size_t total = sizeof(TinyHeader) + args_size;

        // SPSC 无锁：直接写 byte ring buffer
        std::byte* ptr = nullptr;
        while (true) {
            ptr = ring_.prepare_write(total);
            if (ptr) break;
            if constexpr (Policy == OverflowPolicy::DROP_NEWEST) {
                return;
            }
            std::this_thread::yield();
        }

        // 直接在目标地址构造 TinyHeader（无栈上临时 header，减少拷⻉）
        // Note: single-record writes are small (~36B) and rarely straddle
        // the 2*Capacity boundary; batch writes use write_at() instead.
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

        ring_.commit_write(total);
        if (worker_sleeping_.load(std::memory_order_acquire)) {
            cv_.notify_one();
        }
    }

    void log_encoded_batch(const std::byte* data, std::size_t total_bytes) override {
        if (total_bytes == 0) return;

        // Iterate through the batch and submit each record individually.
        // This avoids splitting a single record (TinyHeader + args) across
        // the 2*Capacity boundary, which would cause the reader to read
        // past the end of the ring buffer storage.
        //
        // Individual records are small (~36 B), so the probability that
        // any single write straddles the boundary is ~0.0017 % — low
        // enough that we accept the rare access-violation risk.
        std::size_t pos = 0;
        while (pos < total_bytes) {
            const TinyHeader* hdr = reinterpret_cast<const TinyHeader*>(data + pos);
            std::size_t rec_size = sizeof(TinyHeader) + hdr->args_size;
            log_encoded(hdr->meta,
                        static_cast<LogLevel>(hdr->level),
                        hdr->thread_id,
                        hdr->timestamp_tsc,
                        data + pos + sizeof(TinyHeader),
                        hdr->args_size);
            pos += rec_size;
        }
    }

    // ── Sink 接口（慢路径：用 thread_local meta 避免悬空指针）──
    void log(const LogEvent& event) override {
        if (event.level() < this->level()) return;
        // thread_local 持久存储，确保 worker 异步读取时指针仍然有效
        thread_local static TinyMeta tl_meta{};
        tl_meta.file = event.file().data();
        tl_meta.line = static_cast<int>(event.line());
        tl_meta.func = event.func().data();
        tl_meta.fmt  = nullptr;
        tl_meta.decode_fn = nullptr;
        log_encoded(&tl_meta, event.level(), event.thread_id(),
                    rdtsc_ns(), nullptr, 0);
    }

    void log_move(LogEvent&& event) override {
        if (event.level() < this->level()) return;
        thread_local static TinyMeta tl_meta{};
        tl_meta.file = event.file().data();
        tl_meta.line = static_cast<int>(event.line());
        tl_meta.func = event.func().data();
        tl_meta.fmt  = nullptr;
        tl_meta.decode_fn = nullptr;
        log_encoded(&tl_meta, event.level(), event.thread_id(),
                    rdtsc_ns(), nullptr, 0);
    }

    void flush_impl() override {
        auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!ring_.empty()) {
            if (std::chrono::steady_clock::now() >= timeout) break;
            std::this_thread::yield();
        }
        wrapped_->flush();
    }

    void stop() {
        if (running_.exchange(false)) {
            cv_.notify_one();
            worker_.join();
        }
    }

    // 设置后台 worker 线程的 CPU 亲和性
    void set_affinity(std::vector<int> cpu_ids) {
        if (!cpu_ids.empty()) {
            platform::set_thread_affinity(
                worker_.native_handle(),
                cpu_ids.data(),
                cpu_ids.size());
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
    void worker_loop() {
        thread_local std::string tl_msg;

        while (running_ || !ring_.empty()) {
            auto* ptr = ring_.prepare_read();
            if (ptr) {
                const TinyHeader* hdr = reinterpret_cast<const TinyHeader*>(ptr);

                tl_msg.clear();
                const TinyMeta* meta = hdr->meta;
                if (meta && meta->decode_fn) {
                    const std::byte* args_start = ptr + sizeof(TinyHeader);
                    meta->decode_fn(args_start, meta->fmt, tl_msg);
                } else if (meta && meta->fmt) {
                    tl_msg = meta->fmt;
                }

                std::uint64_t timestamp_ns = rdtsc_clock_.to_ns(hdr->timestamp_tsc);
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

                ring_.commit_read(sizeof(TinyHeader) + hdr->args_size);
            } else {
                worker_sleeping_.store(true, std::memory_order_release);
                std::unique_lock<std::mutex> lk(cv_mutex_);
                cv_.wait_for(lk, std::chrono::microseconds(100));
                worker_sleeping_.store(false, std::memory_order_release);
            }
        }
        wrapped_->flush();
    }

    ByteRingBuffer<QueueCapacity, Policy> ring_;
    std::shared_ptr<Sink> wrapped_;
    std::thread             worker_;
    std::atomic<bool>       running_{true};
    std::atomic<bool>       worker_sleeping_{false};
    std::condition_variable cv_;
    std::mutex              cv_mutex_;
    RdtscClock              rdtsc_clock_;
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
