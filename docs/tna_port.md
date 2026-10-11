# 千夜追忆真实弹幕系统移植（PhK-BattleServer）

> 目标：把《东方千夜追忆》(gotouhou) 1.40 的真实弹幕系统移植进 `PhK-BattleServer`
> （C++17），替换原有 10 个合成测试弹幕。
>
> **版权**：东方同人逆向成果（`libMyGame_arm64.so` / `libMyGame.so` 静态反汇编），
> **仅用于自研玩法**。数据来源：`rev/patterns/*` 逆向报告 + `PhK-Protocol/data/tna_contract.json`。

---

## 1. 模块结构

```
include/phk/battle/danmaku/
  bullet_kind.hpp        # 适配层：别名到协议契约 phk/v1/danmaku.hpp 的 BulletKind(216) + 帧/图集查询
  bullet_motion.hpp      # BulletMotion 状态、MotionTargets 钩子、DeterministicRng、79 函数分派
  spawn_command.hpp      # 适配层：别名到协议契约 phk/v1/danmaku.hpp 的 BulletSpawnCommand(96B)
  emitter.hpp            # 整圆径向发射器 + BulletTemplate（运动学参数模板）
  pattern_timeline.hpp   # PatternWave / PatternDef / 10 个真实模式库
  player_loadout.hpp     # 52 射击 + 13 炸弹数据表接口
src/danmaku/
  bullet_motion.cpp      # ★ 79 个 bulletFunc 的确定性运动学
  emitter.cpp            # RadialAngles / EmitRadial
  pattern_timeline.cpp   # 10 个真实模式 + 逐 tick 波次发射
  player_loadout.cpp     # 52 射击 + 13 炸弹 + 伤害/贴图表
tests/
  danmaku_tests.cpp      # 运动学/发射器/确定性/数据覆盖单测
docs/tna_port.md         # 本文档
```

> **契约单一来源**：`BulletKind`(216) / `BulletMotion`(79) / `BulletSpawnCommand`(96B)
> 的**权威定义**在 PhK-Protocol 生成契约 `phk/v1/danmaku.hpp`（客户端/服务端共用，
> include 根 `../PhK-Protocol/gen/cpp`）。本仓库的 `bullet_kind.hpp` / `spawn_command.hpp`
> 仅为**别名 + 便捷查询**适配层，避免 216 个枚举值在两处重复漂移。
> 该生成头已 re-vendor 到 `third_party/phk-protocol/gen/cpp/phk/v1/` 以支持无 sibling
> checkout 的构建。

集成点：`src/boss_race.cpp`（`BossRaceBullet` 增加 `bullet_kind` / `motion_func`；
`BossRacePatternIds()` 与发射逻辑改为真实弹幕；`LiveBullet` 保存完整运动学状态）。

---

## 2. 单位与坐标约定（重要）

1. **角度单位 = 度（degree）**，不是弧度。原版 `MyMathUtil::sin/cos` 内部 `×2π/360`；
   本模块 `SinDeg/CosDeg` 等价实现。**全模块统一，无任何弧度混用**。
2. **服务端权威坐标 = 整数 milli（1/1000 px）**，沿用 `BossRaceBullet` 风格。
3. **弹幕模块内部使用「屏幕像素坐标」(0..640, 0..960)**，因为原版 `bulletFunc` 的
   边界反弹常量 `x>320 / x<0 / y>480 / y<40` 按 640×960 场地设计。
   `boss_race` 在边界处做转换：
   - 发射/更新：`screen = 320 + centered_px`（y 同理 +480）
   - 快照输出：`x_milli = llround((screen_x - 320) * 1000)`
4. **发射位置哨兵**：`spawn_x/spawn_y == -1.0f` → 使用发射者坐标（`kSpawnFollowEmitter`）。
5. **整圆径向发射**：`angleStep = 360.0f / count`；起始角 `= baseAngle - spread`；
   `while (angle < 0) angle += 360.0f`（归一化到 [0,360)）。
6. **确定性**：`DeterministicRng(seed, tick, pattern_index, spawn_index)` 哈希实现，
   **禁用系统时间/帧率**。原版 `bulletFunc` 本身不含 RNG（参数来自 Command）；
   只有「随机角/随机化」族（`func52/71/76/70/62/7`）调用 `MyMathUtil::random`，
   这里以确定性哈希取代。

---

## 3. 79 个 bulletFunc 的完成度

全部 **79** 个编号均有实现（`ApplyBulletFunc` 全覆盖，`IsKnownBulletFunc` 全返回 true）：

| 族 | func# | 置信度 |
|---|---|---|
| A 初始化/直线 | 0,4,29,47 | 实测 |
| B 加速/减速 | 5,35,39,43 | 5/39/43 实测；35 阈值推断 |
| C 正弦调速/角 | 25,40,43,73,81 | 实测 |
| D 追踪/限速转向 | 2,17,18,48 | 17/48 实测；2 限速转向语义实测、条件极性推断（已按最短路径归一化）；18 推断 |
| E 周期瞄准/出屏转向 | 11,12,13,70,76 | 11/12/70/76 实测；13 细节推断 |
| F 边界反弹/镜像 | 1,7,20,32,38,57,58,62,67,77,78 | 1/7/32/57/58/77/78 实测；20/38/62/67 推断 |
| G 接近触发 | 15,16,24,34,41,42,44,45,53,60,74,80,84,85 | 34/41/42/60/84/85 实测；其余阈值/细节推断 |
| H 分裂/召唤 | 31,50,55,83 | 推断（子子弹生成交宿主，仅状态标记） |
| I 换皮/半径 | 30,54,65 | 实测（贴图切换交宿主） |
| J 参数方程/摆线 | 26,33,66,75,82 | 推断（多路 cos/sin 系数未逐指令验证） |
| K 状态机 | 19,21,22,23,51,71,72 | 21/22/23/71/72 实测；19/51 推断 |
| L 辅助（无字段写入） | 3,6,8,28,36,79 | 按 no-op 处理（原版仅返回布尔/数值） |

