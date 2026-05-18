#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>

namespace cpp109 {

class Timestamp {
public:
    using clock      = std::chrono::system_clock;
    using time_point = clock::time_point;
    using duration   = clock::duration;

    Timestamp() noexcept : tp_(clock::now()) {}
    explicit Timestamp(time_point tp) noexcept : tp_(tp) {}

    static Timestamp now() noexcept{
        return Timestamp();
    }

    // 访问底层 time_point
    time_point get() const noexcept { return tp_; }

    // ── 各部分拆解（本地时间） ──
    int year()   const noexcept{
        std::tm tm;
        to_local_tm(tm);
        return tm.tm_year + 1900;
    }
    int month()  const noexcept{  // 1-12
        std::tm tm;
        to_local_tm(tm);
        return tm.tm_mon + 1;
    }
    int day()    const noexcept{  // 1-31
        std::tm tm;
        to_local_tm(tm);
        return tm.tm_mday;
    }
    int hour()   const noexcept{  // 0-23
        std::tm tm;
        to_local_tm(tm);
        return tm.tm_hour;
    }
    int minute() const noexcept{  // 0-59
        std::tm tm;
        to_local_tm(tm);
        return tm.tm_min;
    }
    int second() const noexcept{  // 0-59
        std::tm tm;
        to_local_tm(tm);
        return tm.tm_sec;
    }
    int millisecond() const noexcept{  // 0-999
        return epoch_milliseconds() % 1000;
    }
    int microsecond() const noexcept{ // 0-999999
        return epoch_microseconds() % 1000000;
    }

    // ── epoch 毫秒 / 微秒 ──
    int64_t epoch_milliseconds() const noexcept{
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   tp_.time_since_epoch()
               ).count();
    }
    int64_t epoch_microseconds() const noexcept{
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   tp_.time_since_epoch()
               ).count();
    }

    // 前一天/后一天比较（用于 DailyFileSink 判断日期变更）
    bool same_day(const Timestamp& other) const noexcept{
        return
        std::chrono::floor<std::chrono::days>(tp_) == std::chrono::floor<std::chrono::days>(other.tp_);
        
    }

private:
    void to_local_tm(std::tm& out) const noexcept {
        auto t = clock::to_time_t(tp_);
#ifdef _WIN32
        localtime_s(&out, &t);
#else
        localtime_r(&t, &out);
#endif
    }

    time_point tp_;
};

} // namespace cpp109
