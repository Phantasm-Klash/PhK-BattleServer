// TNA Boss21 弹幕节奏表单测：验证实测的 (period, phase)、9 节点循环与弹数统计。
//
// 表来源见 include/phk/battle/danmaku/tna_boss21.hpp 的头注释（capstone 实测）。
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "phk/battle/boss_race.hpp"
#include "phk/battle/danmaku/bullet_mode_map.hpp"
#include "phk/battle/danmaku/chain_node.hpp"
#include "phk/battle/danmaku/tna_boss21.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& label) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << label << "\n";
    }
}

using phk::battle::danmaku::kTnaBoss21Cadence;
using phk::battle::danmaku::kTnaBoss21CountPerNode;
using phk::battle::danmaku::kTnaBoss21NodesPerTrigger;
using phk::battle::danmaku::TnaBoss21BulletsAt;
using phk::battle::danmaku::TnaBoss21BulletsInRange;
using phk::battle::danmaku::TnaBoss21FiringCount;
using phk::battle::danmaku::TnaBoss21NodeAngleDeg;
using phk::battle::danmaku::TnaCadenceBullets;
using phk::battle::danmaku::TnaCadenceEntry;
using phk::battle::danmaku::TnaCadenceFires;

void TestTableShape() {
    Check(kTnaBoss21Cadence.size() == 9, "table: 9 measured cadence points (state 14)");
    Check(kTnaBoss21NodesPerTrigger == 9, "table: the emission loop runs nine times");
    Check(kTnaBoss21CountPerNode == 1, "table: each node carries one bullet (tpl+0x14 = 1)");

    std::uint32_t p18 = 0;
    std::uint32_t p400 = 0;
    for (const TnaCadenceEntry& entry : kTnaBoss21Cadence) {
        if (entry.period == 18 && entry.phase == 1) ++p18;
        if (entry.period == 400 && entry.phase == 1) ++p400;
    }
    Check(p18 == 4, "table: four 18-frame sub-cadences (from the 300-cycle blocks)");
    Check(p400 == 4, "table: four (400,1) points");
}

void TestFiringRule() {
    const TnaCadenceEntry p400{400u, 1u, 9, 1, 270.0f, 10.0f, -12.0f};
    Check(!TnaCadenceFires(p400, 0), "fire: tick 0 is before the phase");
    Check(TnaCadenceFires(p400, 1), "fire: tick 1 fires");
    Check(!TnaCadenceFires(p400, 2), "fire: tick 2 does not");
    Check(TnaCadenceFires(p400, 401), "fire: tick 401 fires");
    Check(TnaCadenceFires(p400, 801), "fire: tick 801 fires");

    const TnaCadenceEntry p112{112u, 91u, 9, 0, 270.0f, 10.0f, -12.0f};
    Check(!TnaCadenceFires(p112, 90), "fire: phase 91 not reached at 90");
    Check(TnaCadenceFires(p112, 91), "fire: tick 91 fires");
    Check(TnaCadenceFires(p112, 203), "fire: tick 91+112 fires");
}

void TestBulletCounts() {
    // tick 1：4×9（400 波）+ 6+8+12+12（18 帧子节奏）+ 30（20 帧小波）= 104
    Check(TnaBoss21BulletsAt(1) == 104u, "count: tick 1 emits 104");
    Check(TnaBoss21BulletsAt(2) == 0u, "count: tick 2 emits nothing");
    Check(TnaBoss21BulletsAt(19) == 38u, "count: tick 19 emits the 18-frame sub-cadences");
    Check(TnaBoss21BulletsAt(401) == 66u, "count: tick 401 emits 36 (400-waves) + 30 (20-wave)");
    Check(TnaBoss21BulletsAt(301) == 30u, "count: tick 301 emits the 20-frame wave only");
    Check(TnaBoss21FiringCount(1) == 9, "firing: tick 1 fires nine entries");

    Check(TnaCadenceBullets(kTnaBoss21Cadence[0]) == 9u, "bullets: one wave = 9 nodes x 1");

    // 0..1200：tick 1 / 401 / 801 三次 × 36
    Check(TnaBoss21BulletsInRange(0, 1200) == 4454u, "range: 0..1200 emits 4454");
}

void TestNodeAngles() {
    const TnaCadenceEntry& entry = kTnaBoss21Cadence[0];
    // 基准 270°，每轮 -12°：270, 258, 246 … 174
    Check(TnaBoss21NodeAngleDeg(entry, 0) == 270.0f, "angle: node 0 at 270");
    Check(TnaBoss21NodeAngleDeg(entry, 1) == 258.0f, "angle: node 1 at 258");
    Check(TnaBoss21NodeAngleDeg(entry, 8) == 174.0f, "angle: node 8 at 174");
}

