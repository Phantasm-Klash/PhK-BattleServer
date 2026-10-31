// ChainNode 数据表（数据 + 纯函数展开）。
//
// 当前唯一条目：setBossBullet21 抽样转写（boss_id "boss21"，对应原版
// Enemy::setBossBullet21 @0x4d6f28，40440 字节 / 10110 指令 / 146 节点 [实测统计]）。
//
// **抽样范围声明（禁止编造）**：
// boss_pattern_tables.md 对 #21 只有聚合统计，没有逐节点解码；已逐指令解码的节点
// 属于 #11（473 节点的样板函数）。本文件按任务书转写「可实证的节点」= #11 已解码
// 触发块的结构与数值，作为 #21 时间线的抽样样板：
//   * 触发块 A（§4.2，frame % 300 == 0）：头节点（id 0xfa=250，空载荷 count 0）+
//     载荷节点（id 0x32=50，count 1，偏移 ±cos(a)·30 / ±sin(a)·30 或 ±30）——全字段实测；
//   * 触发块 B（§4.3，frame % 200 ≤ 59 且 frame % 16 == 0）：触发时刻与
//     getAngleToMyShip 瞄准实证；节点 id / count / 偏移**未能确定** → count=0 占位
//     （不发射），角度表达式与 72.0/−0.1 参数记录在溯源字段。
// 其余约 140 个 #21 节点未转写（逆向表未解码）。#21 自身的周期/阶段门控/节点 id
// 同样未解码——块 A/B 的周期与门控值取自 #11 解码，标注为样板来源。
// 节点尺寸 0x40 为任务书指定假定（#11 实测值；#21 尺寸逆向表未给出）。
#include "phk/battle/danmaku/chain_node.hpp"
#include "phk/battle/danmaku/bullet_mode_map.hpp"
#include "phk/battle/danmaku/tna_boss21.hpp"

#include <cstddef>
#include <utility>

#include "phk/battle/boss_race.hpp"  // kRulesetDanmakuMechanics（权威常量）

