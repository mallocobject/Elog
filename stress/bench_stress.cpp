// elog 压测驱动：多线程并发写入 AsyncLogger，全程带序号，跑完校验
// "每条日志恰好落盘一次"（不丢、不重、不错序）。
//
// 语义约定（与 test/elog.cpp 中已有的结论一致，别当成缺陷去改）：
//   * 文件日志由 AsyncLogger 的后台线程批量落盘，测试结束必须
//     wait_for_done() 主动排空，否则会误判为"丢日志"；
//   * Consumer 一旦 stop_accepting()，之后的 append_message 会被静默丢弃 ——
//     所以先 join 生产者、再排空，顺序不能反；
//   * 消息长度恰好等于 LogBlock::kCap 可以写入，超过则被静默丢弃（已知缺陷，
//     见 test/elog.cpp 同名注释）。本程序默认拒绝 payload >= kCap，
//     避免把"缺陷导致的丢弃"统计成"并发丢日志"；
//   * 目录不存在时 fopen 失败会降级为"不写文件"，本程序显式建目录，
//     避免把降级误当成压测结果。
//
// 用法：
//   elog_bench_stress --threads 8 --messages 200000 --payload 128 --verify
//                     （--roll-size 64M --flush-interval 1 可选，见 --help）
// 退出码：0 = 全部落盘且校验通过（未开 --verify 时只代表跑完）；
//         1 = 校验失败 / 参数错误。

#include "elog/async_logger.hpp"
#include "elog/log_block.hpp"
#include "elog/spsc_queue.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using elog::details::AsyncLogger;
using elog::details::LogBlock;

// 预热消息用这个"不会出现的线程号"标记，校验阶段直接排除，
// 否则预热用的 (线程 0, 序号 0..N) 会和正式计数的第一段序号撞车，
// 把预热统计成"重复 + 缺失"。
constexpr std::uint32_t kWarmupTid = 0xFFFFu;

// 消息格式：T<线程号>-<序号>-<填充>，行尾 '\n'。
// 顺序检查只比较同一线程的相邻两行，所以不需要定宽——同线程内序号
// 是单调递增的十进制文本，字典序天然等于数值序。
std::string make_message(std::uint32_t thread_id,
                         std::size_t seq,
                         std::size_t payload) {
    std::string frame = std::format("T{}-{}-", thread_id, seq);
    std::string msg;
    msg.reserve(frame.size() + payload + 1);
    msg += frame;
    msg.append(payload, static_cast<char>('a' + thread_id % 26));
    msg += '\n';
    return msg;
}

struct Config {
    std::uint32_t threads{8};
    std::size_t messages{200'000};   // 每个线程的消息数
    std::size_t payload{128};        // 每条消息的填充字节数
    std::size_t roll_size{64ull * 1024 * 1024};
    std::uint32_t flush_interval{1};
    std::size_t check_per_count{1024};
    std::size_t warmup{1'000};
    // 负载倾斜：每 4 个线程里有 1 个写入量是其他的 4 倍，用来暴露
    // "某个生产者独占消费者" 之类的公平性问题（总和保持不变）。
    bool skew_heavy{false};
    bool verify{false};
    bool keep_logs{false};
    bool json{false};
    fs::path log_dir;
};

struct ParsedLine {
    std::uint32_t tid{0};
    std::size_t seq{0};
    bool ok{false};
};

// 从原始行里尽力解析出 T<tid>-<seq>-；前缀（时间戳等）无关，直接 find('T')。
// 校验阶段要逐行跑上百万次，这里全部走 from_chars，不做字符串切分。
ParsedLine parse_line(std::string_view line) {
    const auto pos = line.find('T');
    if (pos == std::string_view::npos) {
        return {};
    }
    std::string_view rest = line.substr(pos + 1);

    std::uint32_t tid = 0;
    auto res = std::from_chars(rest.data(), rest.data() + rest.size(), tid);
    if (res.ec != std::errc{} || res.ptr == rest.data() || *res.ptr != '-') {
        return {};
    }
    rest = rest.substr(static_cast<std::size_t>(res.ptr - rest.data()) + 1);

    std::size_t seq = 0;
    res = std::from_chars(rest.data(), rest.data() + rest.size(), seq);
    if (res.ec != std::errc{} || res.ptr == rest.data() || *res.ptr != '-') {
        return {};
    }
    return ParsedLine{tid, seq, true};
}

[[nodiscard]] std::uint64_t bytes_of(const fs::path &dir) {
    std::uint64_t total = 0;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec)) {
            total += entry.file_size(ec);
        }
    }
    return total;
}

