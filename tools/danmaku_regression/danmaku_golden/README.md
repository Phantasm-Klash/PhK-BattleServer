# 弹幕密度金标准（danmaku golden）

这些 JSON 是 Phantasm Klash 弹幕引擎的**密度指纹**。它们不是好看的统计数字，
而是可执行的回归锁：任何一次改动只要让某个 tick 的弹数变了，回归台就会红。

## 谁生成、谁消费

| 文件 | 作用 |
| --- | --- |
| `tools/danmaku_regression.py` | 跑矩阵、比 golden、`exit 1` |
| `tools/danmaku_fingerprint.py` | 单份 JSONL → 指纹（在网关流式跑，见下） |
| `tools/danmaku_golden/<case>.json` | 每个用例一个 golden，人可读 |

用例名形如 `boss21_tna-d2-hp1200` = `<boss_id>-d<difficulty>-hp<boss_hp>`。

当前矩阵 3 boss × 4 难度 × 3 血量 = **36 个用例**：

| 维度 | 取值 |
| --- | --- |
| `boss_id` | `boss21_tna`（state 14 实测节奏表）、`boss21_tna_s20`（state 20 独立表）、`boss21`（#11 抽样样板，对照） |
| `difficulty` | 0 / 1 / 2 / 3 |
| `boss_hp` | 6000（正常）/ 1200（名义低值）/ 24（阶段穿越值） |

血量的三档不是凑数，各有分工：

* **6000**：300 tick 内既打不掉也跨不过 66%，锁「稳定态」密度。
* **1200**：名义低值。实测 300 tick 只掉 12 点血（`boss_damage` 很稀），**跨不过**
  66%/33% 阈值——所以它实际锁的是「血量不影响密度」这条不变量，与 6000 的指纹
  完全相同就是证据。
* **24**：真正能跨阈值的值。300 tick 内依次进入 spell_phase 1、2，对局仍在
  running，专门覆盖符卡阶段切换路径。

矩阵常量都在 `danmaku_regression.py` 顶部（`BOSS_IDS` / `DIFFICULTIES` / `BOSS_HPS`
/ `TICKS` / `SEED` / `RULESET`）。要扩矩阵改那里就行。

## 数字是什么

`phk_danmaku_trace` 直接跑 `BossRaceSimulation`，逐 tick 用**生产快照编码器**
`EncodeBossRaceSnapshotPayload` 写一行 JSON（客户端解析的同一种 wire 形状）。
固定 seed=12345、300 tick、`tna-1.40-danmaku`、两名脚本化玩家全程射击
（p-alpha 每 32 tick 换向并在 tick 100 放一次 bomb）。

指纹字段：

| 字段 | 含义 |
| --- | --- |
| `bullets_total` | 整份 trace 里出现过的**唯一** `bullet_id` 数 = 累计发射数。**不是**末帧存活数。 |
| `max_per_tick` | 单 tick「新生成子弹数」的最大值。 |
| `chains_records` | `chains[]` 记录总条数（触发表被实际展开的次数）。 |
| `ticks_with_bullets` | `bullets[]` 非空的 tick 数。 |
| `trace_lines` | 实际写入行数。对局提前 `Finished` 会小于 `ticks`。 |
| `spell_phase_starts` | `spell_phase_start` 事件总数（符卡阶段切换次数，两名玩家分开计）。 |
| `spell_phases` | 进入过的符卡阶段编号集合（已排序）。 |
| `pattern_ids` | 出现过的 `pattern_id` 集合（排序）。 |
| `spawn_series_sha256` | 逐 tick 新增子弹数序列的 sha256。**这是真正的锁**——比任何汇总数字都敏感。 |

汇总数字可能互相抵消（这一波多 3 颗、下一波少 3 颗，`bullets_total` 不变），
但 `spawn_series_sha256` 会翻。所以看差异时先看 sha，再看具体字段。

## 怎么跑

```bash
python tools/danmaku_regression.py            # 对比 golden，不一致 exit 1
python tools/danmaku_regression.py --update   # 重新生成 golden
python tools/danmaku_regression.py --only boss21_tna-d2   # 只跑匹配的用例（调试，不判 STALE、不删 golden）
python tools/danmaku_regression.py --fetch-traces         # 顺带下载原始 JSONL（很大）
```

