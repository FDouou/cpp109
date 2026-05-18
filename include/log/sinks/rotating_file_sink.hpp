#pragma once

#include "../sink.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <cerrno>

namespace cpp109 {

// ──────────────────────────────────────────────────────────
// 模块四·RotatingFileSink：按大小滚动文件输出
//
// 功能：
//   - 日志文件达到指定大小时自动滚动
//   - 旧文件命名：log.txt → log.txt.1 → log.txt.2 ...
//   - 超出 max_files 时自动删除最旧的
// ──────────────────────────────────────────────────────────

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
        std::size_t msg_size = formatted_msg.size() + 1;
        if(current_size_ + msg_size > max_size_){
            do_rotate();
        }
        fwrite(formatted_msg.data(), 1, formatted_msg.size(), file_);
        fwrite("\n", 1, 1, file_);
        current_size_ += msg_size;
    }
    void flush_impl() override{
        fflush(file_);
    }

private:
    void do_rotate(){
        if(file_) fclose(file_);
        remove_oldest();
        //rename
        for(int i = max_files_ - 1; i > 0; i--){
            std::string old_filename = indexed_filename(i);
            std::string new_filename = indexed_filename(i + 1);
            if(std::rename(old_filename.c_str(), new_filename.c_str()) != 0){
                if(errno != ENOENT){
                    throw std::runtime_error("Failed to rename file: " + old_filename);
                }
            }
        }
        std::rename(base_filename_.c_str(), indexed_filename(1).c_str());

        file_ = fopen(base_filename_.c_str(), "a");
        if(file_ == nullptr){
            throw std::runtime_error("Failed to open file: " + base_filename_);
        }
        current_size_ = 0;
    }
    void remove_oldest(){
        std::string oldest_filename = indexed_filename(max_files_ - 1);
        if(std::remove(oldest_filename.c_str()) != 0){
            if(errno != ENOENT){
                throw std::runtime_error("Failed to remove file: " + oldest_filename);
            }
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