struct VerifyResult {
    std::size_t lines{0};
    std::size_t warmup{0};
    std::size_t unparsable{0};
    std::size_t unknown_tid{0};
    std::size_t out_of_range{0};
    std::size_t nonzero{0};
    std::size_t duplicates{0};
    std::size_t missing{0};
    std::size_t first_missing_tid{0};
    std::size_t first_missing_seq{0};
    bool order_ok{true};
    std::size_t order_violation_line{0};
    std::string detail;

    [[nodiscard]] bool ok() const {
        return unparsable == 0 && unknown_tid == 0 && out_of_range == 0 &&
               duplicates == 0 && missing == 0 && order_ok;
    }
};

// 逐文件、逐行做一次完整校验。
VerifyResult verify_dump(const fs::path &dir,
                         std::uint32_t threads,
                         const std::vector<std::size_t> &quota) {
    VerifyResult r;

    std::size_t bits_per_thread = 1;
    for (const std::size_t q : quota) {
        bits_per_thread = std::max(bits_per_thread, q + 1);
    }
    const std::size_t words = (bits_per_thread + 63) / 64;
    std::vector<std::vector<std::uint64_t>> seen(threads);
    for (auto &v : seen) {
        v.assign(words, 0);
    }

    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".log") {
            files.push_back(entry.path());
        }
    }
    if (ec && files.empty()) {
        r.detail = std::format("无法遍历目录 {}: {}", dir.string(), ec.message());
        return r;
    }
    if (files.empty()) {
        r.detail = std::format("目录 {} 下没有任何 .log 文件（日志根本没落盘？）",
                               dir.string());
        return r;
    }

    // 滚动文件的读取顺序必须自己定，不能靠文件名，也不能靠 mtime：
    //   * 文件名字典序会把 name-1.log 排到 name.log 前（同一秒滚动）；
    //   * 一次跑出几百个文件时，mtime 也会撞在一起，退化回字典序。
    // 数据本身给了最可靠的键：FileManager 是"写满才滚"，所以先写的文件里
    // 装的必然是更早的序号段。用每个文件出现过的**最小序号**排序即可。
    struct FileMeta {
        fs::path path;
        std::size_t min_seq{std::numeric_limits<std::size_t>::max()};
    };

    std::vector<FileMeta> metas;
    metas.reserve(files.size());
    {
        std::string scan_buf;
        for (const auto &file : files) {
            FileMeta meta;
            meta.path = file;
            std::ifstream in(file, std::ios::binary);
            if (!in) {
                r.detail = std::format("无法打开 {}", file.string());
                return r;
            }
            while (std::getline(in, scan_buf)) {
                const ParsedLine p = parse_line(scan_buf);
                if (p.ok && p.tid != kWarmupTid) {
                    meta.min_seq = std::min(meta.min_seq, p.seq);
                }
            }
            metas.push_back(std::move(meta));
        }
        std::sort(metas.begin(), metas.end(),
                  [](const FileMeta &a, const FileMeta &b) {
                      if (a.min_seq != b.min_seq) {
                          return a.min_seq < b.min_seq;
                      }
                      return a.path.filename() < b.path.filename();
                  });
    }

    // 顺序校验：同一线程内序号必须严格递增。
    // 绝不能拿整行做字符串比较 —— 序号不补零，"T0-10-" < "T0-9-" 在字典序下
    // 成立，会稳定误报（第一版校验器就是这么错的，8 个场景全被误判成失败）。
    std::vector<std::size_t> last_seq(threads, 0);
    std::vector<bool> has_last(threads, false);

    std::string buf;
    for (const auto &meta : metas) {
        const fs::path &file = meta.path;
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            r.detail = std::format("无法打开 {}", file.string());
            return r;
        }
        while (std::getline(in, buf)) {
            if (!buf.empty() && buf.back() == '\r') {
                buf.pop_back();
            }
            r.lines++;

            const ParsedLine p = parse_line(buf);
            if (!p.ok) {
                r.unparsable++;
                continue;
            }
            if (p.tid == kWarmupTid) {
                r.warmup++; // 预热消息，不参与不丢/不重统计
                continue;
            }
            if (p.tid >= threads) {
                r.unknown_tid++;
                continue;
            }
            if (p.seq >= bits_per_thread) {
                r.out_of_range++;
                continue;
            }
            std::uint64_t &word = seen[p.tid][p.seq / 64];
            const std::uint64_t mask = 1ull << (p.seq % 64);
            if (word & mask) {
                r.duplicates++;
            } else {
                word |= mask;
                r.nonzero++;
            }

            if (r.order_ok && has_last[p.tid] && p.seq <= last_seq[p.tid]) {
                r.order_ok = false;
                r.order_violation_line = r.lines;
                r.detail = std::format(
                    "线程 {} 的顺序被破坏：{} 第 {} 行序号 {} 不大于上一行 {}",
                    p.tid,
                    file.filename().string(),
                    r.lines,
                    p.seq,
                    last_seq[p.tid]);
            }
            last_seq[p.tid] = p.seq;
            has_last[p.tid] = true;
        }
    }

    std::size_t expected = 0;
    for (const std::size_t q : quota) {
        expected += q;
    }

    // 缺失去重后再报：位图里 0 的位就是"从没落盘的序号"。
    // 顺便记住第一个空洞（哪个线程、哪一条），便于事后到日志里核对。
    bool first_gap_found = false;
    for (std::uint32_t t = 0; t < threads; ++t) {
        for (std::size_t s = 0; s < quota[t]; ++s) {
            if ((seen[t][s / 64] & (1ull << (s % 64))) != 0) {
                continue;
            }
            r.missing++;
            if (!first_gap_found) {
                first_gap_found = true;
                r.first_missing_tid = t;
                r.first_missing_seq = s;
            }
        }
    }
    if (r.missing != 0 && r.detail.empty()) {
        r.detail = std::format("缺少 {} 条（期望 {}，实际 {}），首个空洞 T{}-{}",
                               r.missing,
                               expected,
                               r.nonzero,
                               r.first_missing_tid,
                               r.first_missing_seq);
    }
    return r;
}

