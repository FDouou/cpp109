#pragma once

#include "../sink.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <cerrno>

namespace cpp109 {

class RotatingFileSink : public Sink {
public:
    // filename:    基础文件名
    // max_size_mb: 单个文件最大大小（MB）
    // max_files:   最大保留文件数
    RotatingFileSink(std::string filename, std::uint64_t max_size_mb, int max_files):
        base_filename_(std::move(filename)),
        max_size_(max_size_mb * 1024 * 1024),
        max_files_(max_files)
    {
        file_ = fopen(base_filename_.c_str(), "a");
        if(file_ == nullptr){
            throw std::runtime_error("Failed to open file: " + base_filename_);
        }
        // 初始化 current_size_ 为文件大小
        if (fseek(file_, 0, SEEK_END) != 0) {
            throw std::runtime_error("Failed to seek file end: " + base_filename_);
        }
        long size = std::ftell(file_);
        if (size == -1L) {
            throw std::runtime_error("Failed to tell file size: " + base_filename_);
        }
        current_size_ = static_cast<std::uint64_t>(size);
    }

    ~RotatingFileSink() override{
        if(file_) fclose(file_);
    }

protected:
    void write(const std::string& formatted_msg, const LogEvent& event) override{
        if (file_ == nullptr) { report_io_error(); return; }
        std::size_t msg_size = formatted_msg.size() + 1;
        if(current_size_ + msg_size > max_size_){
            do_rotate();
            if (file_ == nullptr) { report_io_error(); return; }
        }
        const bool ok =
            fwrite(formatted_msg.data(), 1, formatted_msg.size(), file_) == formatted_msg.size()
            && fputc('\n', file_) != EOF;
        if (!ok) { report_io_error(); return; }
        current_size_ += msg_size;
    }
    void flush_impl() override{
        if (file_ != nullptr && fflush(file_) != 0) report_io_error();
    }

private:
    // 滚动到新文件。不抛异常（运行期由 worker 调用，异常会被 AsyncSink 吞掉
    // 且留下不一致状态）。无论成功与否都保证 file_ 要么指向有效文件、要么为
    // nullptr，绝不悬垂；失败时已 report_io_error。返回是否成功打开新文件。
    bool do_rotate(){
        // 关闭后立即置空，避免任何后续路径拿到已关闭的 FILE*
        if(file_){ fclose(file_); file_ = nullptr; }

        remove_oldest();
        bool rename_ok = true;
        for(int i = max_files_ - 1; i > 0; i--){
            std::string old_filename = indexed_filename(i);
            std::string new_filename = indexed_filename(i + 1);
            if(std::rename(old_filename.c_str(), new_filename.c_str()) != 0){
                if(errno != ENOENT) rename_ok = false;
            }
        }
        if(std::rename(base_filename_.c_str(), indexed_filename(1).c_str()) != 0){
            if(errno != ENOENT) rename_ok = false;
        }
        if(!rename_ok) report_io_error();

        // 无论 rename 是否全部成功都重开基础文件：失败也只是 file_ == nullptr，
        // 不会像原来那样把已关闭的指针留在 file_ 里被后续 fwrite 使用。
        file_ = fopen(base_filename_.c_str(), "a");
        if(file_ == nullptr) return false;
        current_size_ = 0;
        return true;
    }
    void remove_oldest(){
        std::string oldest_filename = indexed_filename(max_files_ - 1);
        if(std::remove(oldest_filename.c_str()) != 0){
            if(errno != ENOENT) report_io_error();
        }
    }
    std::string indexed_filename(int idx) const{
        return base_filename_ + "." + std::to_string(idx);
    }

    std::string   base_filename_;
    std::uint64_t max_size_ = 1024 * 1024;
    int           max_files_ = 5;
    std::uint64_t current_size_ = 0;
    std::FILE*    file_ = nullptr;
};

} // namespace cpp109
