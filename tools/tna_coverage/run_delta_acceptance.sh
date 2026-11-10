#!/usr/bin/env bash
# 差量 wire 端到端验收（规格 §10.4）：真实弹幕 trace 的「delta 重建 == 全量」。
#
#   服务端同参跑两份 trace：全量（含 bullets[]）与差量（--delta，省略 bullets[]），
#   客户端用 bullet_reconstructor 从事件流递推，逐 tick 与全量逐位比较。
#
# 结论（2026-11-11）：300 tick 逐位相等、state_hash 全等，
# 带宽 55,015,358 B → 1,220,396 B（省 97.8%）。
#
# 用法（gateway 上，PhK-BattleServer 仓内）：
#   bash tools/tna_coverage/run_delta_acceptance.sh
#   SK_DIR=/root/gotouhou/SpellKard bash tools/tna_coverage/run_delta_acceptance.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BUILD="${BUILD_DIR:-$REPO/build-linux}"
OUT="${OUT_DIR:-/tmp/delta_acceptance}"
SK_DIR="${SK_DIR:-/root/gotouhou/SpellKard}"
SEED="${SEED:-12345}"
TICKS="${TICKS:-300}"
mkdir -p "$OUT"

echo "=== 1/4 构建 trace 工具 ==="
cmake --build "$BUILD" --target phk_danmaku_trace -- -j4

echo "=== 2/4 生成全量与差量 trace（seed=$SEED ticks=$TICKS） ==="
"$BUILD/phk_danmaku_trace" --out "$OUT/full.jsonl" --seed "$SEED" --ticks "$TICKS"
"$BUILD/phk_danmaku_trace" --out "$OUT/delta.jsonl" --seed "$SEED" --ticks "$TICKS" --delta

echo "=== 3/4 编译客户端镜像 ==="
cd "$SK_DIR/laya"
if [ ! -f .test-build/src/core/net/bullet_reconstructor.js ] || [ "${FORCE_TSC:-0}" = "1" ]; then
    npx tsc -p tsconfig.test.json
fi

echo "=== 4/4 客户端重建并逐 tick 逐位比较 ==="
cd "$REPO"
node "$SK_DIR/laya/dev/delta_replay.mjs" \
    --full "$OUT/full.jsonl" --delta "$OUT/delta.jsonl" --seed "$SEED"
