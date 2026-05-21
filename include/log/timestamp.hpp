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

    Timestamp() noexcept : tp_(clock::now()) { cache(); }
    explicit Timestamp(time_point tp) noexcept : tp_(tp) { cache(); }

    static Timestamp now() noexcept { return Timestamp(); }

    time_point get() const noexcept { return tp_; }

    int year()        const noexcept { return year_; }
    int month()       const noexcept { return month_; }
    int day()         const noexcept { return day_; }
    int hour()        const noexcept { return hour_; }
    int minute()      const noexcept { return minute_; }
    int second()      const noexcept { return second_; }
    int millisecond() const noexcept { return millisecond_; }
    int microsecond() const noexcept { return microsecond_; }

    bool same_day(const Timestamp& other) const noexcept {
        return std::chrono::floor<std::chrono::days>(tp_) ==
               std::chrono::floor<std::chrono::days>(other.tp_);
    }

private:
    void cache() noexcept {
        auto t = clock::to_time_t(tp_);
        std::tm tm;
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        year_   = tm.tm_year + 1900;
        month_  = tm.tm_mon + 1;
        day_    = tm.tm_mday;
        hour_   = tm.tm_hour;
        minute_ = tm.tm_min;
        second_ = tm.tm_sec;

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            tp_.time_since_epoch()).count();
        millisecond_ = static_cast<int>(ms % 1000);
        microsecond_ = static_cast<int>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                tp_.time_since_epoch()).count() % 1000000);
    }

    time_point tp_;
    int year_        = 1970;
    int month_       = 1;
    int day_         = 1;
    int hour_        = 0;
    int minute_      = 0;
    int second_      = 0;
    int millisecond_ = 0;
    int microsecond_ = 0;
};

} // namespace cpp109
