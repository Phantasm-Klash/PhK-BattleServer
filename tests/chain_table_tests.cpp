// ChainNode 表路径的「发射数量 / 发射时刻」钉帧测试（计划 v2 P3 验收）。
//
// 覆盖对象：`danmaku::EmitChainBossTickDetailed`（纯函数）与
// `BossRaceSimulation` 的链表接线（快照 chains[] + bullets）。
//
// 数据表现状（src/danmaku/chain_node.cpp）：boss21 只有 6 个已转写节点，
// 平坦序号 chain_id 0..5：
//   0 = 触发块 A 头节点   period 300, count 0  → 触发但 0 颗
//   1 = 触发块 A 载荷节点 period 300, count 1  → 每玩家 1 颗
//   2..5 = 触发块 B       period 200, offset 0/16/32/48, count 0 → 触发但 0 颗
//
// 注意 tick 0 同时命中块 A（300 的倍数）与块 B offset 0，故 tick 0 = {0,1,2}/1 颗。
// tick 600 = 300 与 200 的公倍数 → {0,1,2}/1 颗。这些时刻/数量就是本测试的钉子：
// 表数值若被改动（哪怕只改 period 或 count），这里会立刻红。
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "phk/battle/boss_race.hpp"
#include "phk/battle/danmaku/chain_node.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (condition) {
        return;
    }
    std::cerr << "FAIL: " << message << "\n";
    ++g_failures;
}

phk::battle::BossRaceConfig ChainConfig(std::int32_t difficulty = 1) {
    phk::battle::BossRaceConfig config;
    config.match_id = "match_chain_tests_000001";
    config.match_seed = 777u;
    config.boss_max_hp = 1000000;
    config.ruleset_version = phk::battle::kRulesetDanmakuMechanics;
    config.boss_id = "boss21";
    config.difficulty = difficulty;
    return config;
}

// 纯函数展开结果：本 tick 触发的节点平坦序号 + 展开的子弹总数（单玩家副本）。
struct TickTrace {
    std::vector<std::uint32_t> chain_ids;
    std::size_t bullets = 0;
};

TickTrace EmitAt(std::uint64_t tick, std::int32_t difficulty = 1) {
    phk::battle::danmaku::EmitContext ctx;
    ctx.emitter_x = 240.0;
    ctx.emitter_y = 80.0;
    phk::battle::danmaku::MotionTargets targets;
    targets.has_my_ship = true;
    targets.my_ship_x = 200.0;
    targets.my_ship_y = 600.0;
    TickTrace trace;
    const std::vector<phk::battle::danmaku::ChainNodeEmission> emissions =
        phk::battle::danmaku::EmitChainBossTickDetailed(
            "boss21", tick, ctx, targets, phk::battle::danmaku::DifficultyScaleFor(difficulty));
    for (const phk::battle::danmaku::ChainNodeEmission& emission : emissions) {
        trace.chain_ids.push_back(emission.chain_id);
        trace.bullets += emission.bullets.size();
    }
    return trace;
}

// 1) 表结构：6 个已转写节点、count 与 period 逐项钉死。
void TestTableShape() {
    using namespace phk::battle::danmaku;
    Check(HasBossPhaseTable("boss21"), "table: boss21 registered");
    const std::vector<const BossPhaseTable*> tables = FindBossPhaseTables("boss21");
    Check(tables.size() == 1, "table: exactly one phase table for boss21");
    if (tables.size() != 1) {
        return;
    }
    const std::vector<ChainNode>& nodes = tables.front()->nodes;
    Check(nodes.size() == 6, "table: 6 transcribed nodes (A head + A payload + 4 B slots)");
    if (nodes.size() != 6) {
        return;
    }
    Check(nodes[0].trigger_period == 300 && nodes[0].trigger_tick == 0 &&
              nodes[0].spawn_command.count == 0,
          "table: node 0 = block A head (period 300, count 0)");
    Check(nodes[1].trigger_period == 300 && nodes[1].trigger_tick == 0 &&
              nodes[1].spawn_command.count == 1,
          "table: node 1 = block A payload (period 300, count 1)");
    const std::uint32_t block_b_offsets[4] = {0, 16, 32, 48};
    for (std::size_t i = 0; i < 4; ++i) {
        const ChainNode& node = nodes[2 + i];
        Check(node.trigger_period == 200 && node.trigger_tick == block_b_offsets[i] &&
                  node.spawn_command.count == 0,
              "table: block B slot " + std::to_string(i) + " (period 200, offset " +
                  std::to_string(block_b_offsets[i]) + ", count 0)");
    }
}

