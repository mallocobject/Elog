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

  <p align="center"><b>elog — asynchronous file logger, no global lock on the write path</b></p>
</div>

---

一个 C++23 的异步文件日志库。写入线程只做「格式化 + 拷进块」，落盘交给一个
后台消费者线程；多生产者之间用**无锁 SPSC 通道 + 满块批量交接**通信，没有
全局锁，也没有 CAS 循环。

- 头文件库（INTERFACE target），核心 6 个文件、约 760 行
- 格式化用标准库 `std::format`，不依赖 fmt
- 自动带**源位置**（文件、行号、函数名）——`std::source_location`，无宏魔法
- 文件按时间滚动（同名秒内自动加序号），可配滚动大小与刷新间隔
- 可挂回调 hook（`g_log_callback`），把日志同时送给别的系统

> ⚠️ 状态：**可用、有测试、有压测，但 API 未冻结**。已知缺陷见
> [已知限制](#已知限制)，最要紧的一条是「单条 > 64 KiB 的消息会被静默丢弃」。

## 目录

- [快速开始](#快速开始)
- [架构](#架构)
- [API](#api)
- [测试](#测试)
- [性能](#性能)
- [压测](#压测)
- [已知限制](#已知限制)
- [调优速查](#调优速查)

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
    // 目录必须先存在：elog 不会替你建目录（见「已知限制」第 2 条）
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

### 为什么这么切

- **每线程一个生产通道**：`thread_local ProducerCtx` 意味着热路径上生产者之间
  **零竞争**，唯一可能的等待是块用尽时向消费者要回收块。
- **按块而不是按条交接**：通道传的是 64 KiB 满块的指针，交接成本摊到几万条
  消息上；`SpscQueue` 只在 `flush()` 时做一次 `store`、`drain()` 做一次
  `exchange`，没有 CAS、没有 ABA。
- **实例号而不是指针校验归属**：对象销毁后地址会被分配器复用，用
  `AsyncLogger*` 比较会把「旧实例的通道」误判成自己的，导致新实例的日志
  投进无人 drain 的旧通道而静默丢失。`ProducerCtx::owner_id` 用单调递增的
  实例号，永不复用（见 `async_logger.hpp` 注释）。
- **关闭与停止接收分离**：`done_` 是「请求关闭」，`accepting_` 是「不再接收」。
  消费者可能因文件故障自行退出，那时 `done_` 仍为 false；若不单独停止接收，
  生产者会持续投递而无人 drain，内存随日志量无界增长——这是曾经的泄漏点。
- **宁泄漏不 use-after-free**：`do_done()` 刻意不释放 `producers_` 及其中的块，
  以少量泄漏换「进程退出瞬间仍有线程在写日志」时的安全。

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

- **28 个用例**（Catch2 v3），覆盖 `LogBlock` / `SpscQueue` / `FileAppender` /
  `FileManager` / `AsyncLogger` / 级别与宏。
- 用例间用 `ScopedLogState` 保存还原全局日志状态，避免互相串味。
- **不丢日志是断言项**：如「AsyncLogger 排空后所有日志恰好落盘一次」逐个
  校验每条消息出现且仅出现一次。
- 有一条用例**刻意不断言缺陷行为**（单条超块容量），注释里写明了理由：
  把缺陷固化成期望值，会让将来修好它的人被测试挡住。

## 性能

实测环境：**原生 Ubuntu 22.04（内核 `5.15.0-142-generic`，x86_64）裸机，
12 线程 Intel i5-12400F，日志写 `/tmp`（SATA SSD `SAMSUNG MZ7L3480`，
调度器 `mq-deadline`）**，GCC 14 / Release 构建。设备基线用 `dd` 实测：

| dd 跑法 | 结果 | 含义 |
|---|---|---|
| `oflag=direct`（绕页缓存） | 467 MB/s | 设备裸写能力 |
| buffered（吃页缓存） | 1.6 GB/s | 与下面压测同性质 |
| `conv=fdatasync`（写完即刷盘） | 323 MB/s | 真实持久化吞吐 |

下面的吞吐表**全部是 buffered 口径**（数据进页缓存），不是持久化吞吐。

| 场景 | 线程 | 消息 | 入队条/s | 入队 GiB/s | 端到端条/s |
|---|---|---|---|---|---|
| `mt` | 8 | 128 B | 22,015,164 | 2.91 | 6,451,613 |
| `mt32` | 32 | 128 B | 25,913,054 | 3.50 | 5,633,803 |
| `unbalanced`（4:1 倾斜） | 8 | 256 B | 7,587,099 | 1.97 | 3,333,312 |
| `payload1k` | 8 | 1024 B | 3,713,194 | 3.75 | 1,139,601 |
| `payload16k` | 4 | 16384 B | 205,875 | 3.30 | 43,011 |
| `logsize`（32 MiB 滚动） | 8 | 512 B | 7,407,476 | 3.77 | 1,702,128 |
| `flush1s` | 8 | 128 B | 21,905,065 | 2.95 | 9,090,909 |
| `oversize`（64 KiB 消息） | 2 | 64512 B | 43,619 | 2.75 | 19,704 |
| 大规模（1600 万条） | 8 | 128 B | 25,972,821 | 3.37 | — |

口径说明（**对外比较时必须说清楚**）：

- **入队条/s** = 消息数 ÷ 写入窗口（生产者 join 为止），不含排空。
- **端到端条/s** = 消息数 ÷（写入窗口 + 排空），**只有这一列能和别的库比**。
- 上表每一行都通过了「不丢、不重、线程内不乱序」的完整性校验。
- 存在**规模效应**：1600 万条规模比 160 万条高约 18%（2.20 → 2.60 千万条/s），
  小规模测试会低估吞吐（两轮的 `--verify` 设置不同，含校验开销的差异）。

### 四条结论

1. **瓶颈不在存储，在消费者线程**。同一负载把日志目录换到 tmpfs（纯内存），
   吞吐几乎不变（2.87 → 2.63/2.92 GiB/s），只有排空时间从 0.17 s 降到 0.10 s。
   既然绕开整条存储栈没有变化，剩下的就是 CPU 侧。
2. **存在一条与并发无关的天花板，约 2.5–2.9 GiB/s**。2 线程写 64 KiB 消息、
   24 线程写 128 B 消息，撞到的是同一堵墙（见 `oversize` 与 `mt`）。
3. **并发在 12 线程（=逻辑核数）到顶**：4/8/12 线程为 1.14/2.22/2.34 千万条/s，
   16/24 线程回落到 2.23/2.41 千万条/s，而排空时间从 0.10 s 涨到 0.47 s——
   再加生产者只增加积压。
4. **负载倾斜有代价**：同样 64 万条，4:1 倾斜比均匀负载低 44%
   （3,386 → 1,914 MiB/s）。消费者被重载生产者的满块占住，轻载生产者的块排队。

### 系统调用与内存（`strace` / `/usr/bin/time -v` 实测）

| 指标 | 实测值 |
|---|---|
| `fsync` / `fdatasync` 调用 | **0 次**（整个运行期） |
| `write` 调用 | 6,786 次 / 221.5 MB，均摊 **32.6 KiB/次**；无单次 ≥ 64 KiB |
| 系统调用总耗时 | **0.268 s**（占整个运行很小一部分，不是瓶颈） |
| 峰值 RSS vs 落盘量 | 216 MB / 215 MB、1.09 GB / 1.11 GB、26.6 GB / 32.9 GB |

RSS 一列读法：峰值内存**与本次落盘字节数同阶**，与消息条数无关
（固定 100 万条、payload 从 16 B 到 4096 B，落盘量差 150 倍，而 RSS 若按条数
增长就该是常数——实测是跟着字节走）。这是**写入方上下文里的脏页计入 RSS**，
不是泄漏；32.9 GB 那档被内核回写回收压到 26.6 GB，正是触及脏页上限的证据。

### 持久化语义（重要）

`fsync` 调用为 0，文件日志靠 stdio 缓冲 + 每 `flush_interval`（默认 3 秒）
一次 `fflush`；数据进页缓存即算「已落盘」。因此：

- 进程被 `SIGKILL`、机器断电时，**最近一个 `flush_interval` 窗口内的日志会丢**
  （实测路径：消费者每 3 秒唤醒一次才 drain + flush）；
- 上面的 2.5–3.8 GiB/s 全部是页缓存速率，设备真实持久化能力是 **323 MB/s
  量级**——持续写入超过内存+脏页上限后，吞吐会掉到设备水平，并根据回写压力
  反压生产者。

### 与 spdlog 等库的对比

**本仓库没有做过任何跨库实测对比，请不要引用「比 X 快 N 倍」的说法。**
从上面第 1、2 条能推断的只是量级：这类库的瓶颈都是「单线程落盘 + 每条消息
格式化」，真实差距通常来自「每条消息几次内存分配」，落在 2–3 倍以内；能拉开
10 倍以上的只有「关日志时是否提前返回」和「磁盘打满时能否不阻塞生产者」。

已确认的一处结构性劣势（相对 spdlog 一代的异步实现）：

- 热路径上**每条消息至少两次堆分配**：`std::vformat` 的结果与拼接后的
  `std::string fmsg`；对方用 thread_local 复用缓冲，可以做到零分配。

## 压测

`stress/` 下有一套带**完整性校验**的压测（不只是量吞吐）：

```bash
# 全场景矩阵，每个场景开 --verify，每场景 2 轮
python3 stress/run_stress.py --repeat 2 --csv /tmp/elog-stress.csv

# 单跑
./build/stress/elog_bench_stress --threads 8 --messages 200000 --payload 128 --verify
./build/stress/elog_bench_stress --help
```

每条消息带 `T<线程号>-<序号>-` 帧，跑完重建位图逐行校验：**不丢、不重、
线程内不乱序**；失败时打印分类计数与首个空洞（哪个线程、哪一条）。详见
[`stress/README.md`](stress/README.md)。

> 校验器自身踩过三个坑，都写在 `stress/README.md` 里留档：序号不补零不能用
> 字符串比大小、滚动文件名不能按字典序读、文件多了 mtime 会撞在一起。
> 现在按「每个文件里的最小序号」排序，数据自己说明写入顺序。

## 已知限制

1. **单条消息 > 64 KiB（`LogBlock::kCap`）会被静默丢弃**：`append_message`
   换块后第二次 `append` 的返回值被忽略——不截断、不报错、不分片。压测驱动
   因此直接拒绝 `payload >= 64 KiB`，避免把这条缺陷算成并发丢日志。
2. **日志目录必须已存在**：`FileAppender` 不做 `create_directories`，
   `fopen` 失败会走降级路径——构造 `AsyncLogger` 不抛异常，而是由消费者线程
   打印 `[elog] file logging disabled: ...` 后停止接收日志。**日志会静默不落盘，
   终端输出不受影响。**
3. **`wait_for_done()` 之后 `append_message` 会被静默丢弃**（`accepting_` 置
   false）。这也是为什么压测必须「先 join 生产者、再排空」，反了会统计出假丢失。
4. **关闭时有意泄漏 `ProducerCtx` 与其中的块**，换取「退出瞬间仍有人写日志」
   时不 use-after-free。
5. 长期运行会持续新建生产者通道且不回收（`producers_` 只增不减），
   **线程反复创建/销毁的负载下需注意**。
6. 无 `fsync`、无按大小之外的保留策略（不删旧文件）、无压缩。
7. `LOG_xxx` 目前每个调用点一条日志，没有提供「一次格式化、多处输出」。

## 调优速查

| 现象 | 优先看 |
|---|---|
| 排空时间远大于写入窗口 | 消费者跟不上：调大 `flush_interval`、减少滚动、把文件放到本地盘 |
| 条/s 上不去但 GiB/s 已到 2.5+ | 到带宽天花板了，减少前缀开销或改用更小的消息 |
| 加线程吞吐不再涨、排空变长 | 生产者已够多，别再线程化；瓶颈在消费者单线程 |
| 负载倾斜时吞吐明显下降 | 已知公平性代价（见性能表 `unbalanced`），大量小生产者 + 个别重生产者会放大 |
| 写日志时延迟抖动大 | 生产者可能阻塞在「等回收块」；增大消费者节奏或减少单条体积 |

---

许可证与贡献约定尚未确定；在确定之前请勿假定可再分发。

仓库：<https://github.com/mallocobject/Elog>