**统计**：约 **44 个直接来自实测公式**（逐指令直译）；其余为**推断/近似**，已在源码
注释中逐函数标注 `[实测]` / `[推断]` / `[推断阈值]`。

### 已知偏差 / 简化

- `update()` 的 `t1`(0x5dc) 计时器：本实现每帧自增；`life`(0x5b8) 作为构造阈值
  （与 disasm 中「与 T 比较」的用法一致）。原版两计数器的精确关系未完全定位 → **推断**。
- 「换皮」`setBulletType` 调用：服务端不渲染，仅保留 `bullet_kind` 与 `hit_dis` 变更；
  需要换皮的族（1/30/31/39/41/48/54/65/70/76/84）**外观变化未建模**。
- 分裂族（31/41/50/55/83）依赖宿主回调生成子子弹，本模块只置状态标记
  （`p2=1` / `rotate_flg=2`），**未真正生成子子弹**。
- 「全局状态」族（19/21/22/23/34/51/57/71/72/77）需要 `GameLayer` 状态码；
  `MotionTargets::global_state = -1` 时退化为默认分支。

---

## 4. 发射器

`EmitRadial(cmd, tmpl, ctx)` 严格复刻 `Enemy::createBullets1`（@0x37bacc）已解出语义：

- `count == 0` → 直接返回（原版 `cbz w8, return`）。
- `spawn_x/spawn_y == -1.0f` → 发射者坐标。
- `angleStep = 360/count`，起始角 `baseAngle - spread` 归一化。

`SpawnCommand` 为 96 字节 POD，`static_assert(sizeof == 96)` 保证与原版布局一致。
其余 20 个透传参数 → `spriteWithFile` → 子弹字段的**逐参数映射未解出**，因此
运动学参数显式放在 `BulletTemplate` 中，与角度分布解耦（推断）。

---

## 5. 自机射击与炸弹（52 + 13）

数据驱动：一张表 + 一个引擎（`player_loadout.cpp`，由 `player_shots_reimpl.hpp` 转写）。

- 52 条射击：`name/chara/mode/variant/bullets_per_volley/speed/angle 范围/damage/
  cooldown/tex_type/resolved/逐发子弹(type,angle,speed,dx,dy)`。
- 13 个炸弹：`duration_frames/invuln_frames/damage/bomb_type/texture/逐发子弹`。
- 伤害由 type 查表（`PlayerBulletDamage`，来自 `MyBullet::spriteWithFile` 跳转表）。
- `resolved=false` 的条目（含 2 个空实现 `YukariB2`/`AliceA2`）保留原样，未编造数值。

---

## 6. 合成弹幕替换

- 主路径：`ruleset_version` 默认 `"tna-1.40"` → `SpawnRealBulletsForTick`，
  使用 `BossRacePatternLibrary()` 的 **10 个真实模式**：
  `kome_ring / kusabi_gap_ring / arrow_aimed_fan / ring_spiral / kome_homing /
   heart_sine_speed / kunai_bounce / sizuku_sine_radius / stama_mirror / chou_four_dir`。
  每个模式引用**真实弹种**（kome/kusabi/ring/heart/kunai/sizuku/stama/chou/arrow）
  与**真实运动学族**（A/C/D/E/F）。
- 兼容路径：`ruleset_version == "mvp-boss-race-s0"` 时仍走旧的 10 个合成模式
  （`ring/gap_ring/.../blossom`），**不再作为主路径**。
- `BossRaceBullet` 新增 `bullet_kind`（kEnemyBullet 值）与 `motion_func`（bulletFunc 编号）。

---

## 7. 测试

- `tests/boss_race_tests.cpp`：更新为真实模式 id（首 `kome_ring`、末 `chou_four_dir`），
  新增「子弹携带真实 bullet_kind(9..224)」断言。
- `tests/danmaku_tests.cpp`：运动学逐帧坐标（A/B/C/D/E/F 六族）、发射器整圆均分、
  确定性（同参数同序列）、数据覆盖（216 kinds / 79 funcs / 52 shots / 13 bombs）。
- `ctest`：**4/4 全绿**（`phk_battle_tests` / `phk_boss_race_tests` /
  `phk_danmaku_tests` / `phk_battle_lifecycle_tests`），构建无警告。

---

## 8. 未完成 / 待办

1. `spriteWithFile` → `EnemyBullet` 字段的逐参数映射未解出（影响 Command 透传）。
2. 「换皮」外观切换未建模（服务端无渲染）。
3. 分裂族未真正生成子子弹（需宿主回调）。
4. `GameLayer` 全局状态机在服务端无对应（退化为默认分支）。
5. `update()` 的 `t1`/`life` 双计数器语义为推断。
6. `bullet_kind` / `motion_func` 已加入 `BossRaceBullet`；若要把它们暴露到快照协议，
   需与 `PhK-Protocol` 协调（本仓库未改 `match_lifecycle.cpp` 的 JSON 输出）。
   协议侧已提供 `DanmakuBulletState` / `BulletSpawnCommand` / `BATTLE_PAYLOAD_TYPE_DANMAKU_SPAWN`。
7. 已 re-vendor：`third_party/phk-protocol/gen/cpp/phk/v1/` 的 `manifest.hpp`(389 字段)
   与 `danmaku.hpp` 已由 Protocol 侧同步到 canonical（Source commit `b86d7f8`）。
   已验证 **sibling 与 vendored 两个 include 根**均构建通过、`ctest` 4/4 绿、0 warning。
