#!/usr/bin/env python3
"""TNA 1.40 EnemyBullet 弹幕函数 AArch64 模拟器。

思路：把 libMyGame_arm64.so 的 PT_LOAD 段按原 vaddr 映射进 unicorn，
构造一个假的 EnemyBullet 对象（字段按 bullet_funcs_disasm.md §2 布局），
vptr 指向合成的 232 槽虚表（每槽一个哨兵地址，命中即用 Python 复刻语义），
再把所有 PLT/外部调用按地址拦截。于是可以直接**执行原版 bulletFuncN 本体**，
逐 tick 导出真实状态，作为生产实现的对拍基准。

用法：
    python3 tna_emu.py --func 18 --ticks 90 --out /tmp/emu.jsonl
    python3 tna_emu.py --all --ticks 90 --out /tmp/emu.jsonl
"""
import argparse
import ctypes
import json
import math
import struct
import subprocess
import sys

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_PROT_ALL
from unicorn.arm64_const import (
    UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3,
    UC_ARM64_REG_X4, UC_ARM64_REG_X5, UC_ARM64_REG_X8, UC_ARM64_REG_X30,
    UC_ARM64_REG_SP, UC_ARM64_REG_PC, UC_ARM64_REG_S0, UC_ARM64_REG_S1,
)

SO = "/root/gotouhou/.rev/libMyGame_arm64.so"

# ---- 假对象 / 合成表布局 ----
OBJ = 0x30000000          # EnemyBullet 实例
OBJ_SIZE = 0x800
LAYER = 0x30001000        # 假 GameLayer
LAYER_VT = 0x30002000     # GameLayer 合成虚表
VT = 0x31000000           # EnemyBullet 合成虚表（232 槽）
SENT = 0x32000000         # 槽哨兵区（每槽 16 字节）
SCRATCH = 0x33000000      # Vec2/Size/Color3B 返回缓冲
EXIT = 0x34000000         # 返回哨兵
STACK = 0x35000000
STACK_SIZE = 0x20000

# ---- EnemyBullet 字段偏移（bullet_funcs_disasm.md §2）----
F = {
    "rotateFlg": 0x530, "rotateAngle": 0x534, "bulletType": 0x538, "blend": 0x53c,
    "finished": 0x540, "grazeFlg": 0x541, "hitHantei": 0x548, "hitDis": 0x54c,
    "halfW": 0x554, "halfH": 0x558, "angle": 0x560, "angleSpd": 0x564,
    "angleAcc": 0x568, "f56c": 0x56c, "speed": 0x570, "spdAcc": 0x574,
    "speedCap": 0x578, "vx": 0x57c, "vy": 0x580, "bulletLife": 0x588,
    "bulletLoop": 0x58c, "angleInit": 0x590, "speedInit": 0x594, "f598": 0x598,
    "prevAngle": 0x59c, "spdInit2": 0x5a0, "xacc": 0x5a4, "xaccCap": 0x5a8,
    "delay": 0x5ac, "vanishDelay": 0x5b0, "funcNo": 0x5b4, "life": 0x5b8,
    "p1": 0x5bc, "p2": 0x5c0, "pf1": 0x5c4, "pf2": 0x5c8,
    "t1": 0x5dc, "f5e8": 0x5e8,
}

# 需要额外记录的“虚拟字段”
EXTRA = {"opacity": 255, "scale": 1.0, "x": 320.0, "y": 480.0}

