# 多初值档：从诊断到三档全绿（2026-11-09 → 11-10 完成）

## 1 为什么加多初值档

`run.sh` 最初的门禁（profile 0）每帧固定初值：中心位置 (320,480)、
`angle=0, speed=3`、`p1=p2=0`、`A0/V0/V1 = -1` 哨兵、`X/XC/F98/PA = 0`，
`life0 ∈ {4,1,0}`。很多 `bulletFunc` 的分支在这些哨兵值下恰好是 no-op，
因此生产实现即使只覆盖「默认路径」也能通过 86/86 —— 属于覆盖盲区。

`tna_emu.py --profile N` 与 `phk_tna_replica --profile N` 提供三档严格一致的
初值：

| 档 | 位置 | angle/speed | 关键参数 | 覆盖目标 |
|---|---|---|---|---|
| 0 | (320,480) | 0 / 3 | -1 哨兵、p1=p2=0 | 默认直线 |
| 1 | (400,520) | 45 / 2 | A0=30,V1=4,X=0.25,XC=6,F98=0.5,PA=10,angleAcc=1,bulletLife=1,p1=2,p2=3 | 出屏、角加速度/夹持、速度上限 |
| 2 | (-10,30) | 270 / 1.5 | A0=-45,V1=-2,X=-0.5,XC=1,F98=-0.25,PA=5,p1=1,p2=5 | x<0 / y<40、负向镜像与减速 |

★ p1 档 `bulletLife != 0` 会让原版 update 每帧把 angle 累加并夹到
`f56c + angleSpd`（=3），**但原版帧末 `0x50cc84` 会把 bulletLife 递减**，
所以只有第一帧夹持。这一点是 p1 档大批差异的公共来源。

## 2 结果（2026-11-10）

| 档 | MATCH | MISMATCH | RNG_DIFF | 状态 |
|---|---:|---:|---:|---|
| 0 | 86 | 0 | 0 | **PASS** |
| 1 | 86 | 0 | 0 | **PASS** |
| 2 | 86 | 0 | 0 | **PASS** |

`run.sh` 默认 `PROFILES="0 1 2"`，三档全部纳入门禁；`ctest` 20/20。
诊断期间的最低点：p1 24 MISMATCH + 4 RNG_DIFF，p2 11 + 2。

## 3 修复清单（按收益排序）

### 3.1 架构级（一次性消掉 17 个 p1 差异、全部 p2 差异）

- **帧末 `bulletLife--`**：原版 `0x50cc84` 在 >0 时递减，决定下一帧开头是否做
  angle 累加/夹持。生产端漏掉 → 每帧都夹持，与整帧基准从第一帧起分叉。
- **`ApplyBulletFunc` 拆三阶段**（`BulletFuncStage`）：
  - stage 0 early = 分派入口 / 共用出口内联；
  - stage 1 mid = **func29**（`0x50c9ec`，共用出口 accel 应用**之后**、
    速度合成**之前**）；
  - stage 2 late = `0x50cafc` 二次分派 8 个 {33,37,51,66,75,81,82,83}。
  - `UpdateBulletMotion` 顺序：前处理 → stage0 → accel 应用 → stage1 →
    速度合成（3/29/47 跳过）→ stage2 → 预减位移（9 个编号）→ bulletLife--。
  - 遗留 WIP 曾把 func29 误放进 late，已按 `0x50c9ec` 的位置纠正。
- **共用出口 `0x50c8f0` 的 `speed += spdAcc` 夹到 speedCap** 已在上一轮补齐；
  本轮发现多个 func 写的是 **8B 拷贝 `{spdInit2,xacc}` → `{speed,spdAcc}`**，
  生产只写了 speed，导致同帧少加一次 xacc（func61/76）。

### 3.2 逐函数真值修正

