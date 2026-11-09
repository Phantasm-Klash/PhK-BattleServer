# `EnemyBullet::update` 分派真值（AArch64 实测，2026-11-09）

> 对象：`libMyGame_arm64.so`（TNA 1.40，cocos2d-x C++，未 strip）
> 目的：纠正「79 个 `bulletFunc*` 符号 = 79 个逐帧运动函数」这一错误前提。
> 结论先行：**符号表不是分派表**。真正决定某编号是否逐帧执行的是 `update()` 里的跳转表。

---

## 1. 跳转表位置与解码方式

`EnemyBullet::update(float)` @ `0x50b0b8`：

```
50b4e8: ldr   w8, [x19, #1460]     ; funcNo (+0x5b4)
50b4ec: sub   w8, w8, #0x1         ; index = funcNo - 1
50b4f0: cmp   w8, #0x54            ; > 84 → 越界出口
50b4f4: b.hi  0x50c618
50b4f8: adrp  x9, 0xb0a000
50b4fc: add   x9, x9, #0x88e       ; ★ 表地址 = 0xb0a88e
50b500: adr   x10, 0x50b514        ; ★ 基址 = 0x50b514
50b504: ldrh  w11, [x9, x8, lsl #1]
50b508: add   x10, x10, x11, lsl #2 ; ★ 表项 ×4，不是 ×1
50b510: br    x10
```

**修正**：旧文档记作「半字偏移」，漏了 `lsl #2`。不乘 4 会得到非 4 字节对齐的地址
（如 `0x50b955`），落到指令中间，从而得出「大量函数共用同一入口」的假象。

## 2. 结果：85 个编号 → 66 个不同入口

- **66 个独立入口**：直接内联或 `bl bulletFuncN@plt`。
- **20 个落到共用出口 `0x50c618`**：`3, 9, 10, 26, 27, 29, 33, 37, 47, 49, 51, 56, 59, 66, 68, 69, 75, 81, 82, 83`。

但 `0x50c618` **不是 no-op**：

```
50c618: ldr  w8, [x19, #1328]   ; +0x530 rotateFlg
50c61c: cmp  w8, #0x2 …
```

它是**旋转尾 + 一条比较链**。该链里能直接看到 `cmp w8,#0x3b`(59)、`#0x38`(56)、`#0x31`(49)、
`#0x2f`(47)、`#0x1d`(29)、`#0x17`(23)，即「共用出口」的成员在这里被逐个分派。

另有一段**二次分派**（`0x50cbfc`–`0x50cc58`）直接 `bl` 到独立函数体，覆盖
`33, 37, 51, 66, 75, 81, 82, 83`。

> 所以：`0x50c618` = 「未内联」的入口，不等于「不执行」。

## 3. 判别技巧：用特征常量判断是否内联

比较链里很难读，但有个低成本判据——**看函数体内的特征常量是否出现在 `update` 区间**。

`bulletFunc3` 的 `0.0163333` 是 double `0x3F90B9AF72015D87`，装载序列为
`movz/movk x8, #0x5d87 / #0x7201 / #0xb9af / #0x3f90`。

在 `update` 区间（`0x50b0b8`–`0x50cc90`）grep 到：

```
50c9b8: movk x8, #0x7201, lsl #16
50c9bc: movk x8, #0xb9af, lsl #32
50c9c8: movk x8, #0x3f90, lsl #48
50c9c4: add  x9, x19, #0x57c      ; vx
```

⇒ func3 **被内联进 update**，逐帧执行。

反过来，`bl bulletFunc3` 在全库**不存在**——只看调用点会误判成死代码。
**教训：判「是否执行」要用特征常量，不能只用 `bl` 交叉引用。**

## 4. vtable 槽位怎么解（这次真正卡住的地方）

`func28/36/79` 三处都是尾调用 `ldr x2,[x8,#0x490]; br x2`（`x8 = [x0]` 即 vptr）。

