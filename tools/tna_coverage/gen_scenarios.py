#!/usr/bin/env python3
"""生成双端运动学对拍场景（服务端 C++ ↔ 客户端 TS 逐帧比对）。

场景文本格式由两端 runner 共同解析：
    scenario <index> <funcNo> <ticks> <seedLoHex> <seedHiHex> <pattern> <spawn>
    motion  <field> <value>            （每个字段一行）
    target my_ship|enemy <has> <x> <y>
    end

初值表与 `tna_emu.py: make_init(fn, profile)` / `apps/phk_tna_replica:
MakeInitial(func, profile)` **逐字段一致**——三档初值就是整帧基准那三档，
所以双端对拍覆盖的正是刚被 86/86 验证过的那批分支。

用法：
    python3 gen_scenarios.py --out scenarios.txt [--profile 0 1 2] [--ticks 90]
"""
from __future__ import annotations

import argparse

# 字段名用两端共用的 snake_case（TS 的 createBulletMotion 与 C++ 的
# ApplyMotionField 同名）。
BASE_FIELDS = {
    "x": 320.0, "y": 480.0, "vx": 0.0, "vy": 0.0,
    "angle": 0.0, "speed": 3.0, "accel": 0.0, "t": 0.0, "life": 4.0,
    "angle_spd": 0.0, "angle_acc": 0.0, "f56c": 0.0, "speed_cap": 0.0,
    "angle_init": -1.0, "speed_init": -1.0, "f598": 0.0, "prev_angle": 0.0,
    "spd_init2": -1.0, "xacc": 0.0, "xacc_cap": 0.0, "pf1": 1.0, "pf2": 4.0,
    "bullet_kind": 45.0, "rotate_flg": 0.0, "bullet_life": 0.0,
    "bullet_loop": 0.0, "delay": 0.0, "vanish_delay": 0.0,
    "p1": 0.0, "p2": 0.0, "hit_dis": 4.0, "hit_hantei": 1.0,
    "half_w": 4.0, "half_h": 4.0,
}

PROFILES = {
    0: {},  # 基准：屏幕中心 + -1 哨兵 + 零参数
    1: {  # 出屏 + 非零运动参数
        "x": 400.0, "y": 520.0, "angle": 45.0, "speed": 2.0,
        "hit_dis": 6.0, "half_w": 6.0, "half_h": 6.0,
        "pf1": 2.0, "pf2": 3.0,
        "angle_init": 30.0, "speed_init": 2.0, "spd_init2": 4.0,
        "angle_spd": 2.0, "angle_acc": 1.0, "f56c": 1.0,
        "speed_cap": 8.0, "f598": 0.5, "prev_angle": 10.0,
        "xacc": 0.25, "xacc_cap": 6.0, "bullet_life": 1.0,
    },
    2: {  # 负向边界 + 负参数
        "x": -10.0, "y": 30.0, "angle": 270.0, "speed": 1.5,
        "hit_dis": 5.0, "half_w": 5.0, "half_h": 5.0,
        "pf1": 0.5, "pf2": 2.0,
        "angle_init": -45.0, "speed_init": -3.0, "spd_init2": -2.0,
        "angle_spd": -1.0, "angle_acc": -0.5, "f56c": -1.0,
        "speed_cap": 2.0, "f598": -0.25, "prev_angle": 5.0,
        "xacc": -0.5, "xacc_cap": 1.0,
    },
}

# 这三个 func 用 p1=2/p2=8 的专属初值（淡入淡出档），其余 func 用各自的 p1/p2。
SPECIAL_P = {28: (2, 8), 36: (2, 8), 79: (2, 8)}
PROFILE_P = {0: (0, 0), 1: (2, 3), 2: (1, 5)}

MY_SHIP = (320.0, 600.0)
ENEMY = (320.0, 420.0)
# 种子拆成两半，**每半都必须 ≤ 2^32-1**：客户端 runner 用 parseInt(hex,16) 读
# 16 位十六进制，超过 2^53 会丢精度，RNG 材料随之与 C++ 不同（真实踩过）。
SEED_LO = 0x32808F4B
SEED_HI = 0x5D4DB415


def fmt(value: float) -> str:
    """C++ 侧用 `%.17g` 读回，TS 侧用 JS 最短往返；这里统一成精确十进制。"""
    if float(value).is_integer() and abs(value) < 1e15:
        return str(int(value))
    return repr(float(value))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--profile", type=int, nargs="+", default=[0, 1, 2])
    ap.add_argument("--ticks", type=int, default=90)
    ap.add_argument("--life", type=int, nargs="+", default=[4, 1, 0])
    ap.add_argument("--funcs", type=int, nargs="+", default=list(range(86)))
    args = ap.parse_args()

    lines = []
    index = 0
    for profile in args.profile:
        base = dict(BASE_FIELDS)
        base.update(PROFILES[profile])
        for fn in args.funcs:
            for life in args.life:
                fields = dict(base)
                fields["life"] = float(life)
                fields["bullet_func_no"] = float(fn)
                if fn in SPECIAL_P:
                    fields["p1"], fields["p2"] = float(SPECIAL_P[fn][0]), float(SPECIAL_P[fn][1])
                else:
                    fields["p1"], fields["p2"] = float(PROFILE_P[profile][0]), float(PROFILE_P[profile][1])
                lines.append(
                    "scenario %d %d %d %016x %016x 0 0" % (index, fn, args.ticks, SEED_LO, SEED_HI)
                )
                for key in sorted(fields):
                    lines.append("motion %s %s" % (key, fmt(fields[key])))
                lines.append("target my_ship 1 %s %s" % (fmt(MY_SHIP[0]), fmt(MY_SHIP[1])))
                lines.append("target enemy 1 %s %s" % (fmt(ENEMY[0]), fmt(ENEMY[1])))
                lines.append("end")
                index += 1
    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    print("wrote %s (%d scenarios)" % (args.out, index))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
