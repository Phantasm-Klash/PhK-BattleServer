#!/usr/bin/env bash
# 原版 TNA 弹幕基准 ↔ 生产对战端：一键复刻测试 + 覆盖矩阵门禁。
#
# 用法（在 gateway 上，PhK-BattleServer 仓内）：
#   bash tools/tna_coverage/run.sh
#   BUILD_DIR=/path/to/build OUT_DIR=/tmp/out bash tools/tna_coverage/run.sh
#
# 前提：build-linux 已构建 phk_tna_replica（CMakeLists 里无条件注册）。
# 判定：任何 MISMATCH / MISSING / RNG_DIFF 都会以非零码退出。
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BUILD="${BUILD_DIR:-$REPO/build-linux}"
OUT="${OUT_DIR:-/tmp/tna_coverage}"
SEED="${SEED:-12345}"
TICKS="${TICKS:-90}"
mkdir -p "$OUT"

# 1) 原版参考：优先用 unicorn 直接执行 libMyGame_arm64.so 里的函数体（真值），
#    没有 unicorn 时回退到 bullet_funcs_reimpl.hpp 的汇编重实现。
if python3 -c 'import unicorn' 2>/dev/null; then
    python3 "$HERE/tna_emu.py" --all --ticks "$TICKS" \
        --out "$OUT/tna-reference.jsonl"
else
    g++ -std=c++17 -O2 -I "$HERE/reference" \
        -o "$OUT/tna_reference_runner" "$HERE/tna_reference_runner.cpp"
    "$OUT/tna_reference_runner" --out "$OUT/tna-reference.jsonl" \
        --ticks "$TICKS" --seed "$SEED"
fi

# 2) 生产对战端复刻（UpdateBulletMotion + 固定 DeterministicRng）
"$BUILD/phk_tna_replica" --out "$OUT/tna-replica.jsonl" \
    --ticks "$TICKS" --seed "$SEED"

# 3) 逐 (func,tick) 比对 → 覆盖矩阵
python3 "$HERE/compare_tna.py" \
    --reference "$OUT/tna-reference.jsonl" \
    --replica   "$OUT/tna-replica.jsonl" \
    --reimpl    "$HERE/reference/bullet_funcs_reimpl.hpp" \
    --prod      "$REPO/src/danmaku/bullet_motion.cpp" \
    --out-dir   "$OUT"

# 4) 门禁断言
python3 - "$OUT/tna_coverage.csv" <<'PY'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1], encoding="utf-8")))
bad = [r for r in rows if r["verdict"] in ("MISMATCH", "MISSING", "RNG_DIFF")]
matched = sum(1 for r in rows if r["verdict"] == "MATCH")
unknown = sum(1 for r in rows if r["verdict"] == "UNKNOWN_REF")
if bad:
    for r in bad:
        print("FAIL func=%s verdict=%s fields=%s" %
              (r["func"], r["verdict"], r["mismatched_fields"]))
    sys.exit(1)
print("PASS: %d funcs逐弹一致, %d funcs原版未解码(不要求实现)" % (matched, unknown))
PY
