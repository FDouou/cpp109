#pragma once

#include "log_level.hpp"
#include "log_event.hpp"
#include "sink.hpp"

#include <atomic>
#include <format>
#include <source_location>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace cpp109 {

// Batch buffer for foreground thread: accumulate multiple records and submit
// as one batch to reduce atomic RMW on the ring buffer.
namespace detail {
    static constexpr std::size_t BATCH_CAPACITY = 4096;
    inline thread_local std::byte tl_batch_data[BATCH_CAPACITY] = {};
    inline thread_local std::size_t tl_batch_size = 0;
    inline thread_local AsyncSinkBase* tl_batch_sink = nullptr;

    inline void flush_deferred_batch() {
        if (tl_batch_size > 0 && tl_batch_sink) {
            tl_batch_sink->log_encoded_batch(tl_batch_data, tl_batch_size);
            tl_batch_size = 0;
        }
    }
} // namespace detail

class Logger : public std::enable_shared_from_this<Logger> {
public:
    explicit Logger(std::string name) : name_(std::move(name)){}

    template<typename... Args>
    void trace(std::format_string<Args...> fmt, Args&&... args);

    template<typename... Args>
    void debug(std::format_string<Args...> fmt, Args&&... args);

    template<typename... Args>
    void info(std::format_string<Args...> fmt, Args&&... args);

    template<typename... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args);

    template<typename... Args>
    void error(std::format_string<Args...> fmt, Args&&... args);

    template<typename... Args>
    void fatal(std::format_string<Args...> fmt, Args&&... args);

    void log(LogLevel level, std::string formatted_msg,
             std::source_location loc){
                log_impl(level, std::move(formatted_msg), loc);
             }

    void add_sink(std::shared_ptr<Sink> sink){
        std::lock_guard<std::mutex> lock(mutex_);
        sinks_.push_back(sink);
        update_fast_path_unlocked();
    }
    void remove_sink(std::shared_ptr<Sink> sink){
        std::lock_guard<std::mutex> lock(mutex_);
        sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), sink), sinks_.end());
        update_fast_path_unlocked();
    }
    void clear_sinks(){
        std::lock_guard<std::mutex> lock(mutex_);
        sinks_.clear();
        std::atomic_store_explicit(&fast_sink_, std::shared_ptr<Sink>{nullptr}, std::memory_order_release);
        cached_async_sink_ = nullptr;
    }

    void set_level(LogLevel level) { level_.store(level, std::memory_order_release); }
    LogLevel level() const noexcept { return level_.load(std::memory_order_acquire); }

    const std::string& name() const noexcept { return name_; }
    void set_name(const std::string& name) { name_ = name; }

    void set_parent(const std::shared_ptr<Logger>& parent) {
        std::lock_guard<std::mutex> lock(mutex_);
        parent_ = parent;
    }
    std::shared_ptr<Logger> parent() const noexcept { return parent_.lock(); }

    void flush(){
        detail::flush_deferred_batch();
        std::lock_guard<std::mutex> lock(mutex_);
        for(const auto& sink : sinks_){
            sink->flush();
        }
    }

    void set_propagate(bool propagate) noexcept { propagate_.store(propagate, std::memory_order_release); }
    bool propagate() const noexcept { return propagate_.load(std::memory_order_acquire); }

private:
    void log_impl(LogLevel level, std::string message, std::source_location sloc){
        if(level < this->level()) return;

        static thread_local std::uint64_t tl_tid = platform::current_thread_id();
        SourceLoc loc{sloc.file_name(), static_cast<int>(sloc.line()), sloc.function_name()};
        LogEvent event = {name_, level, std::move(message), Timestamp(), loc, tl_tid};

        auto fast = std::atomic_load_explicit(&fast_sink_, std::memory_order_acquire);
        if (fast && parent_.expired()) {
            if (level == LogLevel::FATAL) {
                fast->log(event);
                fast->flush();
                std::abort();
            }
            fast->log(event);
            return;
        }

        std::shared_ptr<Logger> parent;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(level == LogLevel::FATAL){
                dispatch_to_sinks(event);
                flush_unlocked();
                std::abort();
            }

            dispatch_to_sinks(event);

            parent = parent_.lock();
        }

        if(this->propagate() && parent){
            parent->log_impl(level, event.message(), sloc);
        }
    }
    void dispatch_to_sinks(const LogEvent& event){
        for(const auto& sink : sinks_){
            sink->log(event);
        }
    }
    void flush_unlocked(){
        for(const auto& sink : sinks_){
            sink->flush();
        }
    }

    void update_fast_path_unlocked(){
        if (sinks_.size() == 1) {
            std::atomic_store_explicit(&fast_sink_, sinks_[0], std::memory_order_release);
            cached_async_sink_ = dynamic_cast<AsyncSinkBase*>(sinks_[0].get());
        } else {
            std::atomic_store_explicit(&fast_sink_, std::shared_ptr<Sink>{nullptr}, std::memory_order_release);
            cached_async_sink_ = nullptr;
        }
    }

    std::string       name_;
    std::atomic<LogLevel> level_ = LogLevel::INFO;
    std::atomic<bool> propagate_ = true;
    std::weak_ptr<Logger> parent_;
    std::vector<std::shared_ptr<Sink>> sinks_;
    std::shared_ptr<Sink> fast_sink_;
    AsyncSinkBase* cached_async_sink_ = nullptr;
    std::mutex        mutex_;
};

