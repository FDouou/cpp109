#pragma once

#include "sink.hpp"
#include "log_event.hpp"
#include "ring_buffer.hpp"

#include <atomic>
#include <chrono>
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

protected:
    // 不会被调用
    void write(const std::string&, const LogEvent&) override {}

private:
    using QueueItem = LogEvent;

    void worker_loop();

    std::shared_ptr<Sink> wrapped_;
    RingBuffer<QueueItem, QueueSize, Policy> queue_;
    std::thread             worker_;
    std::atomic<bool>       running_{true};
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
        worker_.join();
    }
}

template<std::size_t QueueSize, OverflowPolicy Policy>
void AsyncSink<QueueSize, Policy>::log(const LogEvent& event)
{
    if (event.level() < this->level()) return;
    queue_.enqueue(event);
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
                wrapped_->log(item);
            } catch (...) {//防止线程崩溃
            }
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    wrapped_->flush();
}

} // namespace cpp109