| func | 修正 |
|---|---|
| 0 | 分派表 index=-1 越界，独立符号是 spawn 初始化 → **no-op** |
| 1 | 三段独立反弹；按 `(p1-1)` 字节表 @`0xb0a972` 换皮 {30,19,109,168}；p1==2 写 `speed=spdInit2`，p1==4 8B 拷贝 + `speedCap=XC` |
| 7 | 四段**各自**调用一次 `random(-p1,p1)`（原来汇总调用一次，RNG 流错位） |
| 11/12/70/78 | 分派入口的 `life<1` 门；func12 出屏且 p1==1/2 时 `life--` |
| 17 | 限速第三支：`d < A0 → prev-A0`（不是 `d<0`）；`d==A0` 保持 target |
| 29 | 三段：`t<life` 用 prevAngle 合成 vx/vy；`life<=t<life+p1` vx=vy=0（仅 `t==life+1` 写 A0）；`t>=life+p1` 复位 16B 并重新合成 |
| 30 | 门 `rem=(t-life)%p1`；`rem==0 → hitDis=20`、`rem==p2-1 → hitDis=4` |
| 33 | **不写 angle**（只写 xacc/xaccCap 两个目标坐标；尾部 atan2f 只用于 setRotation） |
| 46 | update 内联版是 `angle = s2 + 表[life==0]` 赋值，不是 `angle += ±90` |
| 52 | 门 `t%life==0`；`angle = p1 * random(-p2,p2)` |
| 54 | 字节表 @`0xb0a9c7` 解出 {163,157,158,159,160,161,162}（旧实现写成逆序） |
| 61/76 | 补 `accel=xacc`；func76 另有 `life<1` 门、`random(-p2,p2)`、`p1==0→kind17`、`life--`、angle 不归一 |
| 73 | **全程 float**（`SinDegF`）并 `p2++`；double 中间量放大出 ~0.2 的 hitDis 误差 |
| 78 | `y<40` 时 `angle=360-angle`（不是 180）并 `life--`；分派入口 `life<1` 门 |
| 85 | `0x5112b4` 无条件写 hitDis=4.0，仅 kind==0x25 且距离门命中时 20.0 |
| 4/5/27 | update 内联门控 / 无 `bl` → **no-op**（独立符号不是逐帧路径） |
| 58/59/47/3 | update 内联真值：58 用 `90±A0`、59 在 `t1<life` 时 `angle+=1`、47 节拍门、3 内联后跳过速度合成 |

## 4 判据补充（并行反汇编取证交叉验证）

- **func73 的 ~0.2 偏差主因是漏 `p2++`**（正弦自变量冻结在初值），
  float/double 精度残差只有 ~2e-6；两者都补才逐位绿。
- **基准噪声不要当 bug 修**：q1 从 tick17 起 <5.1e-4 的 x 漂移、
  q2 的 ≤1e-4 漂移，来自 emu 侧用 float32 累加位置；`compare_tna.py`
  的容差已覆盖，不应为此改生产实现。
- 逐帧权威是 update 的**分派入口内联块**，不是独立符号；内联块的
  `life<1` 门、RNG 位置、8B 拷贝的 accel 一半、镜像常数 C、`p2++`
  在 p0 哨兵初值下全部不可见 —— 这正是「单档全绿」的盲区。
- 取证笔记：`/d/gotouhou/tmp/disasm/notes-multiprofile.md`（本地临时文件，
  不入库）。

## 5 方法论教训

- **「两边一起错」的自洽假象**：p0 全绿不代表正确；只有把基准换成
  「执行原版 `update` 完整一帧」+ 多组非哨兵初值，才能暴露阶段顺序、
  字段漏写、float/double 精度、RNG 调用次数这类问题。
- **先找系统性根因再逐函数修**：帧末 bulletLife-- 与三阶段拆分各一次消掉
  十几个差异，比逐个 func 调参数高效得多。
- **整帧模拟的坑**：`bl bulletFuncN@plt` 需重定向到函数体；`x23` 指向可控
  零内存；`ldp x29,x30,[sp,#144]` 是成对加载，返回地址写 `sp+152`；
  `sdiv` 除零返回 0；`b.pl` 复用前一条 `fcmp` 的 flags。

## 6 复现

```bash
cd /root/gotouhou/PhK-BattleServer
bash tools/tna_coverage/run.sh                 # 默认三档门禁，必须全绿
PROFILES=0 bash tools/tna_coverage/run.sh     # 快速回归单档
PROFILES=2 bash tools/tna_coverage/run.sh     # 单独跑某一档
ctest --test-dir build-linux                  # 20/20
```
