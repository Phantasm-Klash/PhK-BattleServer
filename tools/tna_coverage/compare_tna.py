#!/usr/bin/env python3
"""原版 TNA 基准 ↔ 生产对战端复刻：逐弹比对 + 弹幕类型覆盖矩阵。

输入（都在 gateway 上生成）：
    /tmp/tna-reference.jsonl   tna_reference_runner（bullet_funcs_reimpl.hpp）
    /tmp/tna-replica.jsonl     phk_tna_replica（生产 UpdateBulletMotion）

源码（用于判定「是否实现」）：
    docs/apk-reverse-notes/patterns/bullet_funcs_reimpl.hpp
    PhK-BattleServer/src/danmaku/bullet_motion.cpp

用法：
    python3 compare_tna.py \
        --reference /tmp/tna-reference.jsonl \
        --replica   /tmp/tna-replica.jsonl \
        --reimpl    /path/to/bullet_funcs_reimpl.hpp \
        --prod      /path/to/bullet_motion.cpp \
        --out-dir   /tmp/tna_coverage

产出：tna_coverage.csv（86 行逐 func 判定）+ tna_coverage.md（摘要矩阵）。
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import re
from collections import Counter, defaultdict
from pathlib import Path

FIELDS = ["x", "y", "vx", "vy", "angle", "speed",
          "hit_dis", "half_w", "half_h", "kind", "life"]


def load_jsonl(path: Path) -> dict[tuple[int, int], dict]:
    out: dict[tuple[int, int], dict] = {}
    for line in path.open(encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        row = json.loads(line)
        out[(int(row["func"]), int(row["tick"]))] = row
    return out


def source_sets(reimpl: Path, prod: Path):
    r = reimpl.read_text(encoding="utf-8")
    p = prod.read_text(encoding="utf-8")
    # 原版 dispatch 里真正接了 bulletFuncN 的编号
    ref_impl = {int(m.group(1)) for m in
                re.finditer(r"case\s+(\d+):\s*bulletFunc\d+\s*\(", r)}
    # 原版函数体里调用了 my_random 的编号（随机分支，数值不可能与生产 FNV 流一致）
    rng_funcs = set()
    for block in re.split(r"(?=inline void bulletFunc\d+\s*\()", r):
        m = re.match(r"inline void bulletFunc(\d+)", block)
        if m and "my_random" in block:
            rng_funcs.add(int(m.group(1)))
    # 生产里明确挂 FuncNoop 的编号
    prod_noop = {int(m.group(1)) for m in
                 re.finditer(r"case\s+(\d+):\s*FuncNoop", p)}
    return ref_impl, rng_funcs, prod_noop


def close(a: float, b: float) -> bool:
    return abs(a - b) <= 1e-3 + 1e-4 * max(abs(a), abs(b))


def compare(reference: dict, replica: dict, ref_impl, rng_funcs, prod_noop):
    keys = sorted(set(reference) & set(replica))
    by_func: dict[int, list] = defaultdict(list)
    for k in keys:
        by_func[k[0]].append(k)

    rows = []
    for func in range(86):
        pairs = by_func.get(func, [])
        first_bad = None
        bad_fields: set[str] = set()
        max_delta = 0.0
        for func_id, tick in sorted(pairs):
            a, b = reference[(func_id, tick)], replica[(func_id, tick)]
            for f in FIELDS:
                av, bv = float(a[f]), float(b[f])
                delta = abs(av - bv)
                max_delta = max(max_delta, delta)
                if not close(av, bv):
                    bad_fields.add(f)
                    if first_bad is None:
                        first_bad = tick

        ref_yes = func in ref_impl
        noop = func in prod_noop
        rng = func in rng_funcs
        if not ref_yes:
            verdict = "UNKNOWN_REF"
        elif noop:
            verdict = "MISSING"
        elif first_bad is None:
            verdict = "MATCH"
        elif rng:
            verdict = "RNG_DIFF"
        else:
            verdict = "MISMATCH"
        rows.append({
            "func": func,
            "ref_implemented": int(ref_yes),
            "prod_noop": int(noop),
            "rng_dependent": int(rng),
            "first_mismatch_tick": "" if first_bad is None else first_bad,
            "mismatched_fields": ",".join(sorted(bad_fields)),
            "max_abs_delta": round(max_delta, 6),
            "verdict": verdict,
        })
    return rows


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--reference", required=True)
    ap.add_argument("--replica", required=True)
    ap.add_argument("--reimpl", required=True)
    ap.add_argument("--prod", required=True)
    ap.add_argument("--out-dir", required=True)
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    reference = load_jsonl(Path(args.reference))
    replica = load_jsonl(Path(args.replica))
    ref_impl, rng_funcs, prod_noop = source_sets(Path(args.reimpl), Path(args.prod))
    rows = compare(reference, replica, ref_impl, rng_funcs, prod_noop)

    csv_path = out_dir / "tna_coverage.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)

    counts = Counter(r["verdict"] for r in rows)
    lines = [
        "# 原版 TNA 弹幕类型覆盖矩阵",
        "",
        f"参考 {len(reference)} 条 / 复刻 {len(replica)} 条逐弹状态；"
        f"原版已解码 {len(ref_impl)} 个 func，生产 Noop {len(prod_noop)} 个，"
        f"依赖随机 {len(rng_funcs)} 个。",
        "",
        "| 判定 | 数量 | 含义 |",
        "|---|---:|---|",
        "| MATCH | %d | 逐 tick 位置/方向/大小/类型完全一致 |" % counts["MATCH"],
        "| MISSING | %d | 原版有实现、生产挂 FuncNoop |" % counts["MISSING"],
        "| MISMATCH | %d | 两边都有实现但数值不一致（**待修**） |" % counts["MISMATCH"],
        "| RNG_DIFF | %d | 差异来自 std::rand() vs DeterministicRng，需单独对齐 |" % counts["RNG_DIFF"],
        "| UNKNOWN_REF | %d | 原版本身未解码，不要求生产实现 |" % counts["UNKNOWN_REF"],
        "",
        "## 需要处理的 func",
        "",
        "| func | 判定 | 首个不一致 tick | 不一致字段 | 最大绝对差 |",
        "|---:|---|---:|---|---:|",
    ]
    for r in rows:
        if r["verdict"] in ("MISSING", "MISMATCH", "RNG_DIFF"):
            lines.append("| %d | %s | %s | %s | %s |" %
                         (r["func"], r["verdict"],
                          "-" if r["first_mismatch_tick"] == "" else r["first_mismatch_tick"],
                          r["mismatched_fields"] or "-", r["max_abs_delta"]))
    lines += ["", "## 完全一致", "",
              ", ".join(str(r["func"]) for r in rows if r["verdict"] == "MATCH") or "(无)", ""]
    (out_dir / "tna_coverage.md").write_text("\n".join(lines), encoding="utf-8")
    print("reference", len(reference), "replica", len(replica))
    print("ref_impl", len(ref_impl), "prod_noop", len(prod_noop), "rng", len(rng_funcs))
    print(dict(counts))
    print("wrote", csv_path, out_dir / "tna_coverage.md")


if __name__ == "__main__":
    main()
