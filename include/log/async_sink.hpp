#pragma once

#include "sink.hpp"
#include "log_event.hpp"
#include "ring_buffer.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <string>
#include <thread>

namespace cpp109 {

template<std::size_t QueueSize = 8192, OverflowPolicy Policy = OverflowPolicy::BLOCK>
class AsyncSink : public Sink {
    static_assert((QueueSize & (QueueSize - 1)) == 0, "QueueSize must be a power of 2");

public:
    explicit AsyncSink(std::shared_ptr<Sink> wrapped);
    ~AsyncSink() override;

    void log(const LogEvent& event) override;
    void flush_impl() override;
    void stop();

    void set_formatter(std::unique_ptr<Formatter> fmt) override;
    void set_level(LogLevel level) noexcept override;
    void set_pattern(const std::string& pattern) override;

    Formatter* formatter() noexcept override;

protected:
    void write(const std::string&, const LogEvent&) override {}

private:
    using QueueItem = LogEvent;

    void worker_loop();

    std::shared_ptr<Sink> wrapped_;
    RingBuffer<QueueItem, QueueSize, Policy> queue_;
    std::thread             worker_;
    std::atomic<bool>       running_{true};
    std::condition_variable cv_;
    std::mutex              cv_mutex_;
};

template<typename SinkType,
         std::size_t QueueSize = 8192,
         OverflowPolicy Policy = OverflowPolicy::BLOCK,
         typename... Args>
std::shared_ptr<AsyncSink<QueueSize, Policy>> make_async_sink(Args&&... sink_args)
{
    auto inner = std::make_shared<SinkType>(std::forward<Args>(sink_args)...);
    return std::make_shared<AsyncSink<QueueSize, Policy>>(std::move(inner));
}

template<std::size_t QueueSize, OverflowPolicy Policy>
AsyncSink<QueueSize, Policy>::AsyncSink(std::shared_ptr<Sink> wrapped)
    : wrapped_(std::move(wrapped))
{
    worker_ = std::thread(&AsyncSink::worker_loop, this);
}

template<std::size_t QueueSize, OverflowPolicy Policy>
AsyncSink<QueueSize, Policy>::~AsyncSink()
{
    stop();
}

template<std::size_t QueueSize, OverflowPolicy Policy>
void AsyncSink<QueueSize, Policy>::stop()
{
    if (running_.exchange(false)) {
        cv_.notify_one();
        worker_.join();
    }
}

template<std::size_t QueueSize, OverflowPolicy Policy>
void AsyncSink<QueueSize, Policy>::log(const LogEvent& event)
{
    if (event.level() < this->level()) return;
    auto copy = event;
    //不加锁，cv阻塞超时自动唤醒worker_loop
    queue_.enqueue(std::move(copy));
    cv_.notify_one();
}

template<std::size_t QueueSize, OverflowPolicy Policy>
void AsyncSink<QueueSize, Policy>::flush_impl()
{
    auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!queue_.empty()) {
        if (std::chrono::steady_clock::now() >= timeout) break;
        std::this_thread::yield();
    }
    wrapped_->flush();
}

template<std::size_t QueueSize, OverflowPolicy Policy>
void AsyncSink<QueueSize, Policy>::worker_loop()
{
    while (running_ || !queue_.empty()) {
        QueueItem item;
        if (queue_.dequeue(item)) {
            try {
                wrapped_->log_unlock(item);
            } catch (...) {}
        } else {
            std::unique_lock<std::mutex> lk(cv_mutex_);
            //阻塞等待100微秒自动唤醒
            cv_.wait_for(lk, std::chrono::microseconds(100));
        }
    }

    wrapped_->flush();
}

template<std::size_t QueueSize, OverflowPolicy Policy>
void AsyncSink<QueueSize, Policy>::set_formatter(std::unique_ptr<Formatter> fmt)
{
    wrapped_->set_formatter(std::move(fmt));
}

template<std::size_t QueueSize, OverflowPolicy Policy>
void AsyncSink<QueueSize, Policy>::set_level(LogLevel level) noexcept
{
    level_ = level;
    wrapped_->set_level(level);
}

template<std::size_t QueueSize, OverflowPolicy Policy>
void AsyncSink<QueueSize, Policy>::set_pattern(const std::string& pattern)
{
    wrapped_->set_pattern(pattern);
}

template<std::size_t QueueSize, OverflowPolicy Policy>
Formatter* AsyncSink<QueueSize, Policy>::formatter() noexcept
{
    return wrapped_->formatter();
}

} // namespace cpp109
