#pragma once

#include "platform.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace cpp109 {

class AsyncSinkBase;

// 进程级 alive 标志：backend 析构后拒绝新的注册/注销/flush 访问，
// 防止静态生命周期超长的 sink 在 backend 析构后才销毁导致悬垂访问。
inline std::atomic<bool>& backend_alive_flag() {
    static std::atomic<bool> flag{true};
    return flag;
}

// ── LogBackend：全局后台聚合线程 ──
// 所有 AsyncSink 共享固定数量的 worker 线程（默认 1），
// 替代旧的"每 AsyncSink 一个线程"模型，避免线程数随 sink 数膨胀、
// 大量后台线程抢占 CPU 时间片。
//
// 职责：
//  - 持有全部 AsyncSink 的消费端注册表（按 shard 分片，默认 1 个 shard）
//  - worker 线程轮询本分片所有 ring，聚合解码后逐条写入底层 Sink
//  - 提供注册/注销/唤醒/flush 接口，前台热路径只与自己的 ring 交互
class LogBackend {
public:
    static LogBackend& instance();

    LogBackend(const LogBackend&) = delete;
    LogBackend& operator=(const LogBackend&) = delete;

    ~LogBackend();

    // 注册一个 AsyncSink，返回其所在 shard 编号（默认单线程时为 0）
    std::size_t register_sink(AsyncSinkBase* sink);
    void unregister_sink(std::size_t shard_index, AsyncSinkBase* sink);

    // 前台写入后唤醒所在 shard 的 worker（热路径，仅 cv.notify_one）
    void notify(std::size_t shard_index);

    // 同步排空指定 sink 的 ring 并 flush 底层 Sink（flush_impl 调用）
    void flush_sink(std::size_t shard_index, AsyncSinkBase* sink);

    // 设置指定 shard 后台线程的 CPU 亲和性
    void set_shard_affinity(std::size_t shard_index, const int* cpu_ids, std::size_t count);

    void stop();

private:
    LogBackend();

    struct Shard {
        std::mutex               mtx;     // 保护 sinks 列表；worker 处理时也持锁
        std::condition_variable  cv;
        std::vector<AsyncSinkBase*> sinks;
        std::thread              worker;
        std::atomic<bool>        sleeping{false};  // worker 是否在 cv 上等待
    };

    void ensure_started();
    void shard_worker(Shard* shard);
    static void wake_shard(Shard& shard);

    std::vector<Shard>       shards_;
    std::atomic<bool>        running_{true};
    std::atomic<std::size_t> next_shard_{0};
    std::once_flag           started_;
};

inline LogBackend& LogBackend::instance() {
    static LogBackend backend;
    return backend;
}

inline LogBackend::LogBackend() : shards_(1) {}

inline LogBackend::~LogBackend() {
    backend_alive_flag().store(false, std::memory_order_release);
    stop();
}

inline void LogBackend::ensure_started() {
    std::call_once(started_, [this] {
        for (auto& shard : shards_) {
            shard.worker = std::thread(&LogBackend::shard_worker, this, &shard);
        }
    });
}

inline std::size_t LogBackend::register_sink(AsyncSinkBase* sink) {
    if (!backend_alive_flag().load(std::memory_order_acquire)) return 0;
    ensure_started();
    const auto idx = next_shard_.fetch_add(1, std::memory_order_relaxed) % shards_.size();
    auto& shard = shards_[idx];
    {
        std::lock_guard<std::mutex> lk(shard.mtx);
        shard.sinks.push_back(sink);
    }
    wake_shard(shard);
    return idx;
}

inline void LogBackend::unregister_sink(std::size_t shard_index, AsyncSinkBase* sink) {
    if (!backend_alive_flag().load(std::memory_order_acquire)) return;
    auto& shard = shards_[shard_index];
    {
        std::lock_guard<std::mutex> lk(shard.mtx);
        auto& v = shard.sinks;
        v.erase(std::remove(v.begin(), v.end(), sink), v.end());
    }
    wake_shard(shard);
}

inline void LogBackend::notify(std::size_t shard_index) {
    auto& shard = shards_[shard_index];
    // 热路径：仅当 worker 确实在睡眠等待时才触发唤醒，避免每次 notify
    // 都与 worker 争抢 mutex（P99 长尾的主因）
    if (shard.sleeping.load(std::memory_order_acquire)) {
        shard.cv.notify_one();
    }
}

inline void LogBackend::wake_shard(Shard& shard) {
    if (shard.sleeping.load(std::memory_order_acquire)) {
        shard.cv.notify_one();
    }
}

inline void LogBackend::flush_sink(std::size_t shard_index, AsyncSinkBase* sink) {
    if (!backend_alive_flag().load(std::memory_order_acquire)) return;
    auto& shard = shards_[shard_index];
    std::unique_lock<std::mutex> lk(shard.mtx);
    while (sink->drain_one()) {}
    sink->flush_wrapped();
}

inline void LogBackend::set_shard_affinity(std::size_t shard_index, const int* cpu_ids, std::size_t count) {
    auto& shard = shards_[shard_index];
    if (shard.worker.joinable()) {
        platform::set_thread_affinity(shard.worker.native_handle(), cpu_ids, count);
    }
}

inline void LogBackend::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    for (auto& shard : shards_) wake_shard(shard);
    for (auto& shard : shards_) {
        if (shard.worker.joinable()) shard.worker.join();
    }
}

inline void LogBackend::shard_worker(Shard* shard) {
    while (true) {
        bool progress = false;
        bool has_data = false;
        {
            std::unique_lock<std::mutex> lk(shard->mtx);
            for (auto* sink : shard->sinks) {
                while (sink->drain_one()) progress = true;
                if (sink->has_pending()) has_data = true;
            }
        }
        if (!running_.load(std::memory_order_acquire) && !has_data) break;
        if (!progress) {
            // 进入睡眠前置位 sleeping；notify() 据此判断是否需要唤醒
            shard->sleeping.store(true, std::memory_order_release);
            std::unique_lock<std::mutex> lk(shard->mtx);
            shard->cv.wait_for(lk, std::chrono::microseconds(50), [&] {
                if (!running_.load(std::memory_order_acquire)) return true;
                for (auto* sink : shard->sinks)
                    if (sink->has_pending()) return true;
                return false;
            });
            shard->sleeping.store(false, std::memory_order_release);
        }
    }
    // 退出前 flush 所有底层 Sink
    std::unique_lock<std::mutex> lk(shard->mtx);
    for (auto* sink : shard->sinks)
        sink->flush_wrapped();
}

} // namespace cpp109