// 2) 纯函数：每个时刻触发哪些链、展开几颗。
void TestEmissionSchedule() {
    struct ExpectedTick {
        std::uint64_t tick;
        std::vector<std::uint32_t> chain_ids;
        std::size_t bullets;
    };
    // 块 B 的 200 窗内落点 = {0,16,32,48}；count 0 → 0 颗，但链记录必须存在。
    // 块 A 的 300 周期：头节点 0 颗 + 载荷节点 1 颗。
    const std::vector<ExpectedTick> expectations = {
        {0, {0, 1, 2}, 1},    // 块 A 与块 B offset 0 同时命中
        {16, {3}, 0},
        {32, {4}, 0},
        {48, {5}, 0},
        {200, {2}, 0},
        {216, {3}, 0},
        {232, {4}, 0},
        {248, {5}, 0},
        {300, {0, 1}, 1},     // 块 A：头节点不发、载荷节点每玩家 1 颗
        {400, {2}, 0},
        {416, {3}, 0},
        {432, {4}, 0},
        {448, {5}, 0},
        {600, {0, 1, 2}, 1},  // 300 与 200 的公倍数
        // 非触发 tick：无链记录、无弹。
        {1, {}, 0},
        {15, {}, 0},
        {45, {}, 0},
        {100, {}, 0},
        {299, {}, 0},
        {301, {}, 0},
        {599, {}, 0},
        {601, {}, 0},
        {1000, {2}, 0},       // 1000 % 200 == 0 → 块 B offset 0
    };
    for (const ExpectedTick& expected : expectations) {
        const TickTrace trace = EmitAt(expected.tick);
        Check(trace.chain_ids == expected.chain_ids,
              "schedule: tick " + std::to_string(expected.tick) + " chain ids");
        Check(trace.bullets == expected.bullets,
              "schedule: tick " + std::to_string(expected.tick) + " bullet count = " +
                  std::to_string(expected.bullets) + " (got " + std::to_string(trace.bullets) + ")");
    }
}