- `_ZTV11EnemyBullet` @ `0xd46ca8`（1856 B = 232 槽），relocation 类型是
  `R_AARCH64_ABS64`（带符号名），不是 `RELATIVE`。
- Itanium ABI：符号指向表头（offset-to-top + typeinfo 共 16 B），**vptr = `_ZTV + 0x10`**。
  直接拿 `_ZTV + slot` 会整体错 2 个槽。
- 按 `0xd46cb8 + idx*8` 查 relocation：

| slot | 索引 | 解出符号 | 语义 |
|---|---:|---|---|
| +0x168 | 45 | `cocos2d::Node::getContentSize()` | 子弹贴图尺寸（func6/8 的环绕边界） |
| +0x490 | 146 | `cocos2d::Node::setOpacity(unsigned char)` | 透明度（func28/36/79 的落点） |

## 5. 六个「疑似未实现」的真实语义

| func | 真实行为 | 生产侧 |
|---|---|---|
| 3 | `vx/vy += 0.0163333·(cos life, sin life)`，角度制，double 乘加 | `Func3`（末尾 `IntegrateVelocity` 会覆盖，见注） |
| 6 | X 轴环绕：`x > halfW+cw → -cw`；`x < -cw → halfW+cw`；触发即 `life--` | `Func6` |
| 8 | XY 双轴环绕：X 同 6；`y > halfH+ch → 40-ch`；`y < 40-ch → halfH+ch`；触发即 `life--` | `Func8` |
| 28 | `setOpacity(max(255 - p2·(t1-p1), life))`，`d<0` 时 0（淡出） | `Func28` |
| 36 | `setOpacity(128 + 128·sin(p1·(t1-life)+90))` 夹 [0,255]，且 `hitHantei = alpha>127` | `Func36` |
| 79 | `setOpacity(min(p2·(t1-p1), life))`，`d<0` 时 0（淡入） | `Func79` |

★ **本节注释已被整帧基准证伪（2026-11-09）**：原版 func3 的内联代码写完
`vx/vy` 后是 `b 0x50cafc`，**直接跳过** `50ca58` 的默认速度合成，所以 func3 的
叠加对轨迹**有贡献**（整帧实测 vx=0.0163，而非 3）。真正「被覆盖」的只是
旧基准手工复刻尾段造成的假象。func29（`bl` 后 `b 50cafc`）与 func47
（50c9f8 节拍分支）同理。生产端现已用 `SkipsVelocitySynthesis` 跳过默认段。

`cw/ch` = `getContentSize()`；本工程按 `2·halfW / 2·halfH` 约定，`halfW/halfH` 需由
生成器（弹种表）填入，为 0 时不环绕。

## 6. 已解（原「仍未解」）

基准已改为**执行原版 update 完整一帧**（`tna_emu.py --mode update`），
86 个 funcNo 全部有逐弹真值，`run.sh` = MATCH 86 / 0 MISMATCH。

- 9/10/49/56/59/68/69 无独立符号的原因：它们落在共用出口 `0x50c618`，
  由 rotateFlg 尾 + funcNo 比较链分派。整帧实测 9/10/49/56/68/69 为默认
  直线；**59 有内联实现**（`0x50c768`，`t1<life` 时每帧 `angle += 1`）。
- 二次分派表 @`0xb0a938` 已完整解码：26 走参数方程 setPosition，
  33/37/51/66/75/81/82/83 `bl` 后跳过位置积分，其余默认积分。
- 共用出口 `0x50c8f0` 的 `speed += spdAcc`（夹到 speedCap）已补进生产。

剩余边界见 `findings-20261109.md` §6（主要是客户端 TS 镜像尚未同步）。

## 7. 复跑

```bash
cd /root/gotouhou/PhK-BattleServer
bash tools/tna_coverage/run.sh      # exit 0 = 0 MISMATCH / 0 MISSING / prod_noop==0
```