void TestDensityAgainstSynthetic() {
    // 合成 10 模式：每玩家每 450 tick 74 发（12/7/4/2/8/6/5/10/4/16）。
    // 实测节奏表：450 tick 内 1832 发 —— 24.8 倍。
    const std::uint64_t tna = TnaBoss21BulletsInRange(0, 450);
    const std::uint64_t synthetic = 74;
    Check(tna == 1712u, "density: 450 ticks emit 1712 with the measured cadence");
    Check(tna > synthetic * 20u, "density: measured cadence is over twenty times the synthetic rotation");
}


// 实测节奏表已注册为独立的 boss 表（boss21_tna），与 #11 抽样样板互不干扰。
void TestRegisteredTable() {
    using namespace phk::battle::danmaku;
    Check(HasBossPhaseTable("boss21_tna"), "registry: boss21_tna registered");
    const std::vector<const BossPhaseTable*> tables = FindBossPhaseTables("boss21_tna");
    Check(tables.size() == 1, "registry: exactly one cadence table");
    if (tables.size() != 1) {
        return;
    }
    // 4 条已解码的 (400,1) 波 × 9 节点 = 36 个节点
    Check(tables.front()->nodes.size() == 41, "registry: 41 nodes (36 from the 400-waves + 5 single-node waves)");
    // 全部在 tick 1 触发（phase=1）
    // (112,91) 那条要等到 tick 91，故 tick 1 只触发 41 个节点。
    Check(ChainTriggerCountAt(*tables.front(), 1) == 41, "registry: 41 of 42 fire at tick 1");
    Check(ChainTriggerCountAt(*tables.front(), 91) == 4, "registry: tick 91 fires 4 x 18-frame");
    Check(ChainTriggerCountAt(*tables.front(), 2) == 0, "registry: nothing fires at tick 2");
    Check(ChainTriggerCountAt(*tables.front(), 401) == 37, "registry: 37 fire at tick 401 (36 x 400-waves + 20-wave)");
    // 角度 270 → 174，步进 -12
    const std::vector<ChainNode>& nodes = tables.front()->nodes;
    Check(nodes[0].spawn_command.base_angle_deg == 270.0f, "registry: node 0 at 270 degrees");
    Check(nodes[8].spawn_command.base_angle_deg == 174.0f, "registry: node 8 at 174 degrees");
    Check(nodes[0].spawn_command.count == 1, "registry: each node carries one bullet");
}


// 运动函数：原版 createBullets1 @0x37bacc 读 command+0x00（= bullet_type，即模式 id）
// 经双跳转表映射 (w2,w3)，w2 作为每颗子弹的初始化参数 2 → 运动函数号 [推断]。
// 钉住每条已解码波派生出的 func_no；bullet_type==0 的波（模板弹种字段未定位）不派生。
void TestNodeMotionFunctions() {
    using namespace phk::battle::danmaku;
    Check(HasBossPhaseTable("boss21_tna"), "func: cadence table registered");
    const std::vector<const BossPhaseTable*> tables = FindBossPhaseTables("boss21_tna");
    if (tables.size() != 1) {
        Check(false, "func: exactly one cadence table");
        return;
    }
    const std::vector<ChainNode>& nodes = tables.front()->nodes;
    if (nodes.size() != 41) {
        Check(false, "func: 41 nodes expected, got " + std::to_string(nodes.size()));
        return;
    }

    // 前 36 个节点来自 4 条 (400,1) 波：弹种字段未定位（type=0）→ 不派生，仍是直线。
    bool waves_stay_linear = true;
    for (std::size_t i = 0; i < 36; ++i) {
        if (nodes[i].spawn_command.bullet_type != 0 || nodes[i].motion_override.bullet_func_no != 0) {
            waves_stay_linear = false;
        }
    }
    Check(waves_stay_linear, "func: (400,1) waves keep func 0 (their bullet type is unlocated)");

    // 后 6 个单节点波：48/48/39/39/29/172 → 53/53/44/44/45/35。
    const int expected[5] = {53, 53, 44, 44, 45};
    const int kinds[5] = {48, 48, 39, 39, 29};
    for (std::size_t i = 0; i < 5; ++i) {
        const ChainNode& node = nodes[36 + i];
        Check(node.spawn_command.bullet_type == kinds[i],
              "func: node " + std::to_string(36 + i) + " carries bullet type " + std::to_string(kinds[i]));
        Check(node.motion_override.bullet_func_no == expected[i],
              "func: node " + std::to_string(36 + i) + " derives func " + std::to_string(expected[i]) +
                  ", got " + std::to_string(node.motion_override.bullet_func_no));
        // 派生值必须与查表结果一致（防止表被改而测试没跟上）。
        Check(node.motion_override.bullet_func_no == BulletModeForId(kinds[i]).w2,
              "func: node " + std::to_string(36 + i) + " matches BulletModeForId().w2");
    }
}

