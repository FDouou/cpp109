#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace cpp109 {

class BackgroundCpuPool {
public:
    explicit BackgroundCpuPool(const std::vector<int>& cpu_ids)
        : cpu_ids_(cpu_ids.empty() ? std::vector<int>{0} : cpu_ids)
        , index_(0)
    {}

    // Round-robin: 返回包含单个 CPU ID 的 vector
    std::vector<int> next() noexcept {
        std::size_t i = index_.fetch_add(1, std::memory_order_relaxed);
        return {cpu_ids_[i % cpu_ids_.size()]};
    }

private:
    std::vector<int> cpu_ids_;
    std::atomic<std::size_t> index_;
};

} // namespace cpp109
