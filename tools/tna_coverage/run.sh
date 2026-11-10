#!/usr/bin/env bash
# 原版 TNA 弹幕基准 ↔ 生产对战端：一键复刻测试 + 覆盖矩阵门禁。
#
# 用法（在 gateway 上，PhK-BattleServer 仓内）：
#   bash tools/tna_coverage/run.sh
#   BUILD_DIR=/path/to/build OUT_DIR=/tmp/out bash tools/tna_coverage/run.sh
#   PROFILES="0 1" bash tools/tna_coverage/run.sh     # 只跑指定初值档
#
# 前提：build-linux 已构建 phk_tna_replica（CMakeLists 里无条件注册）。
# 判定：任何 MISMATCH / MISSING / RNG_DIFF 都会以非零码退出。
#
# 三档初值（与 tna_emu.py / phk_tna_replica 的 make_init 严格一致）：
#   0 = 基准（屏幕中心、零参数、-1 哨兵）
#   1 = 出屏 + 非零运动参数（边界分支 / 角加速度 / 速度上限）
#   2 = 负向边界 + 负参数（x<0 / y<40 / 镜像与减速分支）
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BUILD="${BUILD_DIR:-$REPO/build-linux}"
OUT="${OUT_DIR:-/tmp/tna_coverage}"
SEED="${SEED:-12345}"
TICKS="${TICKS:-90}"
# 三档初值全部纳入门禁（2026-11-10 起 p0/p1/p2 均 86/86）。
# 可用 `PROFILES=0` 只跑基准档做快速回归；差异历史见
# tools/tna_coverage/multiprofile-gaps.md。
PROFILES="${PROFILES:-0 1 2}"
mkdir -p "$OUT"

HAS_UNICORN=0
if python3 -c 'import unicorn' 2>/dev/null; then HAS_UNICORN=1; fi

for P in $PROFILES; do
    OUT_P="$OUT/p$P"
    mkdir -p "$OUT_P"
    echo "=== profile $P ==="

    # 1) 原版参考：unicorn 默认执行 EnemyBullet::update **完整一帧**
    #    （分派点 0x50b4e8 → ret），含内联实现、共用出口、速度合成与位置积分，
    #    86 个 funcNo 全覆盖。TNA_EMU_MODE=func 可切回「只执行独立 func 本体」。
    if [ "$HAS_UNICORN" = "1" ]; then
        python3 "$HERE/tna_emu.py" --all --ticks "$TICKS" \
            --mode "${TNA_EMU_MODE:-update}" --profile "$P" \
            --out "$OUT_P/tna-reference.jsonl"
    else
        if [ "$P" != "0" ]; then
            echo "FAIL: profile $P 需要 unicorn（fallback 只有基准初值）" >&2
            exit 1
        fi
        g++ -std=c++17 -O2 -I "$HERE/reference" \
            -o "$OUT/tna_reference_runner" "$HERE/tna_reference_runner.cpp"
        "$OUT/tna_reference_runner" --out "$OUT_P/tna-reference.jsonl" \
            --ticks "$TICKS" --seed "$SEED"
    fi

    # 2) 生产对战端复刻（UpdateBulletMotion + 固定 DeterministicRng）
    "$BUILD/phk_tna_replica" --out "$OUT_P/tna-replica.jsonl" \
        --ticks "$TICKS" --seed "$SEED" --profile "$P"

    # 3) 逐 (func,life0,tick) 比对 → 覆盖矩阵
    python3 "$HERE/compare_tna.py" \
        --reference "$OUT_P/tna-reference.jsonl" \
        --replica   "$OUT_P/tna-replica.jsonl" \
        --reimpl    "$HERE/reference/bullet_funcs_reimpl.hpp" \
        --prod      "$REPO/src/danmaku/bullet_motion.cpp" \
        --out-dir   "$OUT_P"

    # 4) 门禁断言
    python3 - "$OUT_P/tna_coverage.csv" "$P" <<'PY'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1], encoding="utf-8")))
profile = sys.argv[2]
bad = [r for r in rows if r["verdict"] in ("MISMATCH", "MISSING", "RNG_DIFF")]
matched = sum(1 for r in rows if r["verdict"] == "MATCH")
unknown = sum(1 for r in rows if r["verdict"] == "UNKNOWN_REF")
if bad:
    for r in bad:
        print("FAIL p%s func=%s verdict=%s fields=%s" %
              (profile, r["func"], r["verdict"], r["mismatched_fields"]))
    sys.exit(1)
print("PASS p%s: %d funcs逐弹一致, %d funcs无参考数据" %
      (profile, matched, unknown))
PY
done

# 兼容旧调用方：基准档的结果留在 $OUT 根目录。
if [ -f "$OUT/p0/tna_coverage.csv" ]; then
    cp "$OUT/p0/tna_coverage.csv"      "$OUT/tna_coverage.csv"
    cp "$OUT/p0/tna_coverage.md"       "$OUT/tna_coverage.md"
    cp "$OUT/p0/tna-reference.jsonl"   "$OUT/tna-reference.jsonl"
    cp "$OUT/p0/tna-replica.jsonl"     "$OUT/tna-replica.jsonl"
fi