// 端到端：tna ruleset + boss_id=boss21_tna → tick 1 每玩家 36 颗真实弹幕。
void TestSimulationEmits() {
    phk::battle::BossRaceConfig config;
    config.match_id = "match_tna_cadence_0001";
    config.match_seed = 9001u;
    config.boss_max_hp = 6000;
    config.ruleset_version = "tna-1.40-danmaku";
    config.boss_id = "boss21_tna";
    phk::battle::BossRaceSimulation sim(config);
    sim.AddPlayer("player_a", 0, 60000);
    sim.AddPlayer("player_b", 0, 60000);

    const auto first = sim.Tick();  // tick 1：四个 (400,1) 波各 9 发
    if (first.bullets.size() != 208) { std::cerr << "actual bullets=" << first.bullets.size() << "\n"; }
    Check(first.bullets.size() == 208, "sim: tick 1 spawns 208 bullets (2 players x 104)");
    Check(first.tick == 1, "sim: first snapshot is tick 1");

    const auto second = sim.Tick();  // tick 2：无触发
    Check(second.bullets.size() == 208, "sim: tick 2 spawns nothing new");

    // 快照必须把派生出的运动函数带给客户端（客户端只渲染，不重算判定）。
    // tick 1 共 104 发/玩家：36 发来自 (400,1) 波（弹种未定位 → func 0）、
    // 6+8 = 14 发 func 53（弹种 48）、12+12 = 24 发 func 44（弹种 39）、
    // 30 发 func 45（弹种 29）。
    std::size_t func0 = 0;
    std::size_t func53 = 0;
    std::size_t func44 = 0;
    std::size_t func45 = 0;
    for (const phk::battle::BossRaceBullet& bullet : second.bullets) {
        switch (bullet.motion_func) {
            case 0: ++func0; break;
            case 53: ++func53; break;
            case 44: ++func44; break;
            case 45: ++func45; break;
            default: break;
        }
    }
    Check(func0 == 72, "sim: 72 func0 bullets from the (400,1) waves, got " + std::to_string(func0));
    Check(func53 == 28, "sim: 28 func53 bullets, got " + std::to_string(func53));
    Check(func44 == 48, "sim: 48 func44 bullets, got " + std::to_string(func44));
    Check(func45 == 60, "sim: 60 func45 bullets, got " + std::to_string(func45));

    // 推进到 tick 19：四条 18 帧子节奏再发一轮（6+8 func53、12+12 func44），
    // 20 帧小波不触发（19 % 20 != 1）。累计翻倍。
    phk::battle::BossRaceSnapshot at19;
    for (std::uint64_t tick = 3; tick <= 19; ++tick) {
        at19 = sim.Tick();
    }
    std::size_t func53_at19 = 0;
    std::size_t func44_at19 = 0;
    for (const phk::battle::BossRaceBullet& bullet : at19.bullets) {
        if (bullet.motion_func == 53) ++func53_at19;
        if (bullet.motion_func == 44) ++func44_at19;
    }
    Check(func53_at19 == 56, "sim: tick 19 accumulates 56 func53 bullets, got " + std::to_string(func53_at19));
    Check(func44_at19 == 96, "sim: tick 19 accumulates 96 func44 bullets, got " + std::to_string(func44_at19));
}


// state 20（[0x4d9a88, 0x4df70c)）独立表：钉住条目数、无 state-14 泄漏、注册节点数、tick 10 弹数。
void TestState20Table() {
    using namespace phk::battle::danmaku;
    Check(kTnaBoss21CadenceState20.size() == 16, "s20: 16 measured gates");
    for (const TnaCadenceEntry& entry : kTnaBoss21CadenceState20) {
        Check(entry.count_per_node > 0, "s20: every gate carries a decoded count");
        Check(entry.period != 112u && entry.period != 120u,
              "s20: no state-14 gate leaks into the state-20 table");
    }
    Check(HasBossPhaseTable("boss21_tna_s20"), "s20: table registered");
    const std::vector<const BossPhaseTable*> tables = FindBossPhaseTables("boss21_tna_s20");
    Check(tables.size() == 1, "s20: exactly one table");
    if (!tables.empty()) {
        Check(tables.front()->nodes.size() == 16, "s20: 16 nodes (one per gate)");
    }
    // tick 10：六条 %10 门同时命中（12+20+12+15+10+24）。
    Check(TnaBoss21State20BulletsAt(10) == 93u, "s20: tick 10 emits 93");
    // tick 1：没有任何门的周期整除 1。
    Check(TnaBoss21State20BulletsAt(1) == 0u, "s20: tick 1 emits nothing");
}

}  // namespace

int main() {
    TestTableShape();
    TestFiringRule();
    TestBulletCounts();
    TestNodeAngles();
    TestDensityAgainstSynthetic();
    TestRegisteredTable();
    TestNodeMotionFunctions();
    TestSimulationEmits();
    TestState20Table();

    if (g_failures != 0) {
        std::cerr << g_failures << " tna cadence test(s) failed\n";
        return 1;
    }
    std::cout << "tna cadence tests passed\n";
    return 0;
}
