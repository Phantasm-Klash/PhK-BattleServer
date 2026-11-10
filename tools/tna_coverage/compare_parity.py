#!/usr/bin/env python3
"""比对双端运动学输出（服务端 C++ motion-cpp.txt ↔ 客户端 TS motion-ts.txt）。

两边的 runner 都逐 tick 打印
    <scenario> <tick> <x> <y> <vx> <vy> <angle> <speed> <t> <life>
C++ 用 `%.17g`、TS 用 JS 最短往返，**文本不一定逐字节相同**，所以按数值比较：
整数列（scenario/tick/t/life）必须完全相等，浮点列用相对容差。

用法：
    python3 compare_parity.py motion-cpp.txt motion-ts.txt [--atol 1e-12] [--rtol 1e-12]
"""
from __future__ import annotations

import argparse
import sys

FIELDS = ("x", "y", "vx", "vy", "angle", "speed")
INT_FIELDS = ("t", "life")


def parse(path):
    rows = []
    with open(path, encoding="utf-8") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) != 10:
                raise SystemExit("%s:%d 期望 10 列，实得 %d" % (path, lineno, len(parts)))
            try:
                rows.append((int(parts[0]), int(parts[1]),
                             [float(p) for p in parts[2:8]],
                             int(float(parts[8])), int(float(parts[9]))))
            except ValueError as exc:
                raise SystemExit("%s:%d 解析失败: %s" % (path, lineno, exc))
    return rows


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("cpp")
    ap.add_argument("ts")
    ap.add_argument("--atol", type=float, default=1e-12)
    ap.add_argument("--rtol", type=float, default=1e-12)
    ap.add_argument("--max-report", type=int, default=10)
    args = ap.parse_args()

    cpp = parse(args.cpp)
    ts = parse(args.ts)
    if len(cpp) != len(ts):
        print("FAIL 行数不一致: cpp=%d ts=%d" % (len(cpp), len(ts)))
        return 1

    bad = []
    for i, (a, b) in enumerate(zip(cpp, ts)):
        if a[0] != b[0] or a[1] != b[1]:
            bad.append((i, "行标识不一致 cpp=%s/%s ts=%s/%s" % (a[0], a[1], b[0], b[1]), None))
            continue
        for j, name in enumerate(FIELDS):
            fa, fb = a[2][j], b[2][j]
            tol = args.atol + args.rtol * max(abs(fa), abs(fb))
            if not (abs(fa - fb) <= tol):
                bad.append((i, "scenario=%d tick=%d %s cpp=%.17g ts=%.17g Δ=%.3g" %
                            (a[0], a[1], name, fa, fb, fa - fb), None))
                break
        else:
            if a[3] != b[3] or a[4] != b[4]:
                bad.append((i, "scenario=%d tick=%d 整数列不一致 cpp(t=%d,life=%d) ts(t=%d,life=%d)" %
                            (a[0], a[1], a[3], a[4], b[3], b[4]), None))

    if bad:
        print("FAIL 双端运动学不一致：%d/%d 行不同" % (len(bad), len(cpp)))
        for row in bad[:args.max_report]:
            print("  %s" % row[1])
        if len(bad) > args.max_report:
            print("  ... 还有 %d 行" % (len(bad) - args.max_report))
        return 1
    print("PASS 双端运动学一致：%d 行逐字段相同（%d 个场景）" % (len(cpp), len(cpp) // 90 if len(cpp) % 90 == 0 else -1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
