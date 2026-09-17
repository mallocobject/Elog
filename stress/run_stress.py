#!/usr/bin/env python3
"""elog 压测编排脚本：按场景矩阵反复运行 elog_bench_stress，汇总吞吐与校验结果。

只调用二进制，不做任何编译。默认每个场景跑一次，可用 -r/--repeat 增加重复次数
以便观察抖动（例如 --repeat 3 会把同一场景跑 3 遍并给出 min/avg/max）。

用法：
    python3 stress/run_stress.py --list
    python3 stress/run_stress.py                      # 跑全部场景
    python3 stress/run_stress.py -s unbalanced -s mt -r 3 --csv result.csv
    python3 stress/run_stress.py --keep-logs -s logsize
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path


# ---------------------------------------------------------------- 场景定义
@dataclass
class Scenario:
    name: str
    desc: str
    args: list[str] = field(default_factory=list)


# 覆盖点：并发度 / 单线程写入量 / 消息大小 / 滚动频率 / 刷新节奏。
# 所有场景都带 --verify：压测的价值就在于"跑得快但没丢"。
SCENARIOS: list[Scenario] = [
    Scenario("mt", "8 线程 × 20 万条，基准并发吞吐",
             ["--threads", "8", "--messages", "200000", "--payload", "128",
              "--verify"]),
    Scenario("mt32", "32 线程 × 5 万条，高并发竞争",
             ["--threads", "32", "--messages", "50000", "--payload", "128",
              "--verify"]),
    Scenario("unbalanced", "1 个重载线程 + 7 个轻载线程（4:1 负载倾斜）",
             ["--threads", "8", "--messages", "80000", "--payload", "256",
              "--skew-heavy", "--verify"]),
    Scenario("payload1k", "1 KiB 消息，看在途数据量变大后的表现",
             ["--threads", "8", "--messages", "50000", "--payload", "1024",
              "--verify"]),
    Scenario("payload16k", "16 KiB 消息，逼近 LogBlock 单块容量",
             ["--threads", "4", "--messages", "8000", "--payload", "16384",
              "--verify"]),
    Scenario("logsize", "32 MiB 滚动阈值，制造大量滚动与文件切换",
             ["--threads", "8", "--messages", "50000", "--payload", "512",
              "--roll-size", "32M", "--verify"]),
    Scenario("flush1s", "1 秒刷新间隔（默认），观察后台线程节奏影响",
             ["--threads", "8", "--messages", "50000", "--payload", "128",
              "--flush-interval", "1", "--verify"]),
    Scenario("oversize", "单条 64512B，贴近 64 KiB 单块上限（再大驱动会拒绝）",
             ["--threads", "2", "--messages", "2000", "--payload", "64512",
              "--verify"]),
]


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def default_binary() -> str | None:
    root = repo_root()
    for rel in ("build/elog_bench_stress",
                "build/stress/elog_bench_stress",
                "build-verify/elog_bench_stress",
                "build-verify/stress/elog_bench_stress"):
        if (root / rel).is_file():
            return str(root / rel)
    found = shutil.which("elog_bench_stress")
    return found


def parse_json_line(stdout: str) -> dict | None:
    """取最后一行能解析成 JSON 的输出。"""
    for line in reversed(stdout.splitlines()):
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            return json.loads(line)
        except json.JSONDecodeError:
            continue
    return None


def ok_label(code) -> str:
    """二进制里的 ok 字段：1=校验通过，0=校验失败，2=未校验。"""
    if code == 1:
        return "通过"
    if code == 0:
        return "失败"
    if code == 2:
        return "未校验"
    return "-"


def run_once(binary: str, scenario: Scenario, extra: list[str],
             keep_logs: bool, timeout: float,
             log_dir: str | None = None) -> dict:
    cmd = [binary, "--json", *scenario.args, *extra]
    if keep_logs:
        cmd.append("--keep-logs")
    if log_dir:
        cmd += ["--log-dir", log_dir]

    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    metrics = parse_json_line(proc.stdout)
    return {
        "returncode": proc.returncode,
        "metrics": metrics,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
    }


def fmt(value: float | None, digits: int = 1) -> str:
    if value is None:
        return "-"
    return f"{value:,.{digits}f}"


def summarize(rows: list[dict]) -> None:
    """打印 Markdown 汇总表；异常场景加 ❌ 标记。"""
    header = ("| 场景 | 线程 | 总量(条) | MiB/s | 条/s | 排空(s) | 校验 | 结论 |")
    sep = "|---|---|---|---|---|---|---|---|"
    print()
    print(header)
    print(sep)
    for row in rows:
        m = row["metrics"] or {}
        ok = row["returncode"] == 0
        mark = "✅" if ok else "❌"
        note = row["note"]
        print(
            f"| {row['scenario']} "
            f"| {m.get('threads', '-')} "
            f"| {m.get('total', '-')} "
            f"| {fmt(m.get('mib_per_s'), 1)} "
            f"| {fmt(m.get('msg_per_s'), 0)} "
            f"| {fmt(m.get('drain_s'), 3)} "
            f"| {ok_label(m.get('ok'))} "
            f"| {mark} {note} |"
        )
    print()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="elog 压测编排：跑场景矩阵、汇总吞吐、检查不丢日志",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bin", default=None,
                        help="elog_bench_stress 路径（默认自动在 build/ 下查找）")
    parser.add_argument("-s", "--scenario", action="append", default=None,
                        help="只跑指定场景，可重复；--list 查看全部")
    parser.add_argument("-r", "--repeat", type=int, default=1,
                        help="每个场景重复次数（默认 1）")
    parser.add_argument("--csv", default=None, help="把每次运行的结果写入 CSV")
    parser.add_argument("--log-root", default=None,
                        help="日志根目录（默认用系统临时目录）；"
                             "写在 tmpfs 上会占用内存，大场景建议指向真实磁盘")
    parser.add_argument("--keep-logs", action="store_true",
                        help="保留日志目录（默认交由二进制自行删除）")
    parser.add_argument("--timeout", type=float, default=1800.0,
                        help="单个场景的超时秒数（默认 1800）")
    parser.add_argument("--extra", default=None,
                        help="透传给二进制的额外参数，例如 --extra='--warmup 0'")
    parser.add_argument("--list", action="store_true", help="列出场景后退出")
    args = parser.parse_args()

    if args.list:
        for sc in SCENARIOS:
            print(f"{sc.name:12s} {sc.desc}")
        return 0

    chosen = SCENARIOS
    if args.scenario:
        wanted = set(args.scenario)
        chosen = [sc for sc in SCENARIOS if sc.name in wanted]
        unknown = wanted - {sc.name for sc in SCENARIOS}
        if unknown:
            print(f"[stress] 未知场景: {', '.join(sorted(unknown))}", file=sys.stderr)
            return 2

    binary = args.bin or default_binary()
    if not binary or not os.path.isfile(binary):
        print("[stress] 找不到 elog_bench_stress。先构建：\n"
              "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "
              "-DCMAKE_CXX_COMPILER=g++-14\n"
              "  cmake --build build\n"
              "或用 --bin 指定路径。", file=sys.stderr)
        return 2

    extra = args.extra.split() if args.extra else []
    print(f"[stress] 二进制: {binary}")
    print(f"[stress] 场景数: {len(chosen)}，每场景重复 {args.repeat} 次"
          + (f"，额外参数: {' '.join(extra)}" if extra else ""))

    rows: list[dict] = []
    failures = 0
    run_stamp = time.strftime("%Y%m%d-%H%M%S")
    if args.log_root:
        Path(args.log_root).mkdir(parents=True, exist_ok=True)
        free = shutil.disk_usage(args.log_root).free
        print(f"[stress] 日志根目录: {args.log_root}（可用 {free / (1024 ** 3):.1f} GiB）")

    for sc in chosen:
        best: dict | None = None
        for rep in range(args.repeat):
            label = sc.name if args.repeat == 1 else f"{sc.name}#{rep + 1}"
            log_dir = (str(Path(args.log_root) / f"{run_stamp}-{sc.name}-{rep + 1}")
                       if args.log_root else None)
            try:
                out = run_once(binary, sc, extra, args.keep_logs, args.timeout,
                               log_dir)
            except subprocess.TimeoutExpired:
                print(f"[stress] {label}: 超时（>{args.timeout:.0f}s）", file=sys.stderr)
                rows.append({"scenario": label, "metrics": None,
                             "returncode": 124, "note": "超时"})
                failures += 1
                continue

            m = out["metrics"]
            note = sc.desc if rep == 0 else ""
            if out["returncode"] != 0:
                failures += 1
                # 有 JSON = 压测跑完了、是校验没过；没 JSON = 连跑都没跑起来
                note = "校验失败" if m else "运行失败"
            if m:
                print(f"[stress] {label}: {fmt(m.get('msg_per_s'), 0)} 条/s, "
                      f"{fmt(m.get('mib_per_s'), 1)} MiB/s, "
                      f"排空 {fmt(m.get('drain_s'), 3)}s, "
                      f"校验={ok_label(m.get('ok'))}")
            else:
                print(f"[stress] {label}: 未取到 JSON 结果，退出码 {out['returncode']}",
                      file=sys.stderr)
                if out["stderr"].strip():
                    print("  stderr: " + out["stderr"].strip().splitlines()[-1],
                          file=sys.stderr)

            rows.append({"scenario": label, "metrics": m,
                         "returncode": out["returncode"], "note": note})

            # 有失败优先展示失败；否则保留吞吐最高的那次
            if best is None or out["returncode"] != 0:
                best = rows[-1]
            elif best["returncode"] == 0:
                prev = (best["metrics"] or {}).get("msg_per_s") or 0.0
                cur = (m or {}).get("msg_per_s") or 0.0
                if cur > prev:
                    best = rows[-1]
        if best and args.repeat > 1:
            print(f"[stress] {sc.name}: 最好一次 "
                  f"{fmt((best['metrics'] or {}).get('msg_per_s'), 0)} 条/s")

    summarize(rows)

    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8") as fh:
            writer = csv.writer(fh)
            writer.writerow(["scenario", "threads", "messages_per_thread", "payload",
                             "total", "wall_s", "drain_s", "msg_per_s", "mib_per_s",
                             "bytes", "files", "verified", "ok", "returncode"])
            for row in rows:
                m = row["metrics"] or {}
                writer.writerow([
                    row["scenario"], m.get("threads"), m.get("messages_per_thread"),
                    m.get("payload"), m.get("total"), m.get("wall_s"),
                    m.get("drain_s"), m.get("msg_per_s"), m.get("mib_per_s"),
                    m.get("bytes"), m.get("files"), m.get("verified"),
                    m.get("ok"), row["returncode"],
                ])
        print(f"[stress] CSV 已写入 {args.csv}")

    if failures:
        print(f"[stress] {failures} 次运行未通过", file=sys.stderr)
        return 1
    print("[stress] 全部场景通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
