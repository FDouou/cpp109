#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <thread>

namespace cpp109 {

enum class OverflowPolicy {
    BLOCK,
    DROP_NEWEST,
};

template<typename T, std::size_t Size = 8192, OverflowPolicy Policy = OverflowPolicy::BLOCK>
class RingBuffer {
    static_assert((Size & (Size - 1)) == 0, "Size must be a power of 2");

public:
    RingBuffer() = default;
    ~RingBuffer() = default;

    bool enqueue(const T& item);
    bool enqueue(T&& item);
    bool dequeue(T& item);

    bool empty() const noexcept;
    bool full()  const noexcept;
    std::size_t capacity() const noexcept { return Size; }

private:
    static constexpr std::size_t MASK = Size - 1;

    // 对齐到 cache line 防止 false sharing
    alignas(64) std::atomic<std::size_t> write_pos_{0};
    alignas(64) std::atomic<std::size_t> read_pos_{0};
    alignas(64) T buffer_[Size];
};

template<typename T, std::size_t Size, OverflowPolicy Policy>
bool RingBuffer<T, Size, Policy>::enqueue(const T& item)
{
    auto wp = write_pos_.load(std::memory_order_relaxed);
    auto rp = read_pos_.load(std::memory_order_acquire);

    if (wp - rp < Size) {
        buffer_[wp & MASK] = item;
        write_pos_.store(wp + 1, std::memory_order_release);
        return true;
    }

    if constexpr (Policy == OverflowPolicy::DROP_NEWEST) {
        return false;
    }
    else if constexpr (Policy == OverflowPolicy::BLOCK) {
        while (true) {
            std::this_thread::yield();
            rp = read_pos_.load(std::memory_order_acquire);
            if (wp - rp < Size) {
                buffer_[wp & MASK] = item;
                write_pos_.store(wp + 1, std::memory_order_release);
                return true;
            }
        }
    }
}

template<typename T, std::size_t Size, OverflowPolicy Policy>
bool RingBuffer<T, Size, Policy>::enqueue(T&& item)
{
    auto wp = write_pos_.load(std::memory_order_relaxed);
    auto rp = read_pos_.load(std::memory_order_acquire);

    if (wp - rp < Size) {
        buffer_[wp & MASK] = std::move(item);
        write_pos_.store(wp + 1, std::memory_order_release);
        return true;
    }

    if constexpr (Policy == OverflowPolicy::DROP_NEWEST) {
        return false;
    }
    else if constexpr (Policy == OverflowPolicy::BLOCK) {
        while (true) {
            std::this_thread::yield();
            rp = read_pos_.load(std::memory_order_acquire);
            if (wp - rp < Size) {
                buffer_[wp & MASK] = std::move(item);
                write_pos_.store(wp + 1, std::memory_order_release);
                return true;
            }
        }
    }
}

template<typename T, std::size_t Size, OverflowPolicy Policy>
bool RingBuffer<T, Size, Policy>::dequeue(T& item)
{
    auto rp = read_pos_.load(std::memory_order_relaxed);
    auto wp = write_pos_.load(std::memory_order_acquire);
    
    if (rp == wp) {
        return false;
    }
    item = std::move(buffer_[rp & MASK]);
    read_pos_.store(rp + 1, std::memory_order_release);
    return true;
}

template<typename T, std::size_t Size, OverflowPolicy Policy>
bool RingBuffer<T, Size, Policy>::empty() const noexcept
{
    return read_pos_.load(std::memory_order_acquire) ==
           write_pos_.load(std::memory_order_acquire);
}

template<typename T, std::size_t Size, OverflowPolicy Policy>
bool RingBuffer<T, Size, Policy>::full() const noexcept
{
    return write_pos_.load(std::memory_order_acquire) -
           read_pos_.load(std::memory_order_acquire) >= Size;
}

} // namespace cpp109
