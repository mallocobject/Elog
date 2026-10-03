#pragma once

// MSVC 把 getenv 一类 CRT 函数标记为"不安全"(C4996)。该宏必须在本头文件
// 拉入任何 CRT 头文件之前生效;项目属性里也定义了它,这里再兜一层,
// 保证单独包含本头文件(例如被其他工程引用)时同样干净。
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "elog/async_logger.hpp"
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

// Windows 控制台默认不解释 ANSI 转义序列,必须调用内核态 API 打开
// ENABLE_VIRTUAL_TERMINAL_PROCESSING。这里只声明用到的三个函数,刻意不包含
// <windows.h>:它会经 wingdi.h 定义宏 ERROR=0,使 LogLevel::ERROR 在预处理后
// 变成常数 0,连 enum 都无法通过编译;此外还会引入 min/max 宏和 rpcndr.h 的
// #define small char 等大量污染。下面声明与 windows.h/WINAPI 完全一致,
// 因此用户代码同时包含 <windows.h> 也不会冲突。
#ifdef _WIN32
extern "C" {
__declspec(dllimport) void *__stdcall GetStdHandle(unsigned long nStdHandle);
__declspec(dllimport) int __stdcall GetConsoleMode(void *hConsoleHandle,
                                                   unsigned long *lpMode);
__declspec(dllimport) int __stdcall SetConsoleMode(void *hConsoleHandle,
                                                   unsigned long dwMode);
}
#endif