前置：`tools/rsh.sh` 能免密登录网关，网关上有 `/root/gotouhou/PhK-BattleServer`。
本地需要 Git Bash 的 `bash.exe`（脚本会自动找；找不到用 `PHK_BASH` 指定）。

## 为什么指纹在网关上算

一份 300 tick 的 trace 约 50 MB（几百 KB ~ 几 MB 是早期规模，实测已经 50 MB），
24 个用例就是 1.2 GB。`danmaku_fingerprint.py` 被上传到网关，对每份 trace 流式
算完只回传几 KB 的 JSON。原始 trace 留在网关 `/tmp/danmaku-regress/traces/`，
要本地留档才加 `--fetch-traces`（下载到 `tmp/danmaku-regression/traces/`）。

**独立构建目录**：用 `build-regress`，不要用 `build-linux`（那里常有别的构建在跑）。
每次运行都会**增量构建** `build-regress`（`cmake --build --target phk_danmaku_trace`），
不是「有二进制就跳过」——回归台必须测当前源码，否则别人改了表你还在测旧二进制。
首次运行会自动 `cmake -S . -B build-regress -DCMAKE_BUILD_TYPE=Release`。

## 什么时候该 `--update`

**只有在你「有意」改动弹幕数据时。** 具体是：

1. 改节奏表本身（`src/danmaku/chain_node.cpp`、`include/phk/battle/danmaku/tna_boss21.hpp`、
   新增/替换 `BossPhaseTable` 条目）；
2. 改运动学 / 发射几何（速度、角度、`bullet_func_no`、`pf2`、spawn 偏移）；
3. 改难度缩放（`DifficultyScaleFor`）或符卡阶段切换的血量阈值；
4. 有意改矩阵本身（新增 boss / 难度 / 血量 / tick 数 / seed / ruleset）。

`--update` 之后**必须在同一个提交/说明里写清楚改了什么、为什么**。golden 的
diff 就是那次改动的量化影响——把它当成交付物的一部分，别当噪声丢掉。

## 差异不是噪声，是信号

对比模式红了，先别急着 `--update`。按这个顺序看：

1. **`spawn_series_sha256` 变了但所有汇总字段都没变** → 弹幕在 tick 之间挪了位置
   （提前/推后几帧），总量守恒。典型的「节奏偏移」类回归。
2. **`chains_records` 变了** → 触发表被展开的次数变了。查 `EmitChainBossTickDetailed`
   的触发条件、`ChainTablesActive`、`FindBossPhaseTable` 的查找键。
3. **`pattern_ids` 少了/多了** → 走错分支（比如 `chain:<boss_id>` 之外冒出了
   `boss_center_radial` 之类的回退路径）。
4. **`bullets_total` 变了** → 每波弹数或波数变了。查节点 `count_per_node` / `nodes`。
5. **`trace_lines` < `ticks`** → 对局提前结束（血量被打穿）。查伤害或 boss_hp。

如果确认是**非预期**的差异，那就是真 bug，别 `--update`。

## 阶段轮转断言

`boss21_tna` + 低血量用例会额外断言 trace 里**同时**出现 `chain:boss21_tna` 与
`chain:boss21_tna_s20` —— 这是「按符卡阶段轮转两张表」的端到端证据。

判定是**看证据的**，不是硬编码：

* 服务端还没有这个能力（`chain_node.hpp` 里没有 `BossStateBossIdForPhase`）→
  打印 `SKIP` 并说明原因，**不判红**（避免假绿）。
* 用例 300 tick 内根本没跨过阶段（`spell_phase_starts == 0`，比如 hp1200）→
  打印 `SKIP`：无阶段可轮转，断言无从谈起。
* 用例跨了阶段 → 必须两个 `pattern_id` 都在，否则 `FAIL` 并 `exit 1`。
* 整张矩阵里**一个**跨阶段的用例都没有 → `FAIL`：断言等于没覆盖。

当前状态：hp24 的 4 个用例都跨 3 次阶段且 PASS；hp1200 的 4 个用例 SKIP（未跨阈值）。
