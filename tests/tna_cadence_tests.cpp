// TNA Boss21 弹幕节奏表单测：验证实测的 (period, phase)、9 节点循环与弹数统计。
//
// 表来源见 include/phk/battle/danmaku/tna_boss21.hpp 的头注释（capstone 实测）。
#include <cstdint>
#include <iostream>
#include <string>

#include "phk/battle/boss_race.hpp"
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
    Check(kTnaBoss21Cadence.size() == 11, "table: 11 measured cadence points");
    Check(kTnaBoss21NodesPerTrigger == 9, "table: the emission loop runs nine times");
    Check(kTnaBoss21CountPerNode == 1, "table: each node carries one bullet (tpl+0x14 = 1)");

    std::uint32_t p300 = 0;
    std::uint32_t p400 = 0;
    for (const TnaCadenceEntry& entry : kTnaBoss21Cadence) {
        if (entry.period == 300 && entry.phase == 1) ++p300;
        if (entry.period == 400 && entry.phase == 1) ++p400;
    }
    Check(p300 == 4, "table: four (300,1) points");
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
    // tick 1：四个 (400,1) 各 9 发 = 36；(300,1) 的 count 未解码 → 不发。
    Check(TnaBoss21BulletsAt(1) == 36u, "count: tick 1 emits 36 (four waves x 9 nodes)");
    Check(TnaBoss21BulletsAt(2) == 0u, "count: tick 2 emits nothing");
    Check(TnaBoss21BulletsAt(401) == 36u, "count: tick 401 emits 36");
    Check(TnaBoss21BulletsAt(301) == 0u, "count: tick 301 emits nothing (300-waves undecoded)");
    Check(TnaBoss21FiringCount(1) == 4, "firing: tick 1 fires four entries");

    Check(TnaCadenceBullets(kTnaBoss21Cadence[0]) == 9u, "bullets: one wave = 9 nodes x 1");

    // 0..1200：tick 1 / 401 / 801 三次 × 36
    Check(TnaBoss21BulletsInRange(0, 1200) == 36u * 3u, "range: 0..1200 emits 108");
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
    // 实测节奏表目前只解出 4 条 (400,1) 波 → 450 tick 内 72 发。
    const std::uint64_t tna = TnaBoss21BulletsInRange(0, 450);
    const std::uint64_t synthetic = 74;
    Check(tna == 72u, "density: 450 ticks emit 72 with the decoded waves");
    // 其余 7 条波未解码，故当前密度与合成模式同量级——差距在未解码条目，不在算法。
    Check(tna <= synthetic * 2u, "density: decoded subset stays in the synthetic order of magnitude");
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
    Check(tables.front()->nodes.size() == 36, "registry: 36 nodes (four waves x nine steps)");
    // 全部在 tick 1 触发（phase=1）
    Check(ChainTriggerCountAt(*tables.front(), 1) == 36, "registry: all 36 fire at tick 1");
    Check(ChainTriggerCountAt(*tables.front(), 2) == 0, "registry: nothing fires at tick 2");
    Check(ChainTriggerCountAt(*tables.front(), 401) == 36, "registry: all 36 fire at tick 401");
    // 角度 270 → 174，步进 -12
    const std::vector<ChainNode>& nodes = tables.front()->nodes;
    Check(nodes[0].spawn_command.base_angle_deg == 270.0f, "registry: node 0 at 270 degrees");
    Check(nodes[8].spawn_command.base_angle_deg == 174.0f, "registry: node 8 at 174 degrees");
    Check(nodes[0].spawn_command.count == 1, "registry: each node carries one bullet");
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
    if (first.bullets.size() != 72) { std::cerr << "actual bullets=" << first.bullets.size() << " tick=" << first.tick << " state=" << static_cast<int>(first.state) << "\n"; }
    Check(first.bullets.size() == 72, "sim: tick 1 spawns 72 bullets (2 players x 36)");
    Check(first.tick == 1, "sim: first snapshot is tick 1");

    const auto second = sim.Tick();  // tick 2：无触发
    Check(second.bullets.size() == 72, "sim: tick 2 spawns nothing new");
}

}  // namespace

int main() {
    TestTableShape();
    TestFiringRule();
    TestBulletCounts();
    TestNodeAngles();
    TestDensityAgainstSynthetic();
    TestRegisteredTable();
    TestSimulationEmits();

    if (g_failures != 0) {
        std::cerr << g_failures << " tna cadence test(s) failed\n";
        return 1;
    }
    std::cout << "tna cadence tests passed\n";
    return 0;
}