// 3) 模拟接线：快照 chains[] 的字段语义 + 每玩家弹数。
void TestSimulationWiring() {
    phk::battle::BossRaceSimulation sim(ChainConfig());
    sim.AddPlayer("player_a", 0, 60000);
    sim.AddPlayer("player_b", 0, -60000);

    std::vector<std::uint64_t> chain_ticks;
    std::vector<std::uint64_t> bullet_growth_ticks;
    std::size_t previous_bullets = 0;
    for (std::uint64_t tick = 1; tick <= 620; ++tick) {
        const phk::battle::BossRaceSnapshot snapshot = sim.Tick();
        if (!snapshot.chains.empty()) {
            chain_ticks.push_back(snapshot.tick);
        }
        if (snapshot.bullets.size() != previous_bullets) {
            bullet_growth_ticks.push_back(snapshot.tick);
            previous_bullets = snapshot.bullets.size();
        }
    }

    // 触发时刻集合（块 A 300/600；块 B 每 200 的 0/16/32/48，含 600 与 616）。
    const std::vector<std::uint64_t> expected_chain_ticks = {
        16, 32, 48, 200, 216, 232, 248, 300, 400, 416, 432, 448, 600, 616};
    Check(chain_ticks == expected_chain_ticks, "sim: chain record ticks pinned");
    // 只有块 A 的载荷节点真的发弹 → 弹数只在 300 / 600 增长。
    const std::vector<std::uint64_t> expected_growth_ticks = {300, 600};
    Check(bullet_growth_ticks == expected_growth_ticks, "sim: bullets grow only on block A ticks");
    // 2 名玩家 × 每玩家 2 颗（300 与 600 各 1 颗）。
    Check(previous_bullets == 4, "sim: 4 live bullets after 620 ticks (2 players x 2)");
    std::size_t player_a_bullets = 0;
    std::size_t player_b_bullets = 0;
    for (const phk::battle::BossRaceBullet& bullet : sim.Snapshot().bullets) {
        if (bullet.owner_player_id == "player_a") {
            ++player_a_bullets;
        } else if (bullet.owner_player_id == "player_b") {
            ++player_b_bullets;
        }
    }
    Check(player_a_bullets == 2, "sim: player_a got exactly 2 bullets");
    Check(player_b_bullets == 2, "sim: player_b got exactly 2 bullets");

    // 快照 chains[] 字段：chain_id/node_id 为真实值，rng_cursor = 该链本 tick 展开数。
    phk::battle::BossRaceSimulation sim600(ChainConfig());
    sim600.AddPlayer("player_a", 0, 60000);
    sim600.AddPlayer("player_b", 0, -60000);
    while (sim600.CurrentTick() < 600) {
        sim600.Tick();
    }
    const phk::battle::BossRaceSnapshot snapshot600 = sim600.Snapshot();
    Check(snapshot600.chains.size() == 3, "sim: tick 600 has 3 chain records (A head, A payload, B0)");
    if (snapshot600.chains.size() == 3) {
        Check(snapshot600.chains[0].chain_id == 0 && snapshot600.chains[0].node_id == 0 &&
                  snapshot600.chains[0].rng_cursor == 0,
              "sim: chain 0 = block A head, no bullets");
        Check(snapshot600.chains[1].chain_id == 1 && snapshot600.chains[1].node_id == 1 &&
                  snapshot600.chains[1].rng_cursor == 1,
              "sim: chain 1 = block A payload, 1 bullet");
        Check(snapshot600.chains[2].chain_id == 2 && snapshot600.chains[2].node_id == 2 &&
                  snapshot600.chains[2].rng_cursor == 0,
              "sim: chain 2 = block B offset 0, untranscribed (0 bullets)");
    }
}

// 4) 难度 interval 通道：块 A 周期 300 → llround(300×0.80)=240（难度 3）。
void TestChainDifficultyPeriodScaling() {
    const TickTrace at240 = EmitAt(240, 3);
    Check(at240.chain_ids == std::vector<std::uint32_t>({0, 1}) && at240.bullets == 1,
          "difficulty: lunatic scales block A period to 240 (fires at 240)");
    const TickTrace at300 = EmitAt(300, 3);
    Check(at300.chain_ids.empty() && at300.bullets == 0,
          "difficulty: lunatic no longer fires block A at 300");
    // 块 B 周期 200 → 160；offset 保持不变。
    const TickTrace block_b160 = EmitAt(160, 3);
    Check(block_b160.chain_ids == std::vector<std::uint32_t>({2}) && block_b160.bullets == 0,
          "difficulty: lunatic scales block B period to 160 (offset 0 fires at 160)");
    // 难度 0：300 → llround(300×1.15)=345。
    const TickTrace at345 = EmitAt(345, 0);
    Check(at345.chain_ids == std::vector<std::uint32_t>({0, 1}) && at345.bullets == 1,
          "difficulty: easy scales block A period to 345");
}

}  // namespace

int main() {
    TestTableShape();
    TestEmissionSchedule();
    TestSimulationWiring();
    TestChainDifficultyPeriodScaling();

    if (g_failures != 0) {
        std::cerr << g_failures << " chain table test(s) failed\n";
        return 1;
    }
    std::cout << "chain table tests passed\n";
    return 0;
}
