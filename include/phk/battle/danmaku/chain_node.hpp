#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "phk/battle/danmaku/chain_split.hpp"     // kChainSplitChildrenPerHost
#include "phk/battle/danmaku/emitter.hpp"       // BulletTemplate / EmitContext
#include "phk/battle/danmaku/pattern_timeline.hpp"  // AimMode
#include "phk/battle/danmaku/spawn_command.hpp"  // SpawnCommand（96B 语义）

// Boss 弹幕时间线的 ChainNode 数据表（P3 第一步）。
//
// 来源（千夜追忆 1.40 逆向，docs/apk-reverse-notes/patterns/boss_pattern_tables.md）：
// 原版 `Enemy::setBossBulletNN` 是「逐帧硬编码的参数序列 + 节点链表构造器」——
// 每个触发块在 `frame % PERIOD == PHASE` 时构造 1..N 个节点（operator new + 挂链），
// 节点布局（0x40 尺寸，以 #11 为样板）：
//   +0x00/+0x08 prev/next（链表，服务端不需要——发射顺序即链序）
//   +0x10/+0x14 f32   偏移 X / 偏移 Y（或角度；块 A 实证为 (±cos a·30, ±sin a·30) 或 ±30）
//   +0x24     u16    发射数（= 96B Command 的 count）
//   +0x28     int    模式 id（直接写进 CreateBulletCommand+0 的 bulletType；
//                     再经 bullet_mode_map 双跳转表推 (w2,w3) = (bulletFunc, kind)，推断）
//
// 证据分级约定（遵循 rev/patterns）：**[实测]** 指令可读；**[推断]** 上下文推得；
// **未能确定/未转写** 证据不足——留默认值并在此注释，禁止编造数值假装全量还原。
//
// 启用条件（硬约束）：只在 ruleset_version == kRulesetDanmakuMechanics
// （"tna-1.40-danmaku"）且 boss_id 非空、库内有该 boss 的表时生效；
// 其余一切情况（含默认 ruleset、未知 boss_id）逐字节回落现有 10 模式轮转。
// 判定入口：ChainTablesActive()。
namespace phk::battle::danmaku {

// ---------------------------------------------------------------------------
// 分裂规则（子代触发条件；子代参数计算复用 chain_split.hpp 的现有机制）
// ---------------------------------------------------------------------------
enum class ChainSplitTrigger {
    None,       // 不分裂（默认）
    OnAgeTicks, // 宿主生成后经过 after_ticks 触发（对应原版 T==P1 类帧触发）
    OnFunc31,   // 宿主 bulletFuncNo==31 且 p2!=0（复用 ChainSplitRequested 判定）
};

struct ChainSplitRule {
    ChainSplitTrigger trigger = ChainSplitTrigger::None;
    std::uint32_t after_ticks = 0;                 // OnAgeTicks：宿主 spawn 后多少 tick
    std::int32_t children_per_host = kChainSplitChildrenPerHost;  // 子代数（默认同链式分裂常量）
};

// ---------------------------------------------------------------------------
// ChainNode：一个触发节点的完整数据
// ---------------------------------------------------------------------------
struct ChainNode {
    // 触发条件：tick % trigger_period == trigger_tick 时本节点触发。
    // 原版语义 = 全局帧计数器 this->[0x6f4] 的取模（64 tick 真实加速下 frame 原样当 tick 用，
    // 禁换算）。trigger_period == 1 = 每 tick 判定（原版无对应块，占位能力）。
    std::uint32_t trigger_period = 1;
    std::uint32_t trigger_tick = 0;

    // 96B 发射指令（原版节点 +0x24 → count、+0x28 → bullet_type、+0x10/+0x14 → spawn 偏移）。
    SpawnCommand spawn_command;
    // 运动学模板（bullet_func_no / kind / 速度等）。默认值 = 直线初始化。
    BulletTemplate motion_override;
    // 瞄准（覆盖 spawn_command.base_angle_deg）；原版 getAngleToMyShip 块的落地口。
    AimMode aim = AimMode::None;

    // 分裂规则 + 子代数据（递归）。子代的运行期生成由 chain_split.hpp 执行；
    // children 目前承载数据格式（已解码的分裂子节点），运行时激活属后续工作。
    ChainSplitRule split_rule;
    std::vector<ChainNode> children;