namespace phk::battle::danmaku {
namespace {

// 原版模式 id → (w2, w3)（bullet_mode_map 双跳转表，字段赋意为[推断]）。
// 50 → (53, 182)：w2=53 作 bullet_func_no；w3=182 的字段赋值未验证，
// 故 motion_override.bullet_kind 留 0 = 发射时回退 cmd.bullet_type（50 → kome 纹理，
// 与 bullet_kind_table 的纹理表语义一致，实测）。
constexpr int kBlockAPayloadModeId = 0x32;   // 50 [实测：#11 §4.2]
constexpr int kBlockAHeadModeId = 0xfa;      // 250 [实测：#11 §4.2 头节点]
constexpr int kBlockAPayloadFuncNo = 53;     // w2 [推断：BulletModeForId(50)]

// 触发块 A 的载荷节点偏移（#11 §4.2 [实测]）：
//   a = random(1, 360)
//   s8 = (A*0.2 > enemyX) ? ±30 : ±(cos(a)*30)     // A = Enemy+0x630 循环计数（语义推断）
//   s9 = (A*0.1 > enemyY) ? ±30 : ±(sin(a)*30)     // B = Enemy+0x634 循环计数（语义推断）
// 随机量与 fcsel 分支无法静态化为数据 → spawn_x/spawn_y 留「跟随发射者」哨兵，
// 原始表达式记入 offset_expr（人读）。±30 的轴对齐分支同样只在运行期可知。
constexpr float kBlockAOffsetMagnitude = 30.0f;

// 触发块 B（#11 §4.3）：帧条件 frame % 200 ≤ 59 且 frame % 16 == 0。
// frame % 16 == 0 在 200 窗内的落点 = {0,16,32,48}（均 ≤ 59）。
constexpr std::uint32_t kBlockBPeriod = 200;
constexpr std::uint32_t kBlockBOffsets[] = {0, 16, 32, 48};

ChainNode MakeBlockAHeadNode() {
    ChainNode node;
    node.trigger_period = 300;  // frame % 300 == 0 [实测：#11 §4.2]
    node.trigger_tick = 0;
    // 头节点：空载荷（原版 p1[0x28]=0xfa，p1[0x10..0x24]=0）→ count 0 = 不发射。
    node.spawn_command.bullet_type = kBlockAHeadModeId;
    node.spawn_command.count = 0;
    node.provenance = "boss_pattern_tables.md §4.2 触发块 A 头节点 [实测]（#11 解码样板）";
    return node;
}

ChainNode MakeBlockAPayloadNode() {
    ChainNode node;
    node.trigger_period = 300;
    node.trigger_tick = 0;
    node.spawn_command.bullet_type = kBlockAPayloadModeId;  // 原版节点 +0x28 [实测]
    node.spawn_command.count = 1;                            // 原版节点 +0x24 [实测]
    node.spawn_command.spawn_x = kSpawnFollowEmitter;        // 偏移含 random/fcsel，未静态化
    node.spawn_command.spawn_y = kSpawnFollowEmitter;
    // base_angle_deg / spread / 速度：#11 §4.2 未解码 → 未转写（默认 0）。
    node.motion_override.bullet_func_no = kBlockAPayloadFuncNo;  // w2 [推断]
    node.motion_override.bullet_kind = 0;  // 0 = 用 cmd.bullet_type(50)（w3=182 赋值未验证）
    node.offset_expr = "s8/s9 = (A*0.2>enemyX ? ±30 : ±cos(random(1,360))*30) / "
                       "(A*0.1>enemyY ? ±30 : ±sin(random(1,360))*30)";
    node.provenance = "boss_pattern_tables.md §4.2 触发块 A 载荷节点 [实测]（#11 解码样板）；"
                      "#21 自身未逐块解码，周期/门控/id 均取自 #11";
    return node;
}

// 触发块 B 的 4 个触发项（200 窗内 frame%16==0 的落点）。节点字段未能确定 →
// count=0（不发射），仅登记触发时刻与瞄准语义。
ChainNode MakeBlockBNode(std::uint32_t offset) {
    ChainNode node;
    node.trigger_period = kBlockBPeriod;
    node.trigger_tick = offset;
    // 未转写：bullet_type / count / 偏移 / 速度（#11 §4.3 未给出节点字段）→ 全默认值。
    node.spawn_command.count = 0;  // 未能确定 → 占位不发射
    node.aim = AimMode::AtShip;    // getAngleToMyShip [实测：#11 §4.3]
    node.angle_expr = "getAngleToMyShip(enemyX, enemyY)";
    node.offset_expr = "常量 72.0(@0xace230) / double -0.1(@0xace3a0)；节点载荷未能确定";
    node.provenance = "boss_pattern_tables.md §4.3 触发块 B [实测：触发条件与瞄准；"
                      "未能确定：节点 id/count/偏移]（#11 解码样板）";
    return node;
}



// 实测节奏节点（tna_boss21.hpp）：一次触发展开 nodes 个节点、每节点
// count_per_node 发，角度按 angle_step 递减。
ChainNode MakeCadenceNode(const TnaCadenceEntry& entry, std::uint16_t index) {
    ChainNode node;
    node.trigger_period = entry.period;
    node.trigger_tick = entry.phase;
    node.spawn_command.count = static_cast<std::int16_t>(entry.count_per_node);
    node.spawn_command.base_angle_deg = TnaBoss21NodeAngleDeg(entry, index);
    // 弹种与初速来自同一份 .rodata 模板（+0x00 / +0x20）；不填这两项，弹幕会全部
    // 堆在 Boss 点、半径为 0（实测踩过）。
    node.spawn_command.bullet_type = entry.bullet_type;
    node.motion_override.speed = static_cast<double>(entry.speed);
    // 运动函数：原版 `Enemy::createBullets1` @0x37bacc 读 command+0x00（= 本表
    // bullet_type，即节点模式 id），经双跳转表映射出 (w2, w3)，并把这一对作为
    // 参数 2/3 传给每颗子弹的虚初始化（vtable+0x218）[实测：0x37bef0 起]。
    // w2 的取值域（24..59）与 bulletFunc 编号域相容 → 取作运动函数号 [推断]。
    // bullet_type == 0 表示该波模板的弹种字段未定位（4 条 (400,1) 波），
    // 此时不派生（保持 0 = 直线），避免用一个不属于它的函数号改掉运动。
    if (entry.bullet_type > 0) {
        node.motion_override.bullet_func_no = BulletModeForId(entry.bullet_type).w2;
    }
    // 半径：模板里对应 `pf2`（碰撞半径系数）的字段尚未定位，先用引擎直线弹的
    // 默认 4.0px（= 4000 milli，与合成弹幕同口径），保证可判定、可渲染。
    node.motion_override.pf2 = 4.0;
    node.spawn_command.spawn_x = -1.0f;  // 跟随发射者（原版哨兵）
    node.spawn_command.spawn_y = -1.0f;
    node.provenance = "tna_boss21.hpp：节奏点实测（subs+msub）+ 9 次循环 × count=1；"
                      "bullet_func_no 由 BulletModeForId(bullet_type).w2 派生";
    return node;
}

std::vector<BossPhaseTable> BuildLibrary() {
    std::vector<BossPhaseTable> library;

    BossPhaseTable table;
    table.boss_id = "boss21";       // 对应原版 Enemy::setBossBullet21
    table.phase_id = 0;             // 未解码占位（#21 阶段字段逆向表未给出）
    table.node_size_bytes = 0x40;   // 任务书指定假定（#11 实测布局）
    table.source_node_count = 146;  // #21 节点总数 [实测统计]

    // 触发块 A（§4.2）：2 个节点（头 + 载荷）。
    table.nodes.push_back(MakeBlockAHeadNode());
    table.nodes.push_back(MakeBlockAPayloadNode());
    // 触发块 B（§4.3）：4 个触发项（字段未转写）。
    for (std::uint32_t offset : kBlockBOffsets) {
        table.nodes.push_back(MakeBlockBNode(offset));
    }

    library.push_back(std::move(table));

    // 实测节奏表单独一张表（boss_id = "boss21_tna"）：与上面的 #11 抽样样板
    // 互不干扰——样板钉的是「结构」，这张钉的是「实测节奏 + 9 节点循环」。
    BossPhaseTable cadence_table;
    cadence_table.boss_id = "boss21_tna";
    cadence_table.phase_id = 0;
    cadence_table.node_size_bytes = 0x80;
    cadence_table.source_node_count = 146;
    for (const TnaCadenceEntry& entry : kTnaBoss21Cadence) {
        if (entry.count_per_node == 0) {
            continue;  // 未解码条目不发射
        }
        for (std::uint16_t index = 0; index < entry.nodes; ++index) {
            cadence_table.nodes.push_back(MakeCadenceNode(entry, index));
        }
    }
    library.push_back(std::move(cadence_table));

    // state 20 的独立节奏表：与 state 14 互斥，单独注册以便分别 trace 比对。
    BossPhaseTable s20_table;
    s20_table.boss_id = "boss21_tna_s20";
    s20_table.phase_id = 0;
    s20_table.node_size_bytes = 0x80;
    s20_table.source_node_count = 146;
    for (const TnaCadenceEntry& entry : kTnaBoss21CadenceState20) {
        if (entry.count_per_node == 0) {
            continue;
        }
        for (std::uint16_t index = 0; index < entry.nodes; ++index) {
            s20_table.nodes.push_back(MakeCadenceNode(entry, index));
        }
    }
    library.push_back(std::move(s20_table));

    return library;
}

}  // namespace

const std::vector<BossPhaseTable>& BossPhaseLibrary() {
    static const std::vector<BossPhaseTable> kLibrary = BuildLibrary();
    return kLibrary;
}

const BossPhaseTable* FindBossPhaseTable(std::string_view boss_id, std::uint32_t phase_id) {
    for (const BossPhaseTable& table : BossPhaseLibrary()) {
        if (table.boss_id == boss_id && table.phase_id == phase_id) {
            return &table;
        }
    }
    return nullptr;
}

std::vector<const BossPhaseTable*> FindBossPhaseTables(std::string_view boss_id) {
    std::vector<const BossPhaseTable*> out;
    for (const BossPhaseTable& table : BossPhaseLibrary()) {
        if (table.boss_id == boss_id) {
            out.push_back(&table);
        }
    }
    return out;
}

bool HasBossPhaseTable(std::string_view boss_id) {
    return !FindBossPhaseTables(boss_id).empty();
}

namespace {

// 符卡阶段 → 原版 state 段。**整张映射集中在这一处**（校准只改这里）。
//
// 依据（docs/apk-reverse-notes/patterns 与 TNA21_REWRITE_PROPOSAL.md）：
// setBossBullet21 内 state 14（[0x4d6fb8,0x4d89e4)）与 state 20
// （[0x4d9a88,0x4df70c)）是顺序执行的两段。服务端没有原版的 Boss 阶段字段
// （Enemy+0x534，未解码），故用 3 段符卡阶段近似这个进度：
//   phase 0/1（HP 100%~33%）→ state 14 表；phase 2（HP < 33%）→ state 20 表。
// 这是**假设**，不是实测；实测进度推进方式不同的话只改这张表。
struct BossStateSegmentGroup {
    std::string_view base_boss_id;
    std::vector<BossStateSegment> segments;
};

const std::vector<BossStateSegmentGroup>& BossStateSegmentLibrary() {
    static const std::vector<BossStateSegmentGroup> kLibrary = {
        {"boss21_tna", {{"boss21_tna", 0}, {"boss21_tna_s20", 2}}},
    };
    return kLibrary;
}

}  // namespace

const std::vector<BossStateSegment>& BossStateSegments(std::string_view base_boss_id) {
    static const std::vector<BossStateSegment> kNoSegments;
    for (const BossStateSegmentGroup& group : BossStateSegmentLibrary()) {
        if (group.base_boss_id == base_boss_id) {
            return group.segments;
        }
    }
    return kNoSegments;
}

std::string_view BossStateBossIdForPhase(std::string_view base_boss_id, std::uint32_t spell_phase) {
    const std::vector<BossStateSegment>& segments = BossStateSegments(base_boss_id);
    // 默认 = 原样返回：无段表 / 所有段的 from_phase 都大于 spell_phase 时走这里。
    std::string_view resolved = base_boss_id;
    std::uint32_t best_from_phase = 0;
    bool matched = false;
    for (const BossStateSegment& segment : segments) {
        if (segment.from_phase > spell_phase) {
            continue;
        }
        if (!matched || segment.from_phase >= best_from_phase) {
            best_from_phase = segment.from_phase;
            resolved = segment.boss_id;
            matched = true;
        }
    }
    return resolved;
}

bool ChainTablesActive(std::string_view ruleset_version, std::string_view boss_id) {
    if (ruleset_version != kRulesetDanmakuMechanics) {
        return false;
    }
    if (boss_id.empty()) {
        return false;
    }
    return HasBossPhaseTable(boss_id);
}

namespace {

// 单节点展开：触发判定（周期先按 interval_mul 缩放）+ 瞄准覆盖 + EmitRadial。
// triggered=false = 本 tick 未触发（或周期缩放后为 0）；bullets 为空但
// triggered=true = 已触发但 count==0（未转写/头节点，展开 0 颗）。
struct ChainNodeExpansion {
    bool triggered = false;
    std::vector<BulletMotion> bullets;
};

ChainNodeExpansion ExpandChainNode(
    const ChainNode& node,
    std::uint64_t tick,
    const EmitContext& ctx,
    const MotionTargets& targets,
    const DifficultyScale& difficulty
) {
    ChainNodeExpansion out;
    if (node.trigger_period == 0) {
        return out;
    }
    const std::uint32_t period = ScalePeriod(node.trigger_period, difficulty.interval_mul);
    if (period == 0 || tick % period != node.trigger_tick) {
        return out;
    }
    out.triggered = true;
    SpawnCommand cmd = node.spawn_command;
    if (node.aim == AimMode::AtShip && targets.has_my_ship) {
        cmd.base_angle_deg = static_cast<float>(
            AngleToPoint(ctx.emitter_x, ctx.emitter_y, targets.my_ship_x, targets.my_ship_y));
    } else if (node.aim == AimMode::AtEnemy && targets.has_enemy) {
        cmd.base_angle_deg = static_cast<float>(
            AngleToPoint(ctx.emitter_x, ctx.emitter_y, targets.enemy_x, targets.enemy_y));
    }
    out.bullets = EmitRadial(cmd, node.motion_override, ctx, difficulty);
    return out;
}

}  // namespace

std::vector<BulletMotion> EmitChainTableTick(
    const BossPhaseTable& table,
    std::uint64_t tick,
    const EmitContext& ctx,
    const MotionTargets& targets,
    const DifficultyScale& difficulty
) {
    std::vector<BulletMotion> out;
    for (const ChainNode& node : table.nodes) {
        const ChainNodeExpansion expanded = ExpandChainNode(node, tick, ctx, targets, difficulty);
        if (!expanded.triggered) {
            continue;
        }
        out.insert(out.end(), expanded.bullets.begin(), expanded.bullets.end());
    }
    return out;
}

std::vector<BulletMotion> EmitChainBossTick(
    std::string_view boss_id,
    std::uint64_t tick,
    const EmitContext& ctx,
    const MotionTargets& targets,
    const DifficultyScale& difficulty
) {
    std::vector<BulletMotion> out;
    for (const ChainNodeEmission& emission :
             EmitChainBossTickDetailed(boss_id, tick, ctx, targets, difficulty)) {
        out.insert(out.end(), emission.bullets.begin(), emission.bullets.end());
    }
    return out;
}

std::vector<ChainNodeEmission> EmitChainBossTickDetailed(
    std::string_view boss_id,
    std::uint64_t tick,
    const EmitContext& ctx,
    const MotionTargets& targets,
    const DifficultyScale& difficulty
) {
    std::vector<ChainNodeEmission> out;
    const std::vector<const BossPhaseTable*> tables = FindBossPhaseTables(boss_id);
    // chain_id = 节点在合并时间线内的平坦序号（库序 + 表内序），跨表连续计数。
    std::uint32_t node_ordinal = 0;
    for (std::size_t table_index = 0; table_index < tables.size(); ++table_index) {
        const BossPhaseTable& table = *tables[table_index];
        for (std::size_t node_id = 0; node_id < table.nodes.size(); ++node_id) {
            const std::uint32_t chain_id = node_ordinal++;
            const ChainNodeExpansion expanded =
                ExpandChainNode(table.nodes[node_id], tick, ctx, targets, difficulty);
            if (!expanded.triggered) {
                continue;
            }
            ChainNodeEmission emission;
            emission.table_index = static_cast<std::uint32_t>(table_index);
            emission.node_id = static_cast<std::uint32_t>(node_id);
            emission.chain_id = chain_id;
            emission.bullets = std::move(expanded.bullets);
            out.push_back(std::move(emission));
        }
    }
    return out;
}

std::size_t ChainTriggerCountAt(const BossPhaseTable& table, std::uint64_t tick) {
    std::size_t count = 0;
    for (const ChainNode& node : table.nodes) {
        if (node.trigger_period == 0) {
            continue;
        }
        if (tick % node.trigger_period == node.trigger_tick) {
            ++count;
        }
    }
    return count;
}

std::vector<const ChainNode*> FlattenChainNodes(const BossPhaseTable& table) {
    std::vector<const ChainNode*> out;
    // 先序递归（children 展开）；表规模小（抽样 ~10 个），递归深度 = 数据深度。
    struct Walk {
        static void Visit(const ChainNode& node, std::vector<const ChainNode*>* out) {
            out->push_back(&node);
            for (const ChainNode& child : node.children) {
                Visit(child, out);
            }
        }
    };
    for (const ChainNode& node : table.nodes) {
        Walk::Visit(node, &out);
    }
    return out;
}

}  // namespace phk::battle::danmaku
