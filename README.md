<div align="center">
  <pre>
███████╗██╗      ██████╗  ██████╗ 
██╔════╝██║     ██╔═══██╗██╔════╝ 
█████╗  ██║     ██║   ██║██║  ███╗
██╔══╝  ██║     ██║   ██║██║   ██║
███████╗███████╗╚██████╔╝╚██████╔╝
╚══════╝╚══════╝ ╚═════╝  ╚═════╝ 
  </pre>

  <p align="center">
    <img src="https://img.shields.io/badge/C%2B%2B-23-blue.svg" alt="C++23">
    <img src="https://img.shields.io/badge/header--only-yes-success.svg" alt="Header only">
    <img src="https://img.shields.io/badge/platform-Linux-red.svg" alt="Platform">
    <img src="https://img.shields.io/badge/build-CMake-blueviolet.svg" alt="CMake">
    <img src="https://img.shields.io/badge/status-Developing-green.svg" alt="Developing">
  </p>

  <p align="center"><b>Elog — asynchronous file logger, no global lock on the write path</b></p>
</div>

---

一个 C++23 的异步文件日志库...


## 目录

- [快速开始](#快速开始)
- [架构](#架构)
- [API](#api)
- [测试](#测试)

## 快速开始

要求：**GCC 14+**（依赖 C++23 `<print>`）、CMake 3.22+。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14
cmake --build build
cd build && ctest            # 28 个单元测试
```

作为子目录接进你的工程：

```cmake
add_subdirectory(third_party/elog)
target_link_libraries(your_target PRIVATE elog)
```

最小可用示例（`elog/logger.hpp` 是唯一需要 include 的头）：

```cpp
#include "elog/logger.hpp"
#include <filesystem>

int main() {
    // 目录必须先存在：elog 不会替你建目录
    std::filesystem::create_directories("logs");

    elog::set_log_path("logs", "app-", 100 * 1024 * 1024, std::chrono::seconds(3), 1024);
    elog::set_log_threshold(elog::LogLevel::INFO);

    LOG_INFO("user {} logged in from {}", 42, "10.0.0.1");
    LOG_ERROR("open {} failed: {}", "config.yaml", "ENOENT");

    // 退出前排空：析构也会做，但显式调用语义更清楚
    elog::details::g_log_file->wait_for_done();
}
```

默认行为：级别阈值 `INFO`；**不写文件、只打终端**（带 ANSI 颜色）；
低于阈值的日志在**格式化之前**就返回（`lv < g_log_threshold` 提前 return）。

环境变量（进程启动时读一次）：

| 变量 | 作用 |
|---|---|
| `ELOG_PATH` | 日志目录；设置了就在启动时开启文件日志（前缀为空、默认滚动参数） |
| `ELOG_LEVEL` | 级别阈值，如 `DEBUG`、`WARN`；非法值回落到 `INFO` |

## 架构

```
 写入线程 (N 个)                        消费者线程 (1 个)
 ─────────────────                     ──────────────────────────────────
 LOG_INFO(...)
   │ ① lv < threshold ? 直接 return（零成本）
   │ ② std::vformat 格式化正文
   │ ③ 拼前缀：时间戳[线程号]<级别> 文件:行 函数()-> 正文
   │ ④ append_message(fmsg)
   │      ├─ 拷进线程私有的 cur 块（一次 memcpy，快路径）
   │      └─ 块满 → full 通道 push + 换块 + 唤醒消费者
   └──────────────────────────────►  drain() 整批取走
                                       │ ⑤ FileManager::append 逐块落盘
                                       │ ⑥ 满 roll_size 或跨天 → 滚动新文件
                                       └ ⑦ 满 flush_interval → fflush
```

| 文件 | 职责 |
|---|---|
| `elog/logger.hpp` | 对外 API：级别枚举、`LOG_xxx`、阈值、回调、终端输出、源位置捕获 |
| `elog/async_logger.hpp` | 生产者注册与块池、后台消费者循环、关闭与排空 |
| `elog/spsc_queue.hpp` | 单生产者/单消费者批量交接通道（`push`/`flush`/`drain`/`to_fifo`） |
| `elog/log_block.hpp` | 64 KiB 定长块（`kCap`），append 失败不做部分写入 |
| `elog/file_manager.hpp` | 滚动策略：按大小、按天、按秒刷新 |
| `elog/file_appender.hpp` | `FILE*` 的 RAII 包装，`fwrite_unlocked` 写、`fclose` 兜底 flush |


## API

```cpp
// 级别：TRACE < DEBUG < INFO < WARN < ERROR < FATAL
enum class LogLevel : std::uint8_t;

// 启动文件日志。注意：目录必须已存在（见已知限制）
void set_log_path(const std::string& dir, const std::string& prefix,
                  size_t roll_size, std::chrono::seconds flush_interval,
                  size_t check_per_count);

void set_log_threshold(LogLevel lv);         // 运行期可改，原子变量

// 六个函数模板，按 std::format 规则展开参数
LOG_TRACE(fmt, args...);  LOG_DEBUG(...);  LOG_INFO(...);
LOG_WARN(...);            LOG_ERROR(...);  LOG_FATAL(...);

// 可选的回调 hook：设置后走「回调路径」，拿到的是**不含前缀的原始消息**
inline std::function<void(LogLevel, std::string_view)> g_log_callback;

// 关闭并排空（析构里也会做；显式调用可避免把开销算在对象销毁点）
elog::details::g_log_file->wait_for_done();
```

日志行格式：

```
2026-09-17 13:31:41.123456789[140234][INFO] src/main.cpp:42 main()-> user 42 logged in
└─ 本地时区时间戳（只查一次 tzdb）  └─ 线程号   └─ 级别  └─ 源位置（编译期捕获）  └─ 正文
```

`LOG_xxx` 是**函数模板，不是宏**；源位置靠 `consteval WithSourceLocation`
在调用点捕获，因此 `fmt` 是编译期检查的 `std::format_string`，写错占位符
是编译错误而不是运行期异常。

## 测试

```bash
cd build && ctest --output-on-failure      # 单跑某个用例
./test/elog_test "FileManager 超过 roll_size 后滚动且不丢数据"
```

仓库：<https://github.com/mallocobject/Elog>