    // ---- 溯源字段（人读，非执行；空 = 无补充）----
    std::string_view angle_expr;    // 原版角度表达式，如 "getAngleToMyShip(ex,ey)"
    std::string_view offset_expr;   // 原版偏移表达式，如 "±cos(random(1,360))·30"
    std::string_view provenance;    // 证据来源，如 "boss_pattern_tables.md §4.2 [实测]"
};

// ---------------------------------------------------------------------------
// BossPhaseTable：一个 (boss, 原版阶段字段) 的触发块组
// ---------------------------------------------------------------------------
struct BossPhaseTable {
    std::string_view boss_id;
    // 原版 Enemy+0x534（及 +0x6f8/+0x670）阶段字段值。**#21 未解码 → 0 占位**；
    // 已解码样板（#11）中：触发块 A 由 phase A(0xa..0x10) + phase B(0xb..0xe) 门控，
    // 触发块 B 另由 phase C(0xb..0xe) 门控。自研服务端未建模 Boss 阶段状态机
    // （逆向表 §7.3 标注这些字段语义为推断），当前实现把该 boss 的全部阶段表合并为
    // 一条常活时间线（见 EmitChainBossTick）；phase_id 仅作文档性数据保留。
    std::uint32_t phase_id = 0;
    // 原版节点尺寸（#11 = 0x40 实测）。#21 未在逆向表给出，按任务书假定同布局。
    std::uint32_t node_size_bytes = 0x40;
    // 逆向表登记的该函数总节点数（#21 = 146 实测）。已转写数 = nodes 展开数，
    // 远小于本数属预期（抽样范围见各节点 provenance / 本文件末尾说明）。
    std::uint32_t source_node_count = 0;
    std::vector<ChainNode> nodes;
};

// 权威库（顺序即库顺序；新增 boss 只允许追加，不得重排）。
// 当前仅含 setBossBullet21 的抽样转写（boss_id "boss21"）。
[[nodiscard]] const std::vector<BossPhaseTable>& BossPhaseLibrary();

// 按 (boss_id, phase_id) 查表；找不到返回 nullptr。
[[nodiscard]] const BossPhaseTable* FindBossPhaseTable(std::string_view boss_id, std::uint32_t phase_id);

// 该 boss 的全部阶段表（合并时间线用；out 清空后填充，保持库序）。
[[nodiscard]] std::vector<const BossPhaseTable*> FindBossPhaseTables(std::string_view boss_id);

// 该 boss 是否有阶段表。
[[nodiscard]] bool HasBossPhaseTable(std::string_view boss_id);

// ChainNode 路径启用判定（硬约束）：
//   ruleset_version == kRulesetDanmakuMechanics（"tna-1.40-danmaku"）
//   && boss_id 非空 && 库内有该 boss 的表。
// 实现见 chain_node.cpp（kRulesetDanmakuMechanics 常量权威定义在 boss_race.hpp，
// 为避免循环依赖在 .cpp 内 include）。
[[nodiscard]] bool ChainTablesActive(std::string_view ruleset_version, std::string_view boss_id);

// 展开某阶段表在指定 tick 的发射（纯函数，不持有状态）。
// 规则：节点 trigger_period != 0 且 tick % trigger_period == trigger_tick 时触发；
// count == 0 的节点不发射（原版 createBullets1 入口 cbz 语义——「未转写」节点即如此）。
[[nodiscard]] std::vector<BulletMotion> EmitChainTableTick(
    const BossPhaseTable& table,
    std::uint64_t tick,
    const EmitContext& ctx,
    const MotionTargets& targets
);

// 展开某 boss 全部阶段表在指定 tick 的发射（合并时间线；库序 + 表内序）。
[[nodiscard]] std::vector<BulletMotion> EmitChainBossTick(
    std::string_view boss_id,
    std::uint64_t tick,
    const EmitContext& ctx,
    const MotionTargets& targets
);

// 某阶段表在指定 tick 触发的节点数（含 count==0 的未转写节点；测试/调试用）。
[[nodiscard]] std::size_t ChainTriggerCountAt(const BossPhaseTable& table, std::uint64_t tick);

// 递归展开节点树（nodes + children，先序；测试「表加载」用）。
[[nodiscard]] std::vector<const ChainNode*> FlattenChainNodes(const BossPhaseTable& table);

}  // namespace phk::battle::danmaku
