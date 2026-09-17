# elog 压测

一套"跑得快但没丢"的压测：多线程并发写 `AsyncLogger`，每条消息带
`T<线程号>-<序号>` 帧，跑完重建位图逐行校验——不丢、不重、线程内不乱序。

## 文件

| 文件 | 作用 |
|---|---|
| `bench_stress.cpp` | 压测驱动（C++，只依赖 `elog`），负责发压、计时、校验 |
| `run_stress.py` | 场景编排：跑矩阵、汇总吞吐表、可选导出 CSV |
| `run.sh` | `run_stress.py` 的极简 shell 包装 |
| `CMakeLists.txt` | 构建 `elog_bench_stress`，可用 `-DELOG_BUILD_STRESS=OFF` 关闭 |

## 构建

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14
cmake --build build
```

必须是 Release/RelWithDebInfo：Debug 下的吞吐数字没有参考意义。
GCC 需 14+（项目依赖 C++23 `<print>`）。

## 运行

```bash
# 全场景（每个场景都会校验不丢日志）
python3 stress/run_stress.py

# 看有哪些场景
python3 stress/run_stress.py --list

# 只跑指定场景，每个跑 3 遍，结果导出 CSV
python3 stress/run_stress.py -s mt -s unbalanced -r 3 --csv /tmp/elog.csv

# 大场景把日志写到真实磁盘（默认在系统临时目录，可能是 tmpfs，占内存）
python3 stress/run_stress.py --log-root /data/elog-stress --keep-logs

# 直接跑二进制，最细粒度控制
./build/elog_bench_stress --threads 16 --messages 100000 --payload 256 --verify
./build/elog_bench_stress --help
```

## 场景矩阵

| 场景 | 覆盖点 |
|---|---|
| `mt` | 8 线程基准并发吞吐 |
| `mt32` | 32 线程高并发竞争 |
| `unbalanced` | 负载倾斜 4:1，看公平性 |
| `payload1k` / `payload16k` | 消息变大、逼近 64 KiB 单块容量 |
| `logsize` | 32 MiB 滚动阈值，频繁滚动与文件切换 |
| `flush1s` | 后台刷新节奏的影响 |
| `oversize` | 单条 64512 B，贴近 64 KiB 单块上限（再大驱动会拒绝） |

## 判读

- 退出码 `0` = 全程落盘且校验通过（未开 `--verify` 时只代表跑完）；
  `1` = 校验失败或参数错误。校验失败必须让进程返回非 0，否则脚本会把
  "丢了日志"当成通过。
- `排空(s)`（drain）是 `wait_for_done()` 的耗时，反映后台线程处理积压的速度；
  它远大于写入窗口通常说明消费者跟不上。
- 校验失败会打印分类计数：解析失败 / 未知线程 / 序号越界 / 重复 / 缺失 / 顺序，
  并给出**首个空洞**（哪个线程、哪一条）便于到日志里核对。

## 校验器自身的两个坑（都已修，留档避免重犯）

1. **顺序不能用整行字符串比较**：消息帧 `T<线程号>-<序号>-` 的序号不补零，
   `"T0-10-" < "T0-9-"` 在字典序下成立，会把正常输出判成乱序（8 个场景全被
   误报成失败）。现在比较解析后的数值。
2. **读文件不能用文件名字典序**：`FileManager` 同一秒内滚动会生成
   `name.log` / `name-1.log` / `name-2.log`，字典序把 `-1` 排到 `57` 前面，
   于是后写的文件被当成先写的，顺序校验大面积误报。现在按 `last_write_time`
   排序（目录由本程序独占新建，mtime 即写入顺序）。

## 已知边界（别当成压测结论）

1. **单条消息 > `LogBlock::kCap`（64 KiB）会被静默丢弃**——`append_message`
   换块后第二次 `append` 的返回值被忽略。所以驱动直接拒绝 `payload >= 64 KiB`，
   避免把这条已知缺陷统计成"并发丢日志"；`oversize` 场景停在 64512 B。
2. **日志目录必须存在**：`FileAppender` 不会自动建目录，缺目录只会降级为
   "不写文件"（`file logging disabled`）。驱动会显式建目录，建不出来直接失败。
3. **`wait_for_done()` 之后 `append_message` 会被静默丢弃**（`stop_accepting`），
   所以驱动严格按"join 生产者 → 排空"的顺序，反了就会统计出假丢失。
4. 压测结果强依赖磁盘：tmpfs 与机械盘的差距可能有数量级，跨机器比较时
   请连同 `--log-root` 指向的设备一起记录。
5. **二进制在 `build/stress/elog_bench_stress`**（子目录目标），不是 `build/` 根下；
   `run_stress.py` 会自动在几个常见位置查找，也可用 `--bin` 显式指定。