# 虚表槽（已用 .rela 解出）
SLOT = {
    0x80: "setScale", 0x98: "setPosition", 0xb0: "getPosition",
    0x168: "getContentSize",
    0x490: "setOpacity", 0x4c0: "setColor", 0x6d0: "getHitDis", 0x6d8: "setHitDis",
}
SLOT_IDX = {v: k // 8 for k, v in SLOT.items()}


def load_segments(path):
    out = subprocess.run(["aarch64-linux-gnu-readelf", "-lW", path],
                         capture_output=True, text=True).stdout
    segs = []
    for ln in out.splitlines():
        p = ln.split()
        if len(p) >= 6 and p[0] == "LOAD":
            off, va, _, fsz = int(p[1], 16), int(p[2], 16), p[3], int(p[4], 16)
            msz = int(p[5], 16)
            segs.append((va, off, fsz, msz))
    return segs


class Emu:
    def __init__(self, so, hooks):
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        data = open(so, "rb").read()
        self.uc.mem_map(0x0, 0x1500000, UC_PROT_ALL)
        for va, off, fsz, msz in load_segments(so):
            base = va & ~0xFFF
            end = (va + msz + 0xFFF) & ~0xFFF
            try:
                self.uc.mem_write(va, data[off:off + min(fsz, msz)])
            except Exception:
                pass
            _ = base, end
        for addr, size in ((OBJ, OBJ_SIZE), (LAYER, 0x1000), (LAYER_VT, 0x1000),
                           (VT, 0x800), (SENT, 0x2000), (SCRATCH, 0x2000),
                           (0x34000000, 0x1000), (STACK, STACK_SIZE)):
            self.uc.mem_map(addr, size, UC_PROT_ALL)
        # 合成虚表：每槽指向唯一哨兵
        for i in range(232):
            self.uc.mem_write(VT + i * 8, struct.pack("<Q", SENT + i * 0x10))
        for i in range(232):
            self.uc.mem_write(LAYER_VT + i * 8, struct.pack("<Q", SENT + 0x1000 + i * 0x10))
        self.uc.mem_write(LAYER, struct.pack("<Q", LAYER_VT))
        self.uc.reg_write(UC_ARM64_REG_SP, STACK + STACK_SIZE - 0x100)
        self.layer_slots = {}
        self.hooks = hooks
        self.uc.hook_add(UC_HOOK_CODE, self._on_code)
        self.calls = []

    # --- 字段读写 ---
    def rd_f(self, off):
        return struct.unpack("<f", self.uc.mem_read(OBJ + off, 4))[0]

    def wr_f(self, off, val):
        self.uc.mem_write(OBJ + off, struct.pack("<f", val))

    def rd_i(self, off):
        return struct.unpack("<i", self.uc.mem_read(OBJ + off, 4))[0]

    def wr_i(self, off, val):
        self.uc.mem_write(OBJ + off, struct.pack("<i", int(val)))

    def _on_code(self, uc, addr, size, _u):
        if addr == EXIT:
            uc.emu_stop()
            return
        if addr == 0:
            # GOT 未重定位的 PLT 会 br 到 0（0x00000000 = udf）；按“返回 0”兜底
            uc.reg_write(UC_ARM64_REG_X0, 0)
            set_s(uc, 0.0)
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_X30))
            return
        if 0x32000000 <= addr < SENT + 0x2000:
            self._sentinel(uc, addr)
            return
        h = self.hooks.get(addr)
        if h is not None:
            h(self, uc)
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_X30))
            return

    def _sentinel(self, uc, addr):
        if addr < SENT + 0x1000:
            slot = (addr - SENT) // 0x10
            name = SLOT.get(slot * 8)
        else:
            slot = (addr - SENT - 0x1000) // 0x10
            name = "layer:%d" % slot
            self.layer_slots[slot] = self.layer_slots.get(slot, 0) + 1
            # 未知 GameLayer 虚函数：返回 0，x0 指向零缓冲（指针解引用不炸）
            uc.reg_write(UC_ARM64_REG_X0, SCRATCH + 0x180)
            set_s(uc, 0.0)
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_X30))
            return
        lr = uc.reg_read(UC_ARM64_REG_X30)
        if name == "getPosition":
            uc.mem_write(SCRATCH, struct.pack("<ff", EXTRA["x"], EXTRA["y"]))
            uc.reg_write(UC_ARM64_REG_X0, SCRATCH)
        elif name == "getContentSize":
            uc.mem_write(SCRATCH + 0x40, struct.pack("<ff",
                        self.rd_f(F["halfW"]) * 2, self.rd_f(F["halfH"]) * 2))
            uc.reg_write(UC_ARM64_REG_X0, SCRATCH + 0x40)
        elif name == "setOpacity":
            EXTRA["opacity"] = uc.reg_read(UC_ARM64_REG_X1) & 0xFF
        elif name == "setScale":
            EXTRA["scale"] = struct.unpack("<f", struct.pack("<I",
                                   uc.reg_read(UC_ARM64_REG_S0) & 0xFFFFFFFF))[0]
        elif name == "getHitDis":
            # 返回 float（s0），不是指针
            set_s(uc, self.rd_f(F["hitDis"]))
        elif name == "setHitDis":
            v = struct.unpack("<f", struct.pack("<I",
                              uc.reg_read(UC_ARM64_REG_S0) & 0xFFFFFFFF))[0]
            self.wr_f(F["hitDis"], v)
        elif name == "setPosition":
            # Sprite::setPosition(const Vec2&)：参数是指针（x1）
            ptr = uc.reg_read(UC_ARM64_REG_X1)
            try:
                px, py = struct.unpack("<ff", uc.mem_read(ptr, 8))
                EXTRA["x"], EXTRA["y"] = px, py
            except Exception:
                pass
        elif name == "setColor":
            EXTRA["color"] = uc.reg_read(UC_ARM64_REG_X1)
        uc.reg_write(UC_ARM64_REG_PC, lr)


