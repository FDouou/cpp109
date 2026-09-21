#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <thread>

// ── 平台相关的 RDTSC ──
#if defined(_MSC_VER)
    #include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
    #include <x86intrin.h>
#endif

namespace cpp109 {

// ── RDTSC 时钟周期（前台时间戳，开销 ~10ns，远快于 system_clock）──
inline std::uint64_t rdtsc_ns() noexcept {
#if defined(_MSC_VER)
    return __rdtsc();
#elif defined(__x86_64__) || defined(__i386__)
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
#else
    // 非 x86 回退到 steady_clock
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

// ── RdtscClock：将 rdtsc 值转换为 wall-clock 纳秒（全局单例，周期校准）──
// 三件套：
//   1. 锚点周期对齐：worker 每校准周期（默认 1s）采样 (rdtsc, system_clock)
//      对更新 base，绝对时间误差上限 ≈ 校准间隔内的残余漂移（<1ms）；
//   2. 除法换定点乘法：预计算 coef = 2^48 * 1e9 / freq，换算一次 _umul128
//      取高位，替代逐记录 128 位除法（~30 周期 → ~5 周期）；
//   3. 异常检测与回退：校准发现 Δtsc/Δns 突变（>5%，vCPU 迁移 / 非 invariant
//      TSC / 深度休眠）→ disabled，to_ns 退回 system_clock 现场打点（正确性
//      优先，新日志时间戳直接取当前墙钟）。
// 锚点发布：double buffer + 版本号（seqlock 读取模式）。写侧每周期一次
// （1Hz，且被 CAS 抢单收敛到单写者），读侧在 worker 热路径，重试概率≈0。
class RdtscClock {
public:
    static RdtscClock& instance() noexcept {
        static RdtscClock clock;
        return clock;
    }

    // 周期校准：由 LogBackend worker 每轮循环调用。内部节流（距上次校准
    // 不足 interval 直接返回）+ CAS 抢单（多 worker 同时到达只有赢者执行）。
    void calibrate() noexcept {
        if (disabled_.load(std::memory_order_relaxed)) return;
        const std::uint64_t now = steady_ns();
        std::uint64_t last = last_cal_ns_.load(std::memory_order_relaxed);
        if (now - last < interval_ns_.load(std::memory_order_relaxed)) return;
        if (!last_cal_ns_.compare_exchange_strong(
                last, now, std::memory_order_acq_rel, std::memory_order_relaxed)) return;
        do_calibrate();
    }

    // 校准间隔（纳秒，默认 1s）。
    void set_calibration_interval(std::uint64_t interval_ns) noexcept {
        interval_ns_.store(interval_ns, std::memory_order_relaxed);
    }

    // rdtsc → wall-clock 纳秒。禁用时直接返回当前墙钟（放弃换算）。
    std::uint64_t to_ns(std::uint64_t tsc) const noexcept {
        if (disabled_.load(std::memory_order_acquire)) return wall_ns();

        // seqlock 读取：版本号一致才认为锚点对完整
        std::uint64_t v1, v2, a_tsc, a_ns, coef;
        do {
            v1 = published_.load(std::memory_order_acquire);
            const std::uint64_t idx = v1 & 1;
            a_ns  = anchor_ns_[idx].load(std::memory_order_relaxed);
            a_tsc = anchor_tsc_[idx].load(std::memory_order_relaxed);
            coef  = coef_fixed_.load(std::memory_order_relaxed);
            v2 = published_.load(std::memory_order_acquire);
        } while (v1 != v2);

        const std::uint64_t delta = tsc - a_tsc;
        // (delta * coef) >> 48；delta < 2^63、coef < 2^47 → 128 位中间值内
#if defined(_MSC_VER)
        unsigned __int64 hi;
        const unsigned __int64 lo = _umul128(delta, coef, &hi);
#else
        const __uint128_t prod = static_cast<__uint128_t>(delta) * coef;
        const std::uint64_t hi = static_cast<std::uint64_t>(prod >> 64);
        const std::uint64_t lo = static_cast<std::uint64_t>(prod);
#endif
        return a_ns + ((hi << (64 - kFixedShift)) | (lo >> kFixedShift));
    }

    // 纳秒 → TSC 周期（唤醒限流等低精度换算；从当前定点系数反推频率）
    std::uint64_t ns_to_tsc(std::uint64_t ns) const noexcept {
        const std::uint64_t coef = coef_fixed_.load(std::memory_order_relaxed);
        if (coef == 0) return 1;
        const double ns_per_tick =
            std::ldexp(static_cast<double>(coef), -static_cast<int>(kFixedShift));
        return static_cast<std::uint64_t>(static_cast<double>(ns) / ns_per_tick + 0.5);
    }

    // TSC 是否仍可信（false 表示已回退系统时钟）
    bool usable() const noexcept {
        return !disabled_.load(std::memory_order_acquire);
    }

private:
    RdtscClock() noexcept {
        // 初始频率估计：100ms 样本（仅一次）
        auto t1 = std::chrono::steady_clock::now();
        const std::uint64_t c1 = rdtsc_ns();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::uint64_t c2 = rdtsc_ns();
        auto t2 = std::chrono::steady_clock::now();
        const double freq = static_cast<double>(c2 - c1) /
            std::chrono::duration<double>(t2 - t1).count();
        compute_coef(freq);

        // 初始锚点（单线程构造，直接写双槽避免发布竞争）
        const std::uint64_t tsc = rdtsc_ns();
        const std::uint64_t ns  = wall_ns();
        anchor_ns_[0].store(ns, std::memory_order_relaxed);
        anchor_ns_[1].store(ns, std::memory_order_relaxed);
        anchor_tsc_[0].store(tsc, std::memory_order_relaxed);
        anchor_tsc_[1].store(tsc, std::memory_order_relaxed);
        published_.store(1, std::memory_order_release);
        last_tsc_ = tsc;
        last_ns_  = ns;
        last_cal_ns_.store(steady_ns(), std::memory_order_relaxed);
    }

    void do_calibrate() noexcept {
        const std::uint64_t tsc = rdtsc_ns();
        const std::uint64_t ns  = wall_ns();

        const std::uint64_t d_ns  = ns - last_ns_;
        const std::uint64_t d_tsc = tsc - last_tsc_;
        if (d_ns >= kMinFreqSampleNs) {
            // 样本间隔足够长才重估频率，短样本会被调度抖动污染
            const double freq_new = static_cast<double>(d_tsc) * 1e9 / static_cast<double>(d_ns);
            const std::uint64_t coef = coef_fixed_.load(std::memory_order_relaxed);
            const double freq_cur = (coef > 0)
                ? 1e9 / std::ldexp(static_cast<double>(coef), -static_cast<int>(kFixedShift))
                : freq_new;
            // 突变（vCPU 迁移 / 降频 / 深度休眠）：TSC 不可信，永久回退
            if (freq_cur > 0.0 &&
                std::abs(freq_new - freq_cur) / freq_cur > kFreqDriftLimit) {
                disabled_.store(true, std::memory_order_release);
                return;
            }
            // 滑动平均吸收测量噪声，保留长期趋势
            compute_coef(0.2 * freq_new + 0.8 * freq_cur);
        }

        // 发布新锚点：先写数据槽再推版本号（读侧 seqlock 校验一致性）
        const std::uint64_t v = published_.load(std::memory_order_relaxed);
        const std::uint64_t idx = (v + 1) & 1;
        anchor_ns_[idx].store(ns, std::memory_order_relaxed);
        anchor_tsc_[idx].store(tsc, std::memory_order_relaxed);
        published_.store(v + 1, std::memory_order_release);

        last_tsc_ = tsc;
        last_ns_  = ns;
    }

    void compute_coef(double freq_hz) noexcept {
        if (!(freq_hz > 0.0) || !std::isfinite(freq_hz)) return;
        // coef = 2^48 * (1e9 / freq)：每 tick 的纳秒数（定点）
        const double coef = std::ldexp(1e9 / freq_hz, static_cast<int>(kFixedShift));
        coef_fixed_.store(static_cast<std::uint64_t>(coef + 0.5),
                          std::memory_order_relaxed);
    }

    static std::uint64_t wall_ns() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    static std::uint64_t steady_ns() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static constexpr std::uint64_t kFixedShift     = 48;
    static constexpr std::uint64_t kMinFreqSampleNs = 100'000'000;  // 100ms
    static constexpr double        kFreqDriftLimit  = 0.05;         // 5%

    // 锚点双缓冲 + 发布版本（读侧高频，写侧 1Hz 低频；同缓存行减少跨核访问）
    alignas(64) std::atomic<std::uint64_t> anchor_ns_[2];
    std::atomic<std::uint64_t>             anchor_tsc_[2];
    std::atomic<std::uint64_t>             published_{1};
    std::atomic<std::uint64_t>             coef_fixed_{0};
    std::atomic<bool>                      disabled_{false};
    std::atomic<std::uint64_t>             last_cal_ns_{0};
    std::atomic<std::uint64_t>             interval_ns_{1'000'000'000};

    // 仅 calibrate 线程触碰（CAS 抢单保证单写者）
    std::uint64_t last_tsc_ = 0;
    std::uint64_t last_ns_  = 0;
};

} // namespace cpp109