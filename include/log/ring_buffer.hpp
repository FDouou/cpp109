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
    T copy = item;
    return enqueue(std::move(copy));
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

// ── ByteRingBuffer：基于字节的 SPSC ring buffer（双缓冲无拷贝） ──
template<std::size_t Capacity = 1 << 20, OverflowPolicy Policy = OverflowPolicy::BLOCK>
class ByteRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    static_assert(Capacity >= 64, "Capacity too small");
    static constexpr std::size_t MASK = 2 * Capacity - 1;

public:
    ByteRingBuffer()
        : storage_(static_cast<std::byte*>(
              ::operator new(2 * Capacity, std::align_val_t{64})))
    {}

    ~ByteRingBuffer() {
        ::operator delete(storage_, std::align_val_t{64});
    }

    ByteRingBuffer(const ByteRingBuffer&) = delete;
    ByteRingBuffer& operator=(const ByteRingBuffer&) = delete;

    std::byte* prepare_write(std::size_t n) noexcept {
        auto wp = writer_pos_.load(std::memory_order_relaxed);
        if (n == 0) return nullptr;
        if (wp - cached_reader_pos_ > Capacity - n) {
            cached_reader_pos_ = reader_pos_.load(std::memory_order_acquire);
            if (wp - cached_reader_pos_ > Capacity - n) {
                return nullptr;
            }
        }
        return storage_ + (wp & MASK);
    }

    void commit_write(std::size_t n) noexcept {
        auto wp = writer_pos_.load(std::memory_order_relaxed);
        writer_pos_.store(wp + n, std::memory_order_release);
    }

    std::byte* prepare_read() noexcept {
        auto rp = reader_pos_.load(std::memory_order_relaxed);
        auto wp = writer_pos_.load(std::memory_order_acquire);
        if (rp >= wp) return nullptr;
        return storage_ + (rp & MASK);
    }

    void commit_read(std::size_t n) noexcept {
        auto rp = reader_pos_.load(std::memory_order_relaxed);
        reader_pos_.store(rp + n, std::memory_order_release);
    }

    bool empty() const noexcept {
        return reader_pos_.load(std::memory_order_acquire) >=
               writer_pos_.load(std::memory_order_acquire);
    }

    std::size_t capacity() const noexcept { return Capacity; }

private:
    alignas(64) std::atomic<std::size_t> writer_pos_{0};
    alignas(64) std::atomic<std::size_t> reader_pos_{0};
    std::byte* storage_;
    std::size_t cached_reader_pos_ = 0;
};

} // namespace cpp109