void print_usage() {
    std::println(R"(elog 压测驱动

  --threads N           生产者线程数               (默认 8)
  --messages N          每个线程写入的消息数       (默认 200000)
  --payload N           每条消息的填充字节数       (默认 128)
  --roll-size N         滚动阈值，支持 1024/64K/64M 后缀 (默认 64M)
  --flush-interval N    后台刷新间隔秒数           (默认 1)
  --check-per-count N   每 N 次 append 检查一次滚动/刷新 (默认 1024)
  --warmup N            不计入计时的预热消息数     (默认 1000，帧里用 T65535- 标记，
                        校验时单独统计，不会和正式序号撞车)
  --skew-heavy          负载倾斜：每 4 个线程里 1 个写 4 倍量（总量守恒）
  --verify              跑完重建位图，逐行校验不丢不重不乱序
  --keep-logs           保留日志目录（默认删除）
  --json                额外输出一行 JSON 结果，便于脚本采集
  --log-dir PATH        指定日志目录（默认: <临时目录>/elog-stress-<pid>-<时刻>）
  -h, --help            显示本帮助

注意：
  * payload 必须 < 64KiB（LogBlock::kCap），超过会被静默丢弃；
  * --verify 会为每个线程分配 messages 位的位图，默认规模约 160KB，可放心开；
  * 退出码 0 = 校验通过，1 = 校验失败或参数错误。
)");
}