namespace elog {
#define ELOG_FOREACH_LOG_LEVEL(f)                                              \
    f(TRACE) f(DEBUG) f(INFO) f(WARN) f(ERROR) f(FATAL)

enum class LogLevel : std::uint8_t {
#define _FUNCTION(name) name,
    ELOG_FOREACH_LOG_LEVEL(_FUNCTION)
#undef _FUNCTION
};

namespace details {
inline constexpr const char
    level_ansi_colors[static_cast<std::uint8_t>(LogLevel::FATAL) + 1][6] = {
        "\033[90m", "\033[36m", "\033[32m", "\033[33m", "\033[31m", "\033[35m"};

inline constexpr std::string_view log_level_to_string(LogLevel lv) {
    switch (lv) {
#define _FUNCTION(name)                                                        \
    case LogLevel::name:                                                       \
        return #name;
        ELOG_FOREACH_LOG_LEVEL(_FUNCTION)
#undef _FUNCTION
    }
    return "UNKNOWN";
}

inline constexpr LogLevel log_level_from_string(std::string_view lv) {
#define _FUNCTION(name)                                                        \
    if (lv == #name)                                                           \
        return LogLevel::name;
    ELOG_FOREACH_LOG_LEVEL(_FUNCTION)
#undef _FUNCTION

    return LogLevel::INFO;
}

// Windows 上环境变量本身是 UTF-16,而 CRT 的 getenv 会先按 ANSI 代码页
// 把它降级成窄字符——中文路径会因此损坏(GBK 字节被当成 UTF-8 解析)。
// 这里用宽字符版读取再转成 UTF-8,保证库内"路径一律 UTF-8"的约定成立。
// 返回空串同时表示"未设置"。
inline std::string env_utf8(const char *name) {
#ifdef _WIN32
    std::wstring wname;
    for (const char *p = name; *p != '\0'; ++p) {
        wname.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    }
    const wchar_t *const value = ::_wgetenv(wname.c_str());
    if (value == nullptr) {
        return {};
    }
    const std::u8string utf8 = std::filesystem::path(value).u8string();
    return std::string(reinterpret_cast<const char *>(utf8.data()),
                       utf8.size());
#else
    const char *const value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string{};
#endif
}

// 输出被重定向到文件或管道时(不是控制台)不返回成功,调用方因此
// 完全不发颜色码,日志文件里不会出现转义序列。
inline bool detect_ansi_support() noexcept {
#ifdef _WIN32
    constexpr unsigned long kStdOutputHandle =
        static_cast<unsigned long>(-11); // STD_OUTPUT_HANDLE
    constexpr unsigned long kEnableVirtualTerminalProcessing = 0x0004;

    const void *const invalid_handle =
        reinterpret_cast<const void *>(static_cast<std::intptr_t>(-1));

    void *const handle = ::GetStdHandle(kStdOutputHandle);
    if (handle == nullptr || handle == invalid_handle) {
        return false;
    }

    unsigned long mode = 0;
    if (::GetConsoleMode(handle, &mode) == 0) {
        return false; // 取不到控制台模式,说明输出已重定向
    }
    if ((mode & kEnableVirtualTerminalProcessing) != 0) {
        return true;
    }
    return ::SetConsoleMode(handle, mode | kEnableVirtualTerminalProcessing) !=
           0;
#else
    return true;
#endif
}

// 只探测一次:每次 SetConsoleMode 都有系统调用开销
inline bool ansi_colors_enabled() noexcept {
    static const bool enabled = detect_ansi_support();
    return enabled;
}

// 返回空串表示"当前输出不应带颜色"
inline const char *level_color(LogLevel lv) noexcept {
    return ansi_colors_enabled()
               ? level_ansi_colors[static_cast<std::uint8_t>(lv)]
               : "";
}

template <typename T>
class WithSourceLocation {
  public:
    template <typename U>
        requires std::constructible_from<T, U>
    consteval WithSourceLocation(
        U &&inner, std::source_location loc = std::source_location::current())
        : inner_(std::forward<U>(inner)), loc_(std::move(loc)) {
    }

    constexpr const T &format() const noexcept {
        return inner_;
    }

    constexpr const std::source_location &location() const noexcept {
        return loc_;
    }

  private:
    T inner_;
    std::source_location loc_;
};

inline auto g_log_file = []() -> std::unique_ptr<AsyncLogger> {
    const std::string path = env_utf8("ELOG_PATH");
    if (!path.empty()) {
        return std::make_unique<AsyncLogger>(path, "");
    }
    return nullptr;
}();

inline std::atomic<LogLevel> g_log_threshold{[]() -> LogLevel {
    const std::string lv = env_utf8("ELOG_LEVEL");
    if (!lv.empty()) {
        return log_level_from_string(lv);
    }
    return LogLevel::INFO;
}()};

// hook function, empty implement in default case
inline auto g_log_callback = std::function<void(LogLevel, std::string_view)>{};

// 时区只需查一次,避免每条日志做 tzdb 查找
inline const std::chrono::time_zone *cached_zone() {
    static const std::chrono::time_zone *zone = std::chrono::current_zone();
    return zone;
}

inline void
output_log(LogLevel lv, std::string_view msg, const std::source_location &loc) {
    if (lv < g_log_threshold.load(std::memory_order_relaxed)) {
        return;
    }

    // 只在需要前缀的路径(文件日志 / 终端输出) 才做时间戳格式化;
    // 仅回调场景下直接把原始消息交给调用方
    if (g_log_file == nullptr && g_log_callback) {
        g_log_callback(lv, msg);
        return;
    }

    thread_local std::uint64_t tid =
        std::hash<std::thread::id>{}(std::this_thread::get_id());

    std::chrono::zoned_time now{cached_zone(),
                                std::chrono::system_clock::now()};
    std::string fmsg = std::format("{}[{}]<{}> {}:{} {}()-> {}",
                                   now,
                                   tid,
                                   log_level_to_string(lv),
                                   loc.file_name(),
                                   loc.line(),
                                   loc.function_name(),
                                   msg);

    if (g_log_file) {
        fmsg += '\n';
        g_log_file->append_message(fmsg);
    }

    if (g_log_callback) {
        g_log_callback(lv, msg);
    } else {
        const char *color = level_color(lv);
        if (color[0] != '\0') {
            std::cout << color << fmsg << "\033[0m" << std::endl;
        } else {
            std::cout << fmsg << std::endl;
        }
    }
}
} // namespace details

inline void set_log_path(const std::string &dir,
                         const std::string &prefix,
                         size_t roll_size,
                         std::chrono::seconds flush_interval,
                         size_t check_per_count) {
    details::g_log_file = std::make_unique<details::AsyncLogger>(
        dir, prefix, roll_size, flush_interval, check_per_count);
}

// std::atomic::store 不是 constexpr,本函数不能声明为 constexpr
inline void set_log_threshold(LogLevel lv) {
    details::g_log_threshold.store(lv, std::memory_order_relaxed);
}

template <typename... Args>
void log(LogLevel lv,
         details::WithSourceLocation<std::format_string<Args...>> fmt,
         Args &&...args) {
    if (lv < details::g_log_threshold.load(std::memory_order_relaxed)) {
        return;
    }

    auto msg = std::vformat(fmt.format().get(), std::make_format_args(args...));
    details::output_log(lv, msg, fmt.location());
}

#define _FUNCTION(name)                                                        \
    template <typename... Args>                                                \
    void LOG_##name(                                                           \
        details::WithSourceLocation<std::format_string<Args...>> fmt,          \
        Args &&...args) {                                                      \
        return log(                                                            \
            LogLevel::name, std::move(fmt), std::forward<Args>(args)...);      \
    }
ELOG_FOREACH_LOG_LEVEL(_FUNCTION)
#undef _FUNCTION
} // namespace elog
