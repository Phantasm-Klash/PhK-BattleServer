# 多初值档诊断：已知差异与后续路线（2026-11-09）

## 1 为什么加多初值档

`run.sh` 的默认门禁（profile 0）每帧用固定初值：中心位置 (320,480)、
`angle=0, speed=3`、`p1=p2=0`、`A0/V0/V1 = -1` 哨兵、`X/XC/F98/PA = 0`，
`life0 ∈ {4,1,0}`。很多 `bulletFunc` 的分支在这些哨兵值下恰好是 no-op，
因此生产实现即使只覆盖了「默认路径」也能通过 86/86。

`tna_emu.py --profile N` 与 `phk_tna_replica --profile N` 提供三档严格一致的
初值，`run.sh` 用 `PROFILES="0 1 2"` 可一次跑完：

| 档 | 位置 | angle/speed | 关键参数 | 覆盖目标 |
|---|---|---|---|---|
| 0 | (320,480) | 0 / 3 | -1 哨兵、p1=p2=0 | 默认直线（门禁档） |
| 1 | (400,520) | 45 / 2 | A0=30,V1=4,X=0.25,XC=6,F98=0.5,PA=10,angleAcc=1,bulletLife=1,p1=2,p2=3 | 出屏、角加速度/夹持、速度上限 |
| 2 | (-10,30) | 270 / 1.5 | A0=-45,V1=-2,X=-0.5,XC=1,F98=-0.25,PA=5,p1=1,p2=5 | x<0 / y<40、负向镜像与减速 |

★ 前置 `bulletLife != 0` 会让原版 update 把 angle 累加并夹到
`f56c + angleSpd`（p1 档 = 3），这是很多 p1 差异的公共来源，不是基准 bug。

## 2 当前结果

| 档 | MATCH | MISMATCH | RNG_DIFF | 状态 |
|---|---:|---:|---:|---|
| 0 | 86 | 0 | 0 | **门禁 PASS**（`run.sh` 默认） |
| 1 | 58 | 24 | 4 | 诊断档，已知差异 |
| 2 | 73 | 11 | 2 | 诊断档，已知差异（4 项修复前的读数） |

默认门禁只跑 profile 0，`PROFILES="0 1 2"` 是诊断用途，不要求全绿。

## 3 已修（对三档都有收益，且 p0 保持 86/86）

- **func0**：`funcNo=0` 在跳转表里 index=-1 越界，直接落共用出口，全库无
  `bl bulletFunc0@plt`。独立符号是 spawn 时初始化用的，**不是逐帧路径**。
  生产改为 no-op；p1/p2 的 `angle/speed/vx/vy/x/y` 差异消除。
- **func1**：每段反弹后按 `(p1-1)` 走字节表 @`0xb0a972` 换 bulletType
  （表项 `[0,22,2,6]` → 30 / 19 / 109 / 168）；p1=0 时 index 越界不换。
- **func12**：独立符号 `0x50d840` 出屏且 p1==1/2 时除改角外还有 `life--`。
- **func85**：`0x5112b4 str s8,[x19,#1356]` **无条件**写 hitDis（默认 4.0，
  仅 bulletType==0x25 且距离门命中时 20.0），旧实现漏了默认写回。

## 4 p1 剩余 24 个 MISMATCH + 4 个 RNG_DIFF（根因分类）

| 类别 | func | 说明 |
|---|---|---|
| A 阶段顺序 | 33, 46, 58, 59, 61, 64, 67 | 原版二次分派（`bl` 33/37/51/66/75/81/82/83）发生在**速度合成之后、位置积分之前**；生产在 `ApplyBulletFunc` 开头一次性调用，前处理与尾段顺序不同。非零参数下角度/速度被覆盖的时机不同。 |
| B 内联真值缺失 | 2, 11, 12, 17, 20, 29, 32, 38, 39, 48, 50, 62, 63 | 这些 func 在 profile 0 下因哨兵值退化为 no-op，但 p1 的非零 A0/X/XC/PA 会激活额外分支（对准、节拍、16B 复位块等），生产实现只覆盖了基准档。 |
| C 分派表误调用 | 0（已修）, 30, 54, 85（已修） | 部分 FuncN 是从独立符号抄来的，但 update 逐帧路径并不调用它们（或调用时机不同）。需要按跳转表逐编号判定。 |
| D RNG 调用次数/顺序 | 7, 52, 70, 76 | 两边都用同一个 `DeterministicRng` 算法，但 p1 非零 p1/p2 会让随机分支触发次数不同；需核对每个 func 的调用次数与顺序。 |
| E 浮点精度 | 73 | `hit_dis/scale` 差 ~0.2，原版全程 float 中间量，生产用 double；需要按指令链加 `static_cast<float>`。 |

p2 剩余（4 项修复前）：0, 1, 7, 12, 29, 30, 33, 46, 52, 54, 73, 78, 85 ——
与 p1 高度重合，集中在 A/B 两类，说明是系统性而非偶发。

## 5 下一步（建议顺序）

1. **把整帧基准的入口从分派点 `0x50b4e8` 提前到 `update` 入口
   `0x50b0b8`**，让 delay / vanish / angle 累加与夹持全部由原版代码决定，
   消除目前手工复刻前处理带来的角度偏差（func33/46/59 等）。
   需要补：LAYER_VT 的 2088/2104 槽、vtable 632/1744/1752 槽的语义桩。
2. **按跳转表重构 `ApplyBulletFunc` 为两阶段**：early（分派入口 / 共用出口内联）
   与 late（二次分派 9 个 + 29），中间插入 accel 应用与速度合成，
   与原版 `0x50c618 → 0x50c978 → 0x50cafc` 的顺序一致。
3. 用 p1/p2 逐类消掉 B/C/D/E，最后把 `run.sh` 的 `PROFILES` 默认值
   改回 `"0 1 2"`，让三档都成为门禁。
4. 同步客户端 `SpellKard/laya/src/core/sim/danmaku/bullet_motion.ts`
   （见 `findings-20261109.md` §6，该文件仍是无条件 `integrateVelocity()` 的旧口径）。

## 6 复现

```bash
cd /root/gotouhou/PhK-BattleServer
bash tools/tna_coverage/run.sh                 # 默认门禁：profile 0，必须全绿
PROFILES="0 1 2" bash tools/tna_coverage/run.sh   # 三档诊断，p1/p2 允许失败
PROFILES=2 bash tools/tna_coverage/run.sh         # 单独跑某一档
```
