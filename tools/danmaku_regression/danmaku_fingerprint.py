#!/usr/bin/env python3
"""phk_danmaku_trace 产出的 JSONL → 密度指纹（单个 JSON 对象）。

为什么单独一个文件：trace 一份 300 tick 约 50 MB，24 个用例就是 1.2 GB。
指纹在网关本地流式算完，只把几 KB 的 JSON 传回来，回归台就不必搬原始
JSONL。本脚本也可以在本地对任意 trace 单独跑：

    python tools/danmaku_fingerprint.py trace.jsonl

输出字段（顺序固定，便于人读与 diff）：

    bullets_total        整份 trace 里出现过的唯一 bullet_id 数（累计发射数，
                         不是末帧存活数）
    max_per_tick         单 tick「新生成子弹数」的最大值
    chains_records       chains[] 记录总条数
    ticks_with_bullets   bullets[] 非空的 tick 数
    trace_lines          实际写入的行数（对局可能提前 Finished）
    spell_phase_starts   `spell_phase_start` 事件总数（符卡阶段切换次数）
    spell_phases         进入过的符卡阶段编号集合（已排序）
    pattern_ids          出现过的 pattern_id 集合（已排序）
    spawn_series_sha256  逐 tick 新增子弹数序列的 sha256 —— 真正的回归锁

「新生成」的判定：bullet_id 在本 tick 之前从未出现过。引擎按 b0/b1/... 单调
发号，所以等价于「本 tick 出现的最大编号 - 之前见过的最大编号」，但这里用
集合做，避免依赖发号格式。
"""

from __future__ import annotations

import hashlib
import json
import sys


def fingerprint(path: str) -> dict:
    seen: set[str] = set()
    pattern_ids: set[str] = set()
    spell_phases: set[int] = set()
    spell_phase_starts = 0
    chains_records = 0
    ticks_with_bullets = 0
    trace_lines = 0
    max_per_tick = 0
    spawn_series: list[int] = []

    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            trace_lines += 1
            snap = json.loads(line)

            bullets = snap.get("bullets") or []
            if bullets:
                ticks_with_bullets += 1

            new_this_tick = 0
            for bullet in bullets:
                pattern_ids.add(bullet.get("pattern_id", ""))
                bullet_id = bullet.get("bullet_id", "")
                if bullet_id not in seen:
                    seen.add(bullet_id)
                    new_this_tick += 1
            spawn_series.append(new_this_tick)
            if new_this_tick > max_per_tick:
                max_per_tick = new_this_tick

            chains_records += len(snap.get("chains") or [])

            # events[] 是「本 tick 新产生的事件」增量，跨 tick 累加才是总数。
            for event in snap.get("events") or []:
                if event.get("type") == "spell_phase_start":
                    spell_phase_starts += 1
                    spell_phases.add(int(event.get("value", 0)))

    digest = hashlib.sha256("\n".join(str(n) for n in spawn_series).encode("ascii"))

    return {
        "bullets_total": len(seen),
        "max_per_tick": max_per_tick,
        "chains_records": chains_records,
        "ticks_with_bullets": ticks_with_bullets,
        "trace_lines": trace_lines,
        "spell_phase_starts": spell_phase_starts,
        "spell_phases": sorted(spell_phases),
        "pattern_ids": sorted(pattern_ids),
        "spawn_series_sha256": digest.hexdigest(),
    }


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        sys.stderr.write("usage: danmaku_fingerprint.py <trace.jsonl>\n")
        return 2
    result = fingerprint(argv[1])
    json.dump(result, sys.stdout, ensure_ascii=False, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
