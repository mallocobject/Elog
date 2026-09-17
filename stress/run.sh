#!/usr/bin/env bash
# elog 压测入口：极简 shell 包装，真正的编排在 run_stress.py。
#
#   stress/run.sh                       # 全部场景
#   stress/run.sh --list                # 看有哪些场景
#   stress/run.sh -s mt -s unbalanced -r 3 --csv /tmp/elog.csv
#   BIN=/path/to/elog_bench_stress stress/run.sh -s payload16k
#
# 可用环境变量：
#   BIN      压测二进制路径（默认交给脚本自动查找）
#   PYTHON   python 解释器（默认 python3）
#   EXTRA    透传给二进制的额外参数，如 EXTRA="--warmup 0"
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python_bin="${PYTHON:-python3}"

if ! command -v "$python_bin" >/dev/null 2>&1; then
    echo "[stress] 找不到 $python_bin，请设置 PYTHON 环境变量" >&2
    exit 2
fi

args=(--bin "${BIN:-}" "$@")
# --bin 传空串会让脚本以为用户显式指定了路径，这里不带它更干净
if [[ -z "${BIN:-}" ]]; then
    args=("$@")
fi

if [[ -n "${EXTRA:-}" ]]; then
    args+=(--extra "$EXTRA")
fi

exec "$python_bin" "$here/run_stress.py" "${args[@]}"