// ─── FAST PATH MACRO (optimized, TinyHeader) ─────────────────────
// For each log level method, the compiler-generated template body uses
// one of three paths:
//   1. Async single sink → TinyHeader + log_encoded (32B header, zero alloc)
//   2. Sync single sink  → LogEvent + log_move (one allocation if SBO exceeded)
//   3. Fallback (multi sink / parent) → log_impl (format + dispatch)

#define CPP109_LOGGER_METHOD_IMPL(method_name, level_enum)                      \
    template<typename... Args>                                                 \
    void Logger::method_name(std::format_string<Args...> fmt, Args&&... args)  \
    {                                                                          \
        if (level_enum < level_) return;                                       \
        SourceLoc loc{__FILE__, __LINE__, __func__};                           \
        /* extract raw format string */                                         \
        const char* fmt_str = []<typename... Ts>(                              \
            const std::format_string<Ts...>& f) -> const char* {               \
            return reinterpret_cast<                                           \
                const std::string_view*>(&f)->data();                          \
        }.template operator()<Args...>(fmt);                                   \
        static thread_local std::uint64_t tl_tid =                             \
            platform::current_thread_id();                                     \
        /* fast path 1: async sink -> TinyHeader + direct log_encoded */        \
        auto* abase = cached_async_sink_;                                      \
        if (abase && parent_.expired()) {                                      \
            std::uint32_t args_size = 0;                                         \
            if constexpr (sizeof...(Args) > 0) {                                \
                args_size = static_cast<std::uint32_t>(                          \
                    detail::compute_encoded_size(args...));                      \
            }                                                                    \
            std::byte* enc_buf = nullptr;                                        \
            if (args_size > 0) {                                                 \
                enc_buf = detail::get_encode_buffer(args_size);                  \
                detail::encode_args(enc_buf, args...);                          \
            }                                                                    \
            /* 每个模板实例化一个唯一的 static TinyMeta（持久于 .data 段） */        \
            static const TinyMeta _meta{                                        \
                loc.file, loc.line, loc.func, fmt_str,                           \
                sizeof...(Args) > 0                                              \
                    ? &detail::decode_and_format<std::decay_t<Args>...>         \
                    : nullptr                                                    \
            };                                                                   \
            /* 批量缓冲区：积累到 4KB 后一次提交 */                                   \
            const std::size_t needed = sizeof(TinyHeader) + args_size;             \
            std::byte* batch_data = detail::tl_batch_data;                          \
            std::size_t& batch_sz = detail::tl_batch_size;                         \
            detail::tl_batch_sink = abase;                                         \
                                                                                   \
            if (batch_sz + needed > detail::BATCH_CAPACITY && batch_sz > 0) {     \
                abase->log_encoded_batch(batch_data, batch_sz);                   \
                batch_sz = 0;                                                      \
            }                                                                      \
                                                                                   \
            const std::size_t off = batch_sz;                                      \
            batch_sz = off + needed;                                               \
                                                                                   \
            TinyHeader* hdr = reinterpret_cast<TinyHeader*>(batch_data + off);    \
            hdr->timestamp_tsc = rdtsc_ns();                                       \
            hdr->meta           = &_meta;                                           \
            hdr->thread_id      = tl_tid;                                           \
            hdr->args_size      = args_size;                                        \
            hdr->level          = static_cast<uint8_t>(level_enum);                 \
            hdr->flags          = 0;                                                \
                                                                                   \
            if (args_size > 0) {                                                    \
                std::memcpy(batch_data + off + sizeof(TinyHeader),                  \
                           enc_buf, args_size);                                     \
            }                                                                      \
                                                                                   \
            if (level_enum == LogLevel::FATAL) {                                  \
                detail::flush_deferred_batch();                                   \
                abase->flush();                                                   \
                std::abort();                                                     \
            }                                                                     \
            return;                                                              \
        }                                                                        \
        /* fast path 2: sync single sink -> LogEvent + log_move */               \
        auto* fast_sync = cached_async_sink_ ? nullptr                            \
            : std::atomic_load_explicit(                                          \
                &fast_sink_, std::memory_order_acquire).get();                    \
        if (fast_sync && parent_.expired()) {                                    \
            LogEvent event(name_, level_enum, fmt_str, loc, tl_tid,              \
                           std::forward<Args>(args)...);                         \
            fast_sync->log_move(std::move(event));                               \
            if (level_enum == LogLevel::FATAL) {                                 \
                fast_sync->flush();                                              \
                std::abort();                                                    \
            }                                                                    \
            return;                                                              \
        }                                                                        \
        /* slow path: multi sink or with parent -> log_impl */                    \
        try {                                                                   \
            log_impl(level_enum, std::format(fmt, std::forward<Args>(args)...),  \
                     std::source_location::current());                           \
        } catch (const std::format_error&) {                                    \
            log_impl(level_enum, "[FORMAT_ERROR] fallback",                      \
                     std::source_location::current());                           \
        }                                                                        \
    }

CPP109_LOGGER_METHOD_IMPL(trace, LogLevel::TRACE)
CPP109_LOGGER_METHOD_IMPL(debug, LogLevel::DEBUG)
CPP109_LOGGER_METHOD_IMPL(info,  LogLevel::INFO)
CPP109_LOGGER_METHOD_IMPL(warn,  LogLevel::WARN)
CPP109_LOGGER_METHOD_IMPL(error, LogLevel::ERROR)
CPP109_LOGGER_METHOD_IMPL(fatal, LogLevel::FATAL)

#undef CPP109_LOGGER_METHOD_IMPL

} // namespace cpp109
