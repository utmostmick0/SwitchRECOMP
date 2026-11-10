#!/usr/bin/env bash
# fetch_log.sh — 从 PS5 拉取一次运行日志回 Mac 本地
#
# 用法（在 Mac 终端执行）:
#   ./tools/ps5/fetch_log.sh
#
# 行为:
#   1. 优先 Mac 直连 PS5 的 WFM(:8888) 拉取 (日志直接落 Mac 本地, 最干净)。
#   2. 若 Mac 直连失败 (网络不可达/PS5 离线), 自动改走 253 中转:
#      SSH 到 253 跑 ps5_read_log.py, 再 scp 整个 run 目录回 Mac。
#   3. 最终日志落在 <项目根>/runs/ps5log_<时间戳>/ 下:
#        data__switchrecomp__swr.log   (完整运行日志)
#        summary.txt                    (崩溃 census / verdict)
#        timeline.json                  (结构化时间线)
#
# 注意: 取的是 PS5 上 /data/switchrecomp/swr.log 的当前内容。
#       若游戏没点/没重启, 拉到的可能是上一次 run 的旧日志 —— 点完再跑即可。
set -uo pipefail

# ---- 配置 ----
PS5_HOST="192.168.88.33"
PS5_WFM_PORT="8888"
REMOTE="swizard@192.168.88.253"
CODE_DIR="/data/code/switchrecomp"               # 253 上的代码根
REMOTE_RUNS="/data/switchrecomp/runs"            # 253 上的 run 目录

# 项目根 (动态解析, 不依赖 $HOME 具体路径)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
LOCAL_RUNS="$PROJ_ROOT/runs"
RUN_ID="ps5log_$(date +%Y%m%d_%H%M%S)"

echo "==> [1/3] 拉取 PS5 日志 (run: $RUN_ID)"

LOCAL_RUN_DIR="$LOCAL_RUNS/$RUN_ID"
mkdir -p "$LOCAL_RUN_DIR"

# 优先: Mac 直连 PS5 WFM
READ_CMD=(python3 "$PROJ_ROOT/tools/ps5/ps5_read_log.py"
          --host "$PS5_HOST" --port "$PS5_WFM_PORT"
          --run-dir "$LOCAL_RUN_DIR")
if "${READ_CMD[@]}" >/dev/null 2>&1; then
  echo "==> 已从 Mac 直连 PS5 拉取完成"
else
  echo "    Mac 直连 PS5 失败, 改走 253 中转 ..."
  ssh "$REMOTE" "cd $CODE_DIR && python3 tools/ps5/ps5_read_log.py --host $PS5_HOST --port $PS5_WFM_PORT --run-dir $REMOTE_RUNS/$RUN_ID"
  scp -r "$REMOTE:$REMOTE_RUNS/$RUN_ID" "$LOCAL_RUNS/"
  echo "==> 已通过 253 中转并 scp 回 Mac"
fi

echo "==> [3/3] 完成。本地日志:"
echo "    $LOCAL_RUN_DIR/data__switchrecomp__swr.log"
echo "    $LOCAL_RUN_DIR/summary.txt"

# 顺手打印关键 verdict 行 (若本次有运行)
if [ -f "$LOCAL_RUN_DIR/summary.txt" ]; then
  echo "==> verdict 摘要:"
  grep -iE "FATAL|svcBreak|invalid memory|Crash|sentinel|nvn|window (OK|FAILED)|kernel service census" \
       "$LOCAL_RUN_DIR/summary.txt" | head -20 || true
fi
