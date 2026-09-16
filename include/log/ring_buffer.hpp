#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <new>

namespace cpp109 {

// 单生产者单消费者字节环形缓冲（SPSC）。
//
// 存储为 Capacity 字节（不是历史实现里的 2*Capacity）；跨尾写入/读取通过
// Chunk / copy_from 分段处理，保证任何相位下都不会越界：
//   - 写侧：prepare_write 返回第一段，剩余部分由 write_chunk / scatter_write
//     自动写到存储起始处；
//   - 读侧：copy_from 从读位置线性化到调用者缓冲（跨尾自动两段）。
//
// 热路径空间判断常态走本地缓存的读指针（cached_reader_pos_），
// 仅在疑似空间不足时才跨线程 acquire 读真实消费位置。
template<std::size_t Capacity = 1 << 20>
class ByteRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    static_assert(Capacity >= 64, "Capacity too small");
    static constexpr std::size_t MASK = Capacity - 1;

public:
    // 一次操作的分段信息：第一段 ptr/len，剩余在存储起始处。
    struct Chunk {
        std::byte*  ptr   = nullptr;
        std::size_t len   = 0;   // 第一段字节数
        std::size_t total = 0;   // 本次操作总字节数
    };

    ByteRingBuffer()
        : storage_(static_cast<std::byte*>(
              ::operator new(Capacity, std::align_val_t{64})))
    {}

    ~ByteRingBuffer() {
        ::operator delete(storage_, std::align_val_t{64});
    }

    ByteRingBuffer(const ByteRingBuffer&) = delete;
    ByteRingBuffer& operator=(const ByteRingBuffer&) = delete;

    // ── 写侧 ──
    // 准备写入 n 字节；ptr == nullptr 表示空间不足。
    Chunk prepare_write(std::size_t n) noexcept {
        if (n == 0 || n > Capacity) return {};
        auto wp = writer_pos_.load(std::memory_order_relaxed);
        if (wp - cached_reader_pos_ > Capacity - n) {
            cached_reader_pos_ = reader_pos_.load(std::memory_order_acquire);
            if (wp - cached_reader_pos_ > Capacity - n) return {};
        }
        const std::size_t off   = wp & MASK;
        const std::size_t first = (n <= Capacity - off) ? n : (Capacity - off);
        return Chunk{storage_ + off, first, n};
    }

    // 把 n 字节连续源数据写入已 prepare 的区域（自动跨尾）
    void write_chunk(const Chunk& c, const std::byte* src, std::size_t n) noexcept {
        if (n == 0 || c.ptr == nullptr) return;
        const std::size_t first = (n < c.len) ? n : c.len;
        std::memcpy(c.ptr, src, first);
        if (n > first) {
            std::memcpy(storage_, src + first, n - first);
        }
    }

    // 在已 prepare 的记录内按偏移写入（header + payload 分别写，自动跨尾）
    void scatter_write(const Chunk& c, std::size_t offset,
                       const std::byte* src, std::size_t n) noexcept {
        if (n == 0 || c.ptr == nullptr) return;
        if (offset < c.len) {
            const std::size_t take =
                (offset + n <= c.len) ? n : (c.len - offset);
            std::memcpy(c.ptr + offset, src, take);
            offset += take;
            src    += take;
            n      -= take;
        }
        if (n > 0) {
            std::memcpy(storage_ + (offset - c.len), src, n);
        }
    }

    void commit_write(std::size_t n) noexcept {
        auto wp = writer_pos_.load(std::memory_order_relaxed);
        writer_pos_.store(wp + n, std::memory_order_release);
    }

    // ── 读侧 ──
    // 当前对消费者可见的字节数（本线程视角）
    std::size_t readable() const noexcept {
        return writer_pos_.load(std::memory_order_acquire) -
               reader_pos_.load(std::memory_order_relaxed);
    }

    // 从读位置 + offset 处拷贝 n 字节到 dst（自动跨尾）
    void copy_from(std::size_t offset, void* dst, std::size_t n) const noexcept {
        if (n == 0) return;
        auto rp = reader_pos_.load(std::memory_order_relaxed);
        const std::size_t off   = (rp + offset) & MASK;
        const std::size_t first = (n <= Capacity - off) ? n : (Capacity - off);
        std::memcpy(dst, storage_ + off, first);
        if (n > first) {
            std::memcpy(static_cast<std::byte*>(dst) + first, storage_, n - first);
        }
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
