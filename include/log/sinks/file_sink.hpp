#pragma once

#include "../sink.hpp"

#include <cstdio>
#include <string>
#include <chrono>

namespace cpp109 {

// ──────────────────────────────────────────────────────────
// 模块四·FileSink：普通文件输出
//
// 功能：
//   - 写入指定文件
//   - 支持覆盖（truncate）或追加（append）
//   - 自动 flush 间隔（默认每行 flush）
//   - 线程安全（基类 mutex 保护写入）
// ──────────────────────────────────────────────────────────

class FileSink : public Sink {
public:
    // filename: 文件路径
    // truncate: true=覆盖, false=追加
    explicit FileSink(std::string filename, bool truncate = false){
        filename_ = std::move(filename);
        file_ = fopen(filename_.c_str(), truncate ? "w" : "a");
        if (file_ == nullptr) {
            throw std::runtime_error("Failed to open file: " + filename_);
        }
    }

    ~FileSink() override{
        if(file_) fclose(file_);
    }

    // 设置自动 flush 间隔（0 表示每行都 flush，-1 表示从不自动 flush）
    void set_flush_interval(int interval_ms) noexcept { flush_interval_ms_ = interval_ms; }

    // 重新打开文件（用于外部 logrotate 等场景）
    bool reopen(){
        std::lock_guard lock(mutex_);
        fclose(file_);
        file_ = fopen(filename_.c_str(), "a");
        return file_ != nullptr;
    }

protected:
    void write(const std::string& formatted_msg, const LogEvent& event) override{
        fwrite(formatted_msg.data(), 1, formatted_msg.size(), file_);
        fwrite("\n", 1, 1, file_);
        if(flush_interval_ms_ == 0) {
            flush_impl();
        }else if(flush_interval_ms_ > 0) {
            auto now = std::chrono::steady_clock::now();
            auto duration =std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush_time_).count();
            if(duration >= flush_interval_ms_) {
                flush_impl();
                last_flush_time_ = now;
            }
        }
    }
    void flush_impl() override{
        fflush(file_);
    }

private:
    std::string filename_;
    std::FILE*  file_ = nullptr;
    int         flush_interval_ms_ = 0;
    std::chrono::steady_clock::time_point last_flush_time_ = std::chrono::steady_clock::now();
};

} // namespace cpp109