// 支持 1K / 4M 之类的后缀，压测脚本里写起来方便。
[[nodiscard]] bool parse_size(std::string_view text, std::size_t &out) {
    if (text.empty()) {
        return false;
    }
    std::size_t multiplier = 1;
    const char last = text.back();
    if (last == 'k' || last == 'K') {
        multiplier = 1024;
        text.remove_suffix(1);
    } else if (last == 'm' || last == 'M') {
        multiplier = 1024 * 1024;
        text.remove_suffix(1);
    } else if (last == 'g' || last == 'G') {
        multiplier = 1024ull * 1024 * 1024;
        text.remove_suffix(1);
    }
    std::size_t value = 0;
    const auto res = std::from_chars(text.data(), text.data() + text.size(), value);
    if (res.ec != std::errc{} || res.ptr != text.data() + text.size()) {
        return false;
    }
    if (value > std::numeric_limits<std::size_t>::max() / multiplier) {
        return false;
    }
    out = value * multiplier;
    return true;
}

[[nodiscard]] bool parse_config(int argc, char **argv, Config &cfg) {
    const auto need_value = [&](int &i, std::string_view flag) {
        if (i + 1 >= argc) {
            std::println(stderr, "[stress] {} 缺少参数", flag);
            return false;
        }
        ++i;
        return true;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage();
            std::exit(0);
        } else if (arg == "--threads") {
            if (!need_value(i, arg)) {
                return false;
            }
            cfg.threads = static_cast<std::uint32_t>(std::strtoul(argv[i], nullptr, 10));
        } else if (arg == "--messages") {
            if (!need_value(i, arg)) {
                return false;
            }
            if (!parse_size(argv[i], cfg.messages)) {
                return false;
            }
        } else if (arg == "--payload") {
            if (!need_value(i, arg)) {
                return false;
            }
            if (!parse_size(argv[i], cfg.payload)) {
                return false;
            }
        } else if (arg == "--roll-size") {
            if (!need_value(i, arg)) {
                return false;
            }
            if (!parse_size(argv[i], cfg.roll_size)) {
                return false;
            }
        } else if (arg == "--flush-interval") {
            if (!need_value(i, arg)) {
                return false;
            }
            cfg.flush_interval =
                static_cast<std::uint32_t>(std::strtoul(argv[i], nullptr, 10));
        } else if (arg == "--check-per-count") {
            if (!need_value(i, arg)) {
                return false;
            }
            if (!parse_size(argv[i], cfg.check_per_count)) {
                return false;
            }
        } else if (arg == "--warmup") {
            if (!need_value(i, arg)) {
                return false;
            }
            if (!parse_size(argv[i], cfg.warmup)) {
                return false;
            }
        } else if (arg == "--log-dir") {
            if (!need_value(i, arg)) {
                return false;
            }
            cfg.log_dir = argv[i];
        } else if (arg == "--skew-heavy") {
            cfg.skew_heavy = true;
        } else if (arg == "--verify") {
            cfg.verify = true;
        } else if (arg == "--keep-logs") {
            cfg.keep_logs = true;
        } else if (arg == "--json") {
            cfg.json = true;
        } else {
            std::println(stderr, "[stress] 未知参数: {}", arg);
            return false;
        }
    }

    if (cfg.threads == 0 || cfg.messages == 0) {
        std::println(stderr, "[stress] --threads/--messages 必须为正");
        return false;
    }
    if (cfg.payload >= LogBlock::kCap) {
        std::println(stderr,
                     "[stress] --payload 必须 < {}（超过会被 LogBlock 静默丢弃，"
                     "会把缺陷统计成丢日志）",
                     LogBlock::kCap);
        return false;
    }
    if (cfg.roll_size < 1024) {
        std::println(stderr, "[stress] --roll-size 太小（< 1KiB），会在每次 "
                             "append 后滚动");
        return false;
    }
    if (cfg.log_dir.empty()) {
        // 不需要 pid：时间戳已经足够区分并发运行，而且这里是<chrono>内的 API，
        // 不依赖 POSIX 头。
        cfg.log_dir = fs::temp_directory_path() /
                      std::format("elog-stress-{}",
                                  Clock::now().time_since_epoch().count());
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    Config cfg;
    if (!parse_config(argc, argv, cfg)) {
        return 1;
    }

    std::error_code ec;
    fs::create_directories(cfg.log_dir, ec);
    if (ec) {
        // FileAppender 不会自动建目录，目录不存在只会降级为"不写文件"，
        // 那样压测结果毫无意义，所以这里直接失败。
        std::println(stderr, "[stress] 无法创建日志目录 {}: {}",
                     cfg.log_dir.string(),
                     ec.message());
        return 1;
    }

    const std::size_t per_thread_base = cfg.messages;
    // 负载倾斜时总写入量保持不变：每 4 个线程里 1 个写 4 份，另外 3 个各写 1 份。
    const std::size_t skew_units = cfg.skew_heavy
                                       ? (cfg.threads / 4) * 7 + (cfg.threads % 4)
                                       : cfg.threads;
    std::vector<std::size_t> quota(cfg.threads, per_thread_base);
    if (cfg.skew_heavy && cfg.threads >= 4) {
        const std::size_t base = per_thread_base * cfg.threads / skew_units;
        for (std::uint32_t t = 0; t < cfg.threads; ++t) {
            quota[t] = (t % 4 == 0) ? base * 4 : base;
        }
    }

    std::uint64_t expected_total = 0;
    for (const std::size_t q : quota) {
        expected_total += q;
    }
    const std::size_t approx_msg_bytes = cfg.payload + 24;

    std::println("[stress] 线程 {} × 基准 {} 条（合计 {} 条{}），payload {}B（约 {} MiB 落盘）",
                 cfg.threads,
                 per_thread_base,
                 expected_total,
                 cfg.skew_heavy ? "，负载倾斜 4:1" : "",
                 cfg.payload,
                 (expected_total * approx_msg_bytes) / (1024 * 1024));

    AsyncLogger logger(cfg.log_dir.string(),
                       "stress-",
                       cfg.roll_size,
                       std::chrono::seconds(cfg.flush_interval),
                       cfg.check_per_count);

    // 预热：不计时，让 thread_local 的 ProducerCtx、块池和文件都先热起来。
    for (std::size_t i = 0; i < cfg.warmup; ++i) {
        logger.append_message(make_message(kWarmupTid, i, 16));
    }

    // 起跑线：所有生产者必须等主线程发令后才开始写，保证计时窗口内
    // 线程是真正同时在压的（否则先起的线程独占计数器，吞吐会虚高）。
    std::mutex gate_mtx;
    std::condition_variable gate_cv;
    bool go = false;

    std::vector<std::thread> producers;
    producers.reserve(cfg.threads);
    for (std::uint32_t t = 0; t < cfg.threads; ++t) {
        producers.emplace_back([&, t] {
            const std::size_t mine = quota[t];
            {
                std::unique_lock lock(gate_mtx);
                gate_cv.wait(lock, [&] { return go; });
            }
            for (std::size_t i = 0; i < mine; ++i) {
                logger.append_message(make_message(t, i, cfg.payload));
            }
        });
    }

    {
        std::lock_guard lock(gate_mtx);
        go = true;
    }
    gate_cv.notify_all();

    const auto begin = Clock::now();
    for (auto &producer : producers) {
        producer.join();
    }
    const auto write_end = Clock::now();

    // join 之后才排空：若在生产者还在写的时候 wait_for_done()，
    // stop_accepting() 会让之后的 append_message 静默丢弃，统计出假丢失。
    const auto drain_begin = Clock::now();
    logger.wait_for_done();
    const auto drain_end = Clock::now();

    const auto wall = std::chrono::duration<double>(write_end - begin).count();
    const auto drain = std::chrono::duration<double>(drain_end - drain_begin).count();

    const std::uint64_t bytes = bytes_of(cfg.log_dir);
    std::error_code dir_ec;
    std::size_t file_count = 0;
    for (const auto &entry : fs::directory_iterator(cfg.log_dir, dir_ec)) {
        if (entry.is_regular_file(dir_ec) && entry.path().extension() == ".log") {
            ++file_count;
        }
    }

    const double mps = wall > 0 ? static_cast<double>(expected_total) / wall : 0.0;
    // 单位必须分清：MB/s = 1e6，MiB/s = 2^20。除以 2^20 却标成 MB/s 会系统性
    // 少报 4.86%（第一版就是这么错的），跨机器比数时足够误导人。
    const double mibps =
        wall > 0 ? static_cast<double>(bytes) / wall / (1024.0 * 1024.0) : 0.0;
    const double avg_msg = expected_total > 0
                               ? static_cast<double>(bytes) /
                                     static_cast<double>(expected_total)
                               : 0.0;

    std::optional<VerifyResult> vr;
    if (cfg.verify) {
        vr = verify_dump(cfg.log_dir, cfg.threads, quota);
    }

    std::println("[stress] 写入窗口 {:.3f}s，排空 {:.3f}s，吞吐 {:.0f} 条/s（{:.2f} GiB/s = {:.1f} MiB/s）",
                 wall,
                 drain,
                 mps,
                 mibps / 1024.0,
                 mibps);
    std::println("[stress] 落盘 {} 文件 / {} 字节，均摊 {:.1f} B/条",
                 file_count,
                 bytes,
                 avg_msg);

    int exit_code = 0;
    if (vr) {
        // 防御一下：万一解析出问题导致行数少于预热行数，别让减法下溢
        const std::size_t formal_lines =
            vr->lines > vr->warmup ? vr->lines - vr->warmup : 0;
        std::println("[stress] 校验: 正式行 {}（期望 {}），预热行 {}",
                     formal_lines,
                     expected_total,
                     vr->warmup);
        if (vr->ok() && formal_lines == expected_total) {
            std::println("[stress] 校验通过：不丢、不重、线程内不乱序");
        } else {
            // 主动要求了 --verify 却对不上，就不能返回 0：脚本按退出码判成败，
            // 返回 0 会让"校验失败"被静默吞掉。
            exit_code = 1;
            std::println(stderr,
                         "[stress] 校验失败: 解析失败 {} / 未知线程 {} / 序号越界 {} / "
                         "重复 {} / 缺失 {} / 顺序 {}",
                         vr->unparsable,
                         vr->unknown_tid,
                         vr->out_of_range,
                         vr->duplicates,
                         vr->missing,
                         vr->order_ok ? "OK" : "被破坏");
            if (!vr->detail.empty()) {
                std::println(stderr, "[stress] 详情: {}", vr->detail);
            }
        }
    } else {
        std::println("[stress] 未开启 --verify：落盘内容未校验，仅报告吞吐");
    }

    if (cfg.json) {
        // ok / verified 用整型 0/1/2：2 = 未校验，避免把 null 混进 JSON 里
        const int ok_code = cfg.verify ? (exit_code == 0 ? 1 : 0) : 2;
        std::println(
            R"({{"threads":{},"messages_per_thread":{},"payload":{},"total":{},"wall_s":{:.6f},"drain_s":{:.6f},"msg_per_s":{:.1f},"mib_per_s":{:.2f},"bytes":{},"files":{},"verified":{},"ok":{}}})",
            cfg.threads,
            per_thread_base,
            cfg.payload,
            expected_total,
            wall,
            drain,
            mps,
            mibps,
            bytes,
            file_count,
            cfg.verify,
            ok_code);
    }

    if (cfg.keep_logs) {
        std::println("[stress] 日志保留在 {}", cfg.log_dir.string());
    } else {
        fs::remove_all(cfg.log_dir, ec);
    }
    return exit_code;
}
