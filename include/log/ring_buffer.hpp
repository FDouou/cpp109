#pragma once

#include <atomic>
#include <cstddef>
#include <new>

namespace cpp109 {

enum class OverflowPolicy {
    BLOCK,
    DROP_NEWEST,
};

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
