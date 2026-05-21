#pragma once

#include "../sink.hpp"
#include "../timestamp.hpp"

#include <cstdio>
#include <string>
#include <ctime>
#include <cerrno>
#include <filesystem>
#include <vector>
#include <algorithm>

namespace cpp109 {

class DailyFileSink : public Sink {
public:
    enum class RotationPeriod {
        MINUTE,
        HOUR,
        DAY,
    };

    // filename_pattern: 含 strftime 占位符的文件名模板，如 "logs/%Y%m%d.log"
    DailyFileSink(std::string filename_pattern,
                  RotationPeriod period = RotationPeriod::DAY,
                  int max_files = 0)
        : filename_pattern_(std::move(filename_pattern)),
          period_(period),
          max_files_(max_files),
          last_rotate_time_(Timestamp::now())
    {
        // 打开第一个文件
        do_rotate();
    }

    ~DailyFileSink() override {
        if (file_) fclose(file_);
    }

protected:
    void write(const std::string& formatted_msg, const LogEvent& event) override {
        if (should_rotate()) {
            do_rotate();
        }
        fwrite(formatted_msg.data(), 1, formatted_msg.size(), file_);
        fputc('\n', file_);
    }

    void flush_impl() override {
        fflush(file_);
    }

private:
    bool should_rotate() const {
        auto now = Timestamp::now();
        switch (period_) {
            case RotationPeriod::MINUTE:
                return now.year() != last_rotate_time_.year() ||
                       now.month() != last_rotate_time_.month() ||
                       now.day() != last_rotate_time_.day() ||
                       now.hour() != last_rotate_time_.hour() ||
                       now.minute() != last_rotate_time_.minute();
            case RotationPeriod::HOUR:
                return now.year() != last_rotate_time_.year() ||
                       now.month() != last_rotate_time_.month() ||
                       now.day() != last_rotate_time_.day() ||
                       now.hour() != last_rotate_time_.hour();
            case RotationPeriod::DAY:
                return !now.same_day(last_rotate_time_);
        }
        return false;
    }

    void do_rotate() {
        if (file_) fclose(file_);

        std::string filename = current_filename();
        file_ = fopen(filename.c_str(), "a");
        if (file_ == nullptr) {
            throw std::runtime_error("Failed to open file: " + filename);
        }

        last_rotate_time_ = Timestamp::now();

        if (max_files_ > 0) {
            cleanup_old_files();
        }
    }

    std::string current_filename() const {
        auto now = Timestamp::now();
        auto t = std::chrono::system_clock::to_time_t(now.get());
        std::tm tm;
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif

        char buffer[1024];
        std::size_t len = std::strftime(buffer, sizeof(buffer), filename_pattern_.c_str(), &tm);
        if(len == 0){
            throw std::runtime_error("Log filename pattern is too long or invalid");
        }
        return std::string(buffer, len);
    }

    void cleanup_old_files() {
        // 从文件名模板中提取目录和模式
        std::string pattern = filename_pattern_;
        std::string dir = ".";
        auto pos = pattern.find_last_of("/\\");
        if (pos != std::string::npos) {
            dir = pattern.substr(0, pos);
            pattern = pattern.substr(pos + 1);
        }

        // 收集匹配的文件
        std::vector<std::pair<std::filesystem::file_time_type, std::string>> files;
        try {
            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                std::string name = entry.path().filename().string();
                if (matches_pattern(name, pattern)) {
                    files.emplace_back(entry.last_write_time(), name);
                }
            }
        } catch (...) {
            return; // 目录不存在或无权限，静默忽略
        }

        // 按时间排序，删除最旧的
        if (static_cast<int>(files.size()) > max_files_) {
            std::sort(files.begin(), files.end());
            int to_delete = static_cast<int>(files.size()) - max_files_;
            for (int i = 0; i < to_delete; ++i) {
                std::string path = dir + "/" + files[i].second;
                std::remove(path.c_str());
            }
        }
    }

    bool matches_pattern(const std::string& filename, const std::string& pattern) const {
        // 简单匹配：检查文件名是否符合模式的基本结构
        // 这里做简化处理，只检查扩展名是否匹配
        auto dot_pos = pattern.find('.');
        if (dot_pos != std::string::npos) {
            std::string ext = pattern.substr(dot_pos);
            return filename.size() > ext.size() &&
                   filename.compare(filename.size() - ext.size(), ext.size(), ext) == 0;
        }
        return true;
    }

    std::string    filename_pattern_;
    RotationPeriod period_;
    int            max_files_;
    Timestamp      last_rotate_time_;
    std::FILE*     file_ = nullptr;
};

} // namespace cpp109
