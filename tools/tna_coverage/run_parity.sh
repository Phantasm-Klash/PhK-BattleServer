#!/usr/bin/env bash
# 双端运动学对拍：服务端 C++ ↔ 客户端 TS，逐 tick 比对全部运动学输出。
#
# 与 run.sh 的关系：
#   run.sh        服务端生产 vs **原版**（unicorn 整帧执行）→ 逐弹真值门禁
#   run_parity.sh 客户端生产 vs **服务端生产** → 双端一致性门禁
# 两者都过了，才说明「原版 → 服务端 → 客户端」整条链没有口径漂移。
#
# 用法（gateway 上，PhK-BattleServer 仓内）：
#   bash tools/tna_coverage/run_parity.sh
#   SK_DIR=/root/gotouhou/SpellKard PROFILES="0" bash tools/tna_coverage/run_parity.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BUILD="${BUILD_DIR:-$REPO/build-linux}"
OUT="${OUT_DIR:-/tmp/tna_parity}"
SK_DIR="${SK_DIR:-/root/gotouhou/SpellKard}"
PROFILES="${PROFILES:-0 1 2}"
TICKS="${TICKS:-90}"
LIVES="${LIVES:-4 1 0}"
mkdir -p "$OUT"

echo "=== 1/4 生成场景（profiles=$PROFILES lives=$LIVES ticks=$TICKS） ==="
python3 "$HERE/gen_scenarios.py" --out "$OUT/scenarios.txt" \
    --profile $PROFILES --ticks "$TICKS" --life $LIVES

echo "=== 2/4 服务端 C++ 运行 ==="
"$BUILD/phk_motion_diff" "$OUT/scenarios.txt" "$OUT/motion-cpp.txt"

echo "=== 3/4 客户端 TS 运行（$SK_DIR） ==="
if [ ! -d "$SK_DIR/laya" ]; then
    echo "FAIL 找不到客户端仓 $SK_DIR/laya（可用 SK_DIR=... 指定）" >&2
    exit 1
fi
cd "$SK_DIR/laya"
if [ ! -f .test-build/src/core/sim/danmaku/bullet_motion.js ] || [ "${FORCE_TSC:-0}" = "1" ]; then
    npx tsc -p tsconfig.test.json
fi
node dev/motion_diff.mjs "$OUT/scenarios.txt" "$OUT/motion-ts.txt"

echo "=== 4/4 逐 tick 比对 ==="
cd "$REPO"
python3 "$HERE/compare_parity.py" "$OUT/motion-cpp.txt" "$OUT/motion-ts.txt"
