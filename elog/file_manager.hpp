#pragma once

#include "elog/file_appender.hpp"
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace elog::details {
class FileManager {
  public:
    explicit FileManager(
        std::string dir,
        std::string prefix,
        size_t roll_size = 100 * 1024 * 1024,
        std::chrono::seconds flush_interval = std::chrono::seconds(3),
        size_t check_per_count = 1024)
        : dir_(std::move(dir)), prefix_(std::move(prefix)),
          roll_size_(roll_size), flush_interval_(flush_interval),
          check_per_count_(check_per_count) {
        roll_file();
    }

    void append(std::string_view data) {
        append(data.data(), data.size());
    }

    void append(const char *data, size_t len);

    void flush() {
        file_->flush();
    }

  private:
    void roll_file(std::chrono::system_clock::time_point now =
                       std::chrono::system_clock::now());
    const std::string dir_;
    const std::string prefix_;
    const size_t roll_size_;
    const std::chrono::seconds flush_interval_;
    const size_t check_per_count_;
    // 与 check_per_count_ 同为 size_t：混用有符号和无符号比较会触发
    // -Wsign-compare；这个计数只会自增和归零，不会为负
    size_t count_{0};

    std::unique_ptr<FileAppender> file_;
    std::string current_path_;
    int same_second_count_{0};

    std::chrono::time_point<std::chrono::system_clock, std::chrono::days>
        last_day_;
    std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds>
        last_roll_second_;
    std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds>
        last_flush_second_;
};

inline void FileManager::append(const char *data, size_t len) {
    file_->append(data, len);
    if (file_->written_bytes() > roll_size_) {
        roll_file();
        file_->reset_written_bytes();
    } else {
        count_++;
        if (count_ >= check_per_count_) {
            count_ = 0;
            auto now = std::chrono::system_clock::now();
            auto now_day = std::chrono::floor<std::chrono::days>(now);
            if (now_day != last_day_) {
                roll_file(now);
            } else if (now - last_flush_second_ >= flush_interval_) {
                last_flush_second_ =
                    std::chrono::floor<std::chrono::seconds>(now);
                file_->flush();
            }
        }
    }
}

inline void FileManager::roll_file(std::chrono::system_clock::time_point now) {
    last_roll_second_ = last_flush_second_ =
        std::chrono::floor<std::chrono::seconds>(now);
    last_day_ = std::chrono::floor<std::chrono::days>(now);

    // 刻意不创建目录:目录不存在时打开失败,由 AsyncLogger 捕获后降级为
    // 不写文件(见 test.cpp 的"日志目录不存在时降级"用例)。静默建目录会
    // 掩盖配置写错的路径,这是本库明确的契约。
    std::chrono::zoned_time zt{std::chrono::current_zone(), last_roll_second_};

    // 同一秒内多次滚动时加序号后缀,避免反复 append 到同一个文件
    // 文件名里不能出现 ':':Windows 会把它当作驱动器/备用数据流(ADS)分隔符,
    // 打开必然失败。因此时分秒用 '-' 分隔,保证在 Windows 上是合法文件名。
    std::string path =
        std::format("{}/{}{:%Y-%m-%dT%H-%M-%S}.log", dir_, prefix_, zt);
    if (path == current_path_) {
        path = std::format("{}/{}{:%Y-%m-%dT%H-%M-%S}-{}.log",
                           dir_,
                           prefix_,
                           zt,
                           ++same_second_count_);
    } else {
        same_second_count_ = 0;
        current_path_ = path;
    }

    file_ = std::make_unique<FileAppender>(path);
}
} // namespace elog::details