def s_reg(uc):
    return struct.unpack("<f", struct.pack("<I", uc.reg_read(UC_ARM64_REG_S0) & 0xFFFFFFFF))[0]


def set_s(uc, val):
    uc.reg_write(UC_ARM64_REG_S0, struct.unpack("<I", struct.pack("<f", float(val)))[0])


# MyMathUtil::sin/cos 的真实实现（0x5ab0c0 / 0x5ab0dc，逐指令）：
#   s1 = 2π(float, @0xb23b04)  s2 = 360.0f(@0xace178)
#   s0 = deg * s1   (fmul, float)
#   s0 = s0 / s2    (fdiv, float)
#   b sinf@plt / cosf@plt
# 旧 hook 用 Python 的 double math.sin(math.radians(deg))，在 func36 的
# `fcvtzs w8,s0,#7`（×128 截断）边界上会差 1（192 vs 191）——那是**基准误差**，
# 不是生产缺陷。改用 libm 的单精度 sinf/cosf 后 79 函数整体无回归，func36 转绿。
_LIBM = ctypes.CDLL("libm.so.6")
_sinf = _LIBM.sinf
_sinf.argtypes = [ctypes.c_float]
_sinf.restype = ctypes.c_float
_cosf = _LIBM.cosf
_cosf.argtypes = [ctypes.c_float]
_cosf.restype = ctypes.c_float
_K2PI_F = ctypes.c_float(6.2831854820251465).value   # 0xb23b04


def _deg2rad_f(deg):
    prod = ctypes.c_float(deg * _K2PI_F).value
    return ctypes.c_float(prod / 360.0).value


def _sin_deg_f(_emu, uc):
    set_s(uc, _sinf(_deg2rad_f(s_reg(uc))))


def _cos_deg_f(_emu, uc):
    set_s(uc, _cosf(_deg2rad_f(s_reg(uc))))


# 必须与 apps/phk_tna_replica/main.cpp 的 kShipX/kShipY/kEnemyX/kEnemyY 一致，
# 否则所有带瞄准的函数都会因宿主坐标不同而假失败。
MY_SHIP = (320.0, 600.0)
ENEMY = (320.0, 420.0)


def ang_to(x, y, px, py):
    a = math.degrees(math.atan2(py - y, px - x))
    return a + 360.0 if a < 0 else a


_RND = {"n": 0}


def _arc4(emu_, uc):
    # 确定性替代 arc4random（MyMathUtil::random @0x5ab0f8 调它）
    _RND["n"] += 1
    x = (_RND["n"] * 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
    x ^= x >> 30
    x = (x * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
    x ^= x >> 27
    uc.reg_write(UC_ARM64_REG_X0, (x >> 32) & 0x7FFFFFFF)


HOOKS = {
    0x2e67c0: _arc4,                                                  # arc4random@plt
    # EnemyBullet::setBulletType(kEnemyBullet)@plt：只关心写入的类型值
    0x2e8d90: lambda e, uc: e.wr_i(F["bulletType"], uc.reg_read(UC_ARM64_REG_X1)),
    0x2ec170: lambda e, uc: uc.reg_write(UC_ARM64_REG_X0, LAYER),          # getGameLayer
    0x2fb7d0: lambda e, uc: set_s(uc, ang_to(                      # getAngleToMyShip
        struct.unpack("<f", struct.pack("<I", uc.reg_read(UC_ARM64_REG_S0) & 0xFFFFFFFF))[0],
        struct.unpack("<f", struct.pack("<I", uc.reg_read(UC_ARM64_REG_S1) & 0xFFFFFFFF))[0],
        MY_SHIP[0], MY_SHIP[1])),
    0x2ea940: lambda e, uc: set_s(uc, ang_to(                      # getAngleToEnemy
        struct.unpack("<f", struct.pack("<I", uc.reg_read(UC_ARM64_REG_S0) & 0xFFFFFFFF))[0],
        struct.unpack("<f", struct.pack("<I", uc.reg_read(UC_ARM64_REG_S1) & 0xFFFFFFFF))[0],
        ENEMY[0], ENEMY[1])),
    0x2ece70: _sin_deg_f,       # MyMathUtil::sin（度，单精度 sinf）
    0x2f01e0: _cos_deg_f,       # MyMathUtil::cos（度，单精度 cosf）
    0x2f8990: lambda e, uc: (uc.mem_write(SCRATCH + 0x100,
               struct.pack("<ff", MY_SHIP[0], MY_SHIP[1])),
               uc.reg_write(UC_ARM64_REG_X0, SCRATCH + 0x100)),   # getMyShipPos
    0x2eedf0: lambda e, uc: (uc.mem_write(SCRATCH + 0x120,
               struct.pack("<ff", ENEMY[0], ENEMY[1])),
               uc.reg_write(UC_ARM64_REG_X0, SCRATCH + 0x120)),   # getEnemyPos
}


def f32(v):
    return struct.unpack("<f", struct.pack("<f", float(v)))[0]


def snapshot(emu, func, tick):
    return {"func": func, "tick": tick, "life0": EXTRA.get("life0", 4),
            "x": EXTRA["x"], "y": EXTRA["y"],
            "vx": emu.rd_f(F["vx"]), "vy": emu.rd_f(F["vy"]),
            "angle": emu.rd_f(F["angle"]), "speed": emu.rd_f(F["speed"]),
            "hit_dis": emu.rd_f(F["hitDis"]), "half_w": emu.rd_f(F["halfW"]),
            "half_h": emu.rd_f(F["halfH"]), "kind": emu.rd_i(F["bulletType"]),
            "life": emu.rd_i(F["life"]), "t": float(emu.rd_i(F["t1"])),
            "opacity": EXTRA["opacity"], "scale": EXTRA["scale"]}


def run_func(emu, func_addr, size, ticks, init, seed=12345):
    uc = emu.uc
    # 初始化对象
    uc.mem_write(OBJ, struct.pack("<Q", VT))
    uc.mem_write(OBJ + 0x700, b"\x00" * 0x100)
    for k, v in init.items():
        if k in F:
            if k in ("rotateFlg", "bulletType", "bulletLife", "bulletLoop",
                     "delay", "vanishDelay", "funcNo", "life", "p1", "p2", "t1"):
                struct.pack("<f", 0)
                uc.mem_write(OBJ + F[k], struct.pack("<i", int(v)))
            else:
                uc.mem_write(OBJ + F[k], struct.pack("<f", float(v)))
    EXTRA.update({"opacity": 255, "scale": 1.0, "life0": init.get("life", 4),
                  "x": float(init.get("x", 320.0)), "y": float(init.get("y", 480.0))})
    rows = []
    for tick in range(ticks):
        # 复刻 UpdateBulletMotion 的前后处理，使唯一差异只剩 bulletFunc 本体
        if emu.rd_i(F["delay"]) > 0:
            emu.wr_i(F["delay"], emu.rd_i(F["delay"]) - 1)
            rows.append(snapshot(emu, init["funcNo"], tick))
            continue
        if emu.rd_i(F["bulletLife"]) != 0:
            ang = f32(emu.rd_f(F["angle"]) + emu.rd_f(F["angleAcc"]))
            f56 = emu.rd_f(F["f56c"])
            lim = f32(f56 + emu.rd_f(F["angleSpd"]))
            if f56 > 0.0 and ang > lim:
                ang = lim
            if f56 < 0.0 and ang < lim:
                ang = lim
            emu.wr_f(F["angle"], ang)
        # func6/8 在 update 里是内联副本，入口带 `cmp life,#1; b.lt` 的守卫
        # （0x50b6a8 / 0x50bc30）。独立函数体没有这个守卫，这里补上以对齐实机。
        skip_call = (func_addr in (FUNC_ADDR.get(6), FUNC_ADDR.get(8))
                     and emu.rd_i(F["life"]) < 1)
        for r in (UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3,
                  UC_ARM64_REG_X4, UC_ARM64_REG_X5):
            uc.reg_write(r, 0)
        uc.reg_write(UC_ARM64_REG_X0, OBJ)      # this
        uc.reg_write(UC_ARM64_REG_X8, SCRATCH + 0x200)   # 结构体出参缓冲
        # func24 之类以 Vec2* 为入参（x1），实机传的就是子弹当前位置；
        # 与 x8 的「出参缓冲」分开，避免互相覆盖
        uc.mem_write(SCRATCH + 0x240, struct.pack("<ff", EXTRA["x"], EXTRA["y"]))
        uc.reg_write(UC_ARM64_REG_X1, SCRATCH + 0x240)
        # func6/8/11 以 (x, y) 为浮点入参，并用 x8 指向的位置缓冲做「进/出参」
        nan = float("nan")
        uc.mem_write(SCRATCH + 0x200, struct.pack("<ff", nan, nan))
        set_s(uc, EXTRA["x"])
        uc.reg_write(UC_ARM64_REG_S1, struct.unpack("<I", struct.pack("<f", EXTRA["y"]))[0])
        uc.reg_write(UC_ARM64_REG_X30, EXIT)
        uc.reg_write(UC_ARM64_REG_PC, func_addr)
        try:
            if not skip_call:
                uc.emu_start(func_addr, EXIT, count=200000)
        except Exception as exc:  # noqa: BLE001
            rows.append({"func": init["funcNo"], "tick": tick, "error": str(exc)})
            break
        # 回读位置缓冲（原版把位置交给 func 改写后再积分）
        px, py = struct.unpack("<ff", uc.mem_read(SCRATCH + 0x200, 8))
        if math.isfinite(px) and math.isfinite(py):
            EXTRA["x"], EXTRA["y"] = px, py   # func6/8 写了位置缓冲才采信
        # update 默认尾段：vx/vy 由 (angle,speed) 合成
        ang = emu.rd_f(F["angle"])
        spd = emu.rd_f(F["speed"])
        emu.wr_f(F["vx"], f32(spd * f32(math.sin(math.radians(ang)))))
        emu.wr_f(F["vy"], f32(spd * f32(math.cos(math.radians(ang)))))
        # 注意：原版 vx=cos、vy=sin
        emu.wr_f(F["vx"], f32(spd * f32(math.cos(math.radians(ang)))))
        emu.wr_f(F["vy"], f32(spd * f32(math.sin(math.radians(ang)))))
        EXTRA["x"] += emu.rd_f(F["vx"])
        EXTRA["y"] += emu.rd_f(F["vy"])
        emu.wr_i(F["t1"], emu.rd_i(F["t1"]) + 1)
        rows.append({
            "func": init["funcNo"], "tick": tick,
            "x": round(EXTRA["x"], 6), "y": round(EXTRA["y"], 6),
            "vx": round(emu.rd_f(F["vx"]), 6), "vy": round(emu.rd_f(F["vy"]), 6),
            "angle": round(emu.rd_f(F["angle"]), 6),
            "speed": round(emu.rd_f(F["speed"]), 6),
            "hit_dis": round(emu.rd_f(F["hitDis"]), 6),
            "half_w": round(emu.rd_f(F["halfW"]), 6),
            "half_h": round(emu.rd_f(F["halfH"]), 6),
            "kind": emu.rd_i(F["bulletType"]),
            "life": emu.rd_i(F["life"]),
            "t": float(emu.rd_i(F["t1"])),
            "opacity": EXTRA["opacity"], "scale": round(EXTRA["scale"], 6),
        })
    return rows


FUNC_ADDR = {}
FUNC_SIZE = {}


def load_syms():
    txt = subprocess.run(["aarch64-linux-gnu-readelf", "-sW", SO],
                         capture_output=True, text=True).stdout
    for ln in txt.splitlines():
        p = ln.split()
        if len(p) >= 8 and p[3] == "FUNC":
            name = p[7]
            if "bulletFunc" in name and name.startswith("_ZN11EnemyBullet"):
                tail = name.split("bulletFunc")[1]
                num = ""
                for ch in tail:
                    if ch.isdigit():
                        num += ch
                    else:
                        break
                if num:
                    FUNC_ADDR[int(num)] = int(p[1], 16)
                    FUNC_SIZE[int(num)] = int(p[2])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--func", type=int)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--ticks", type=int, default=90)
    ap.add_argument("--out", default="/tmp/emu.jsonl")
    args = ap.parse_args()

    load_syms()
    emu = Emu(SO, HOOKS)
    funcs = sorted(FUNC_ADDR) if args.all else [args.func]
    with open(args.out, "w", encoding="utf-8") as fh:
        for fn in funcs:
            if fn not in FUNC_ADDR:
                continue
            # 与 apps/phk_tna_replica/main.cpp 的 MakeInitial 完全一致，
            # 否则 -1 哨兵值（A0/V0/V1）会让两侧走不同分支。
            init = {
                "funcNo": fn, "x": 320.0, "y": 480.0, "angle": 0.0, "speed": 3.0,
                "bulletType": 45, "hitDis": 4.0, "halfW": 4.0, "halfH": 4.0,
                "pf1": 1.0, "pf2": 4.0, "hitHantei": 1, "bulletLife": 0,
                "life": 4, "t1": 0,
                "angleInit": -1.0, "speedInit": -1.0, "spdInit2": -1.0,
                "angleSpd": 0.0, "angleAcc": 0.0, "f56c": 0.0, "speedCap": 0.0,
                "f598": 0.0, "prevAngle": 0.0, "xacc": 0.0, "xaccCap": 0.0,
                "accel": 0.0, "rotateFlg": 0, "delay": 0, "vanishDelay": 0,
                "p1": 0, "p2": 0, "f5e8": 0.0,
            }
            if fn in (28, 36, 79):
                init["p1"] = 2
                init["p2"] = 8
            for life0 in (4, 1, 0):
                init2 = dict(init)
                init2["life"] = life0
                for row in run_func(emu, FUNC_ADDR[fn], FUNC_SIZE[fn],
                                    args.ticks, init2):
                    row["life0"] = life0
                    fh.write(json.dumps(row) + "\n")
    print("wrote", args.out)


if __name__ == "__main__":
    main()
