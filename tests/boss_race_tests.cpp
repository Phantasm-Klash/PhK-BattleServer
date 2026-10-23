// Tests for the MVP "mvp_boss_race" simulation: two players, identical Boss
// copies, ten deterministic bullet patterns, first-to-defeat wins.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

#include "phk/battle/boss_race.hpp"
#include "phk/battle/coop_boss.hpp"  // 64 tick 基准断言要读 coop 常量

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& label) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << label << "\n";
    }
}

phk::battle::BattleInput MakeInput(
    const std::string& match_id,
    const std::string& player_id,
    std::uint64_t tick,
    std::uint64_t seq,
    bool shoot,
    std::uint32_t direction_bits = 0
) {
    phk::battle::BattleInput input;
    input.match_id = match_id;
    input.player_id = player_id;
    input.tick = tick;
    input.seq = seq;
    input.shoot = shoot;
    input.direction_bits = direction_bits;
    return input;
}

phk::battle::BossRaceConfig MakeConfig(std::uint64_t seed, std::uint64_t boss_hp) {
    phk::battle::BossRaceConfig config;
    config.match_id = "match_mvp_000001";
    config.match_seed = seed;
    config.boss_max_hp = boss_hp;
    return config;
}

void TestPatternCatalog() {
    const auto& ids = phk::battle::BossRacePatternIds();
    Check(ids.size() == 10, "pattern catalog has ten entries");
    Check(ids.front() == "kome_ring", "first pattern is kome_ring");
    Check(ids.back() == "chou_four_dir", "last pattern is chou_four_dir");
}

void TestRealBulletsCarryKindAndMotion() {
    auto sim = phk::battle::BossRaceSimulation(MakeConfig(1234567u, 1000000));
    Check(sim.AddPlayer("player_a", -20000, 60000), "add player_a for real bullets");
    Check(sim.AddPlayer("player_b", 20000, 60000), "add player_b for real bullets");

    std::uint64_t seq = 0;
    bool saw_real_bullet = false;
    for (std::uint64_t tick = 1; tick <= 120; ++tick) {
        Check(sim.SubmitInput(MakeInput(sim.Config().match_id, "player_a", tick, ++seq, false)), "input a");
        Check(sim.SubmitInput(MakeInput(sim.Config().match_id, "player_b", tick, ++seq, false)), "input b");
        const auto snapshot = sim.Tick();
        for (const auto& bullet : snapshot.bullets) {
            // 真实弹种：kEnemyBullet 值域 9..224。
            if (bullet.bullet_kind >= 9 && bullet.bullet_kind <= 224) {
                saw_real_bullet = true;
            }
            Check(bullet.bullet_kind >= 9 && bullet.bullet_kind <= 224, "bullet kind is a real kEnemyBullet value");
            Check(!bullet.pattern_id.empty(), "bullet carries pattern id");
        }
    }
    Check(saw_real_bullet, "real bullets were emitted with valid kinds");
}

void TestFirstToDefeatWins() {
    auto sim = phk::battle::BossRaceSimulation(MakeConfig(20260625u, 300));
    Check(sim.AddPlayer("player_a", -20000, 60000), "add player_a");
    Check(sim.AddPlayer("player_b", 20000, 60000), "add player_b");
    Check(!sim.AddPlayer("player_c", 0, 0), "third player rejected");

    std::uint64_t seq_a = 0;
    std::uint64_t seq_b = 0;
    for (std::uint64_t tick = 1; tick <= 200 && sim.State() != phk::battle::BossRaceState::Finished; ++tick) {
        // player_a shoots every tick, player_b never shoots.
        Check(sim.SubmitInput(MakeInput(sim.Config().match_id, "player_a", tick, ++seq_a, true)), "submit player_a input");
        Check(sim.SubmitInput(MakeInput(sim.Config().match_id, "player_b", tick, ++seq_b, false)), "submit player_b input");
        sim.Tick();
    }

    Check(sim.State() == phk::battle::BossRaceState::Finished, "race finished");
    Check(sim.WinnerPlayerId() == "player_a", "shooter wins the race");

    const auto snapshot = sim.Snapshot();
    Check(snapshot.players.size() == 2, "snapshot has two players");
    bool found_a_zero = false;
    bool found_b_intact = false;
    for (const auto& player : snapshot.players) {
        if (player.player_id == "player_a") {
            found_a_zero = player.boss_current_hp == 0 && player.damage_dealt == 300;
        }
        if (player.player_id == "player_b") {
            found_b_intact = player.boss_current_hp == 300 && player.damage_dealt == 0;
        }
    }
    Check(found_a_zero, "player_a boss defeated and damage accounted");
    Check(found_b_intact, "player_b boss untouched");
}

void TestDeterministicBullets() {
    auto sim_a = phk::battle::BossRaceSimulation(MakeConfig(424242u, 100000));
    auto sim_b = phk::battle::BossRaceSimulation(MakeConfig(424242u, 100000));
    Check(sim_a.AddPlayer("player_a", 0, 60000), "add a to sim_a");
    Check(sim_b.AddPlayer("player_a", 0, 60000), "add a to sim_b");
    Check(sim_a.AddPlayer("player_b", 20000, 60000), "add b to sim_a");
    Check(sim_b.AddPlayer("player_b", 20000, 60000), "add b to sim_b");

    std::uint64_t seq = 0;
    std::size_t peak_bullets = 0;
    for (std::uint64_t tick = 1; tick <= 300; ++tick) {
        Check(sim_a.SubmitInput(MakeInput(sim_a.Config().match_id, "player_a", tick, ++seq, false)), "sim_a input");
        peak_bullets = std::max(peak_bullets, sim_a.Tick().bullets.size());
        Check(sim_b.SubmitInput(MakeInput(sim_b.Config().match_id, "player_a", tick, ++seq, false)), "sim_b input");
        sim_b.Tick();
    }

    Check(sim_a.Snapshot().state_hash == sim_b.Snapshot().state_hash, "same seed produces identical state hash");
    Check(sim_a.Snapshot().bullets.size() == sim_b.Snapshot().bullets.size(), "same seed produces identical bullet count");
    Check(peak_bullets > 0, "bullets were emitted");
}

void TestPatternSchedulerCoversTenPatterns() {
    auto sim = phk::battle::BossRaceSimulation(MakeConfig(7u, 100000));
    Check(sim.AddPlayer("player_a", 0, 60000), "add player for pattern check");
    const std::uint32_t period = sim.Config().pattern_period_ticks;

    std::string seen = ",";
    for (std::uint32_t index = 0; index < 10; ++index) {
        const std::string id = sim.PatternIdForTick(static_cast<std::uint64_t>(period) * (index + 1));
        Check(seen.find("," + id + ",") == std::string::npos, "pattern id unique: " + id);
        seen += id + ",";
    }
}

// ---- 线程A「游戏内容」：boss 目录 + 自机 loadout ----------------------------

void TestBossCatalogTimelineExpansion() {
    phk::battle::BossRaceConfig config = MakeConfig(2026u, 0);
    config.boss_id = "tian";
    auto sim = phk::battle::BossRaceSimulation(config);

    const auto* entry = sim.BossEntry();
    Check(entry != nullptr, "tian resolves to a boss catalog entry");
    if (entry != nullptr) {
        Check(entry->id == "tian", "boss entry id is tian");
        Check(entry->boss_max_hp == 20000, "tian hp comes from the catalog");
    }
    Check(sim.Config().boss_max_hp == 20000, "simulation adopts the catalog boss hp");

    const std::uint32_t period = sim.Config().pattern_period_ticks;
    // 阶段组展开：kome_ring x4 → arrow_aimed_fan x3 → ring_spiral x4 →
    // kome_homing x3 → chou_four_dir x4（总 18 波），随后循环回波 0。
    Check(sim.PatternIdForTick(period * 1) == "kome_ring", "wave 1 → kome_ring");
    Check(sim.PatternIdForTick(period * 4) == "arrow_aimed_fan", "wave 4 → arrow_aimed_fan");
    Check(sim.PatternIdForTick(period * 7) == "ring_spiral", "wave 7 → ring_spiral");
    Check(sim.PatternIdForTick(period * 11) == "kome_homing", "wave 11 → kome_homing");
    Check(sim.PatternIdForTick(period * 14) == "chou_four_dir", "wave 14 → chou_four_dir");
    Check(sim.PatternIdForTick(period * 18) == "kome_ring", "wave 18 loops back to kome_ring");
    // 越界回退：极大 tick 仍落在库内（不越界）。
    const std::string far = sim.PatternIdForTick(period * 100000ull);
    Check(!far.empty(), "far tick still resolves to a library pattern id");
}

void TestBossEmitterOffsetTriangular() {
    using namespace phk::battle::danmaku;
    const BossCatalogEntry* tian = FindBossCatalogEntry("tian");
    Check(tian != nullptr, "tian catalog entry present");
    if (tian == nullptr) {
        return;
    }
    std::int32_t x = 0;
    std::int32_t y = 0;
    BossEmitterOffsetMilli(tian->movement, 0, &x, &y);
    Check(x == -60000 && y == 0, "tian tick 0 → -amplitude on x");
    BossEmitterOffsetMilli(tian->movement, 300, &x, &y);
    Check(x == 60000 && y == 0, "tian tick half → +amplitude on x");
    BossEmitterOffsetMilli(tian->movement, 600, &x, &y);
    Check(x == -60000 && y == 0, "tian tick period → -amplitude on x");
    BossEmitterOffsetMilli(tian->movement, 150, &x, &y);
    Check(x == 0 && y == 0, "tian tick half/2 → 0 offset");
    // 整数整除精确：(2*60000)/300 = 400。
    BossEmitterOffsetMilli(tian->movement, 75, &x, &y);
    Check(x == -30000, "tian tick 75 → -30000");

    const BossCatalogEntry* hui = FindBossCatalogEntry("hui");
    Check(hui != nullptr, "hui catalog entry present");
    if (hui != nullptr) {
        BossEmitterOffsetMilli(hui->movement, 240, &x, &y);
        Check(x == 0 && y == 30000, "hui is vertical (bob_y)");
    }
    BossMovementSpec still;
    BossEmitterOffsetMilli(still, 123, &x, &y);
    Check(x == 0 && y == 0, "static movement yields zero offset");
}

void TestBossSelectionIsDeterministicAndDistinct() {
    phk::battle::BossRaceConfig tian_cfg = MakeConfig(97531u, 0);
    tian_cfg.boss_id = "tian";
    phk::battle::BossRaceConfig hui_cfg = MakeConfig(97531u, 0);
    hui_cfg.boss_id = "hui";
    phk::battle::BossRaceConfig default_cfg = MakeConfig(97531u, 100000);

    auto run = [](phk::battle::BossRaceConfig config) {
        auto sim = phk::battle::BossRaceSimulation(config);
        sim.AddPlayer("player_a", -20000, 60000);
        sim.AddPlayer("player_b", 20000, 60000);
        std::uint64_t seq = 0;
        for (std::uint64_t tick = 1; tick <= 400; ++tick) {
            sim.SubmitInput(MakeInput(sim.Config().match_id, "player_a", tick, ++seq, false));
            sim.SubmitInput(MakeInput(sim.Config().match_id, "player_b", tick, ++seq, false));
            sim.Tick();
        }
        return sim.Snapshot().state_hash;
    };

    const std::string tian_a = run(tian_cfg);
    const std::string tian_b = run(tian_cfg);
    Check(tian_a == tian_b, "same boss + seed → identical state hash");
    Check(tian_a != run(hui_cfg), "tian and hui produce different states");
    Check(tian_a != run(default_cfg), "boss timeline differs from the default scheduler");
}

void TestUnknownBossFallsBackToDefault() {
    phk::battle::BossRaceConfig unknown = MakeConfig(4242u, 100000);
    unknown.boss_id = "bogus";
    auto sim = phk::battle::BossRaceSimulation(unknown);
    Check(sim.BossEntry() == nullptr, "unknown boss falls back to the default timeline");
    Check(sim.Config().boss_max_hp == 100000, "unknown boss keeps the configured hp");

    phk::battle::BossRaceConfig plain = MakeConfig(4242u, 100000);
    auto sim_plain = phk::battle::BossRaceSimulation(plain);
    Check(sim.PatternIdForTick(45ull * 7) == sim_plain.PatternIdForTick(45ull * 7), "unknown boss == default scheduler");
}

void TestPlayerLoadoutSelectsShotAndBomb() {
    phk::battle::BossRaceConfig config = MakeConfig(112233u, 1000000);
    phk::battle::PlayerLoadoutSpec loadout;
    loadout.player_id = "player_a";
    loadout.character_id = "wide";
    loadout.shot_type = "ReimuA1";  // damage 50
    loadout.bomb_type = "Reimu";    // damage 45
    config.loadouts.push_back(loadout);
    auto sim = phk::battle::BossRaceSimulation(config);
    Check(sim.AddPlayer("player_a", 0, 60000), "add loaded player_a");
    Check(sim.AddPlayer("player_b", 20000, 60000), "add default player_b");

    const auto* shot = sim.PlayerShotSpecFor("player_a");
    Check(shot != nullptr && shot->name == std::string("ReimuA1"), "player_a resolves ReimuA1");
    Check(sim.PlayerShotSpecFor("player_b") == nullptr, "player_b has no shot spec");
    const auto* bomb = sim.PlayerBombSpecFor("player_a");
    Check(bomb != nullptr && bomb->name == std::string("Reimu"), "player_a resolves Reimu bomb");

    // tick 1：a 用 ReimuA1(50)，b 用默认(10)。
    sim.SubmitInput(MakeInput(sim.Config().match_id, "player_a", 1, 1, true));
    sim.SubmitInput(MakeInput(sim.Config().match_id, "player_b", 1, 1, true));
    sim.Tick();
    std::uint64_t damage_a = 0;
    std::uint64_t damage_b = 0;
    for (const auto& player : sim.Snapshot().players) {
        if (player.player_id == "player_a") damage_a = player.damage_dealt;
        if (player.player_id == "player_b") damage_b = player.damage_dealt;
    }
    Check(damage_a == 50, "loaded shot deals the catalog damage (50)");
    Check(damage_b == 10, "unloaded shot keeps the default damage (10)");

    // tick 2：a 不使用射击但触发炸弹（Reimu bomb damage 45）。
    auto bomb_input = MakeInput(sim.Config().match_id, "player_a", 2, 2, false);
    bomb_input.bomb = true;
    sim.SubmitInput(bomb_input);
    sim.SubmitInput(MakeInput(sim.Config().match_id, "player_b", 2, 2, false));
    sim.Tick();
    for (const auto& player : sim.Snapshot().players) {
        if (player.player_id == "player_a") damage_a = player.damage_dealt;
    }
    Check(damage_a == 95, "loaded bomb adds the catalog bomb damage (45)");
}

void TestInputTickWindowOverflowGuard() {
    auto sim = phk::battle::BossRaceSimulation(MakeConfig(9u, 100000));
    Check(sim.AddPlayer("player_a", 0, 60000), "add player for overflow check");
    auto input = MakeInput(
        sim.Config().match_id,
        "player_a",
        std::numeric_limits<std::uint64_t>::max(),
        1,
        false
    );
    Check(!sim.SubmitInput(input), "reject far-future max tick");
}

// ---- 黄金向量导出（线程A A6） ------------------------------------------------
//
// 生成方向：C++ 为权威，导出 JSON；TS 读同一 fixture 重放并逐位断言。
// 触发：环境变量 PHK_BOSS_RACE_FIXTURE=<输出路径> 运行时导出（不参与断言）。
//
// 固定输入：match_seed=0x0B05B0SS，boss=tian，player_a 用 ReimuA1/Reimu 自机、
// tick>=10 起每 tick 射击；player_b 默认自机、从不射击。确定性保证双端一致。
void WriteGoldenFixture(const std::string& path) {
    phk::battle::BossRaceConfig config;
    config.match_id = "golden_boss_race";
    config.match_seed = 987654321ull;
    config.boss_max_hp = 0;  // 目录 HP（tian = 20000）
    config.boss_id = "tian";
    config.ruleset_version = "tna-1.40";
    config.pattern_period_ticks = 45;
    phk::battle::PlayerLoadoutSpec loadout;
    loadout.player_id = "player_a";
    loadout.character_id = "wide";
    loadout.shot_type = "ReimuA1";
    loadout.bomb_type = "Reimu";
    config.loadouts.push_back(loadout);

    auto sim = phk::battle::BossRaceSimulation(config);
    sim.AddPlayer("player_a", -20000, 60000);
    sim.AddPlayer("player_b", 20000, 60000);

    const std::uint64_t tick_count = 240;
    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (out == nullptr) {
        std::cerr << "cannot open fixture path: " << path << "\n";
        return;
    }
    std::fprintf(out, "{\n");
    std::fprintf(out, "  \"matchId\": \"%s\",\n", config.match_id.c_str());
    std::fprintf(out, "  \"matchSeed\": %llu,\n", static_cast<unsigned long long>(config.match_seed));
    std::fprintf(out, "  \"bossId\": \"%s\",\n", config.boss_id.c_str());
    std::fprintf(out, "  \"rulesetVersion\": \"%s\",\n", config.ruleset_version.c_str());
    // 64 tick 基准（§2.1）：这是真实加速，不是从 60 重采样，故 fixture 直接写 64。
    std::fprintf(out, "  \"tickRateHz\": 64,\n");
    std::fprintf(out, "  \"patternPeriodTicks\": 45,\n");
    std::fprintf(out, "  \"tickCount\": %llu,\n", static_cast<unsigned long long>(tick_count));
    std::fprintf(out, "  \"players\": [\n");
    std::fprintf(out, "    {\"playerId\": \"player_a\", \"xMilli\": -20000, \"yMilli\": 60000, \"shotType\": \"ReimuA1\", \"bombType\": \"Reimu\"},\n");
    std::fprintf(out, "    {\"playerId\": \"player_b\", \"xMilli\": 20000, \"yMilli\": 60000, \"shotType\": \"\", \"bombType\": \"\"}\n");
    std::fprintf(out, "  ],\n");
    std::fprintf(out, "  \"ticks\": [\n");

    std::uint64_t seq_a = 0;
    std::uint64_t seq_b = 0;
    for (std::uint64_t tick = 1; tick <= tick_count; ++tick) {
        auto input_a = MakeInput(config.match_id, "player_a", tick, ++seq_a, tick >= 10);
        auto input_b = MakeInput(config.match_id, "player_b", tick, ++seq_b, false);
        sim.SubmitInput(input_a);
        sim.SubmitInput(input_b);
        const auto snapshot = sim.Tick();

        std::fprintf(out, "    {\"tick\": %llu, \"stateHash\": \"%s\", \"players\": [",
                     static_cast<unsigned long long>(snapshot.tick), snapshot.state_hash.c_str());
        for (std::size_t i = 0; i < snapshot.players.size(); ++i) {
            const auto& player = snapshot.players[i];
            std::fprintf(out, "%s{\"playerId\": \"%s\", \"xMilli\": %d, \"yMilli\": %d, \"bossCurrentHp\": %llu, \"damageDealt\": %llu}",
                         i == 0 ? "" : ", ", player.player_id.c_str(), player.x_milli, player.y_milli,
                         static_cast<unsigned long long>(player.boss_current_hp),
                         static_cast<unsigned long long>(player.damage_dealt));
        }
        std::fprintf(out, "], \"bullets\": [");
        for (std::size_t i = 0; i < snapshot.bullets.size(); ++i) {
            const auto& bullet = snapshot.bullets[i];
            std::fprintf(out, "%s{\"id\": \"%s\", \"xMilli\": %d, \"yMilli\": %d, \"vxMilli\": %d, \"vyMilli\": %d}",
                         i == 0 ? "" : ", ", bullet.bullet_id.c_str(), bullet.x_milli, bullet.y_milli,
                         bullet.vx_milli, bullet.vy_milli);
        }
        std::fprintf(out, "]}%s\n", tick == tick_count ? "" : ",");
    }
    std::fprintf(out, "  ]\n}\n");
    std::fclose(out);
    std::cout << "golden fixture written: " << path << "\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// 64 tick 基准（DANMAKU_CLIENT_ENGINE_PLAN §2.1）
//
// 铁则：64 tick 是**真实加速**（每秒多跑 4 tick），不是从 60 重采样。因此所有
// 「每帧」常量数值原样保留，直接当每 tick 用。本测试把两件事钉死：
//   1) 三套模拟的 tick 率都是 64；
//   2) 周期常量一个都没被 ×60/64 缩放（45 还是 45、90 还是 90、600 还是 600）。
// 后来者若手滑加换算，这里会立刻红。
// ---------------------------------------------------------------------------
void TestTickRateIs64AndPeriodsUnscaled() {
    Check(phk::battle::kBossRaceTickRateHz == 64, "race tick rate is 64 Hz");
    Check(phk::battle::kCoopBossTickRateHz == 64, "coop tick rate is 64 Hz");
    Check(phk::battle::kBattleTickRateHz == 64, "battle tick rate is 64 Hz");

    Check(phk::battle::kBossRacePatternPeriodTicks == 45, "pattern period stays 45 ticks (no scaling)");
    Check(phk::battle::kCoopFanIntervalTicks == 90, "coop fan interval stays 90 ticks (no scaling)");
    Check(phk::battle::kCoopCannonIntervalTicks == 600, "coop cannon interval stays 600 ticks (no scaling)");
    Check(phk::battle::kCoopCannonStartOffset == 450, "coop cannon offset stays 450 ticks (no scaling)");
    Check(phk::battle::kCoopCannonDurationTicks == 150, "coop cannon duration stays 150 ticks (no scaling)");
    Check(phk::battle::kCoopSpiralEveryTicks == 6, "coop spiral interval stays 6 ticks (no scaling)");

    auto config = MakeConfig(777u, 1000000);
    phk::battle::BossRaceSimulation sim(config);
    Check(sim.Config().tick_rate_hz == 64, "race config defaults to 64 Hz");
    for (int i = 0; i < 64; ++i) {
        sim.Tick();
    }
    Check(sim.CurrentTick() == 64, "64 Tick() calls advance exactly 64 ticks (= one second at 64 Hz)");
}

// §2.8 快照 wire 新字段：字段必须存在，且 P1 的占位值符合约定。
void TestSnapshotCarriesP1WireFields() {
    auto config = MakeConfig(424242u, 1000000);
    phk::battle::BossRaceSimulation sim(config);
    sim.AddPlayer("player_a", 0, 60000);
    sim.AddPlayer("player_b", 0, 60000);

    // 第 45 tick 是 pattern 波次边界：首波发射 + 链聚合有值。
    for (std::uint64_t tick = 1; tick <= 45; ++tick) {
        sim.SubmitInput(MakeInput(config.match_id, "player_a", tick, tick, false, 0x2u /* right */));
        sim.SubmitInput(MakeInput(config.match_id, "player_b", tick, tick, false, 0u));
        sim.Tick();
    }

    const phk::battle::BossRaceSnapshot snapshot = sim.Snapshot();
    Check(snapshot.tick == 45, "snapshot tick");

    Check(snapshot.boss.max_hp == config.boss_max_hp, "boss.max_hp mirrors config");
    Check(snapshot.boss.hp > 0 && snapshot.boss.hp <= snapshot.boss.max_hp, "boss.hp within range");
    Check(snapshot.boss.effects.empty(), "boss.effects defaults to empty (P4 fills it)");
    Check(snapshot.boss.movement.type == "static", "boss movement defaults to static without catalog");
    Check(snapshot.boss.movement.phase_t == 0, "boss movement phase_t defaults to 0");

    Check(snapshot.players.size() == 2, "two players in snapshot");
    const phk::battle::BossRacePlayerSnapshot& player_a = snapshot.players[0];
    Check(player_a.player_id == "player_a", "players[0] is player_a (map order)");
    Check(player_a.vx_milli == 3000 && player_a.vy_milli == 0, "player vx/vy mirror this tick's movement");
    Check(player_a.facing_millideg == 9000, "facing follows horizontal input (9000 = right)");
    Check(player_a.lives == 1, "lives placeholder = 1");
    Check(player_a.bombs == 3, "bombs placeholder = 3");
    Check(player_a.invulnerable_until_tick == 0, "invulnerable_until_tick defaults to 0 (P3)");
    Check(player_a.graze == 0, "graze defaults to 0 (P3)");

    Check(!snapshot.bullets.empty(), "pattern emitted bullets at the wave boundary");
    bool bullets_carry_chain_fields = true;
    for (const phk::battle::BossRaceBullet& bullet : snapshot.bullets) {
        if (bullet.hidden) bullets_carry_chain_fields = false;
        if (bullet.generation != 0) bullets_carry_chain_fields = false;
        if (bullet.chain_id != snapshot.boss.phase) bullets_carry_chain_fields = false;
    }
    Check(bullets_carry_chain_fields, "bullets carry chain_id/generation/hidden (hidden=false, gen=0)");

    Check(snapshot.chains.size() == 1, "one chain aggregated for the active pattern");
    Check(snapshot.chains.front().chain_id == snapshot.boss.phase, "chain_id = active pattern index");
    Check(snapshot.chains.front().node_id == 0 && snapshot.chains.front().rng_cursor == 0,
          "chain node_id/rng_cursor placeholders");

    Check(snapshot.hand.empty(), "hand defaults to empty (P3)");
    Check(snapshot.deck_remaining == 0, "deck_remaining defaults to 0 (P3)");
    Check(snapshot.draw_cooldown_ticks == 0, "draw_cooldown_ticks defaults to 0 (P3)");
    Check(snapshot.events.empty(), "events defaults to empty (P3)");
    Check(snapshot.event_cursor == 0, "event_cursor defaults to 0 (P3)");
}

// 特殊机制（铁拳/黑色帷幕）：默认关闭（现有模式零变化），只有
// ruleset_version == "tna-1.40-danmaku" 才启用并进快照 effects[]。
void TestSpecialMechanicsGateAndEffects() {
    auto tick_to = [](phk::battle::BossRaceSimulation& sim, std::uint64_t target) {
        while (sim.CurrentTick() < target) {
            sim.Tick();
        }
    };

    // 1) 默认 ruleset：300 tick（本该是铁拳起手相位）后 effects 仍为空。
    auto plain_config = MakeConfig(31337u, 1000000);
    phk::battle::BossRaceSimulation plain(plain_config);
    plain.AddPlayer("player_a", 0, 60000);
    plain.AddPlayer("player_b", 0, 60000);
    tick_to(plain, 310);
    Check(plain.Snapshot().boss.effects.empty(), "mechanics: disabled by default (effects empty)");

    // 2) 启用 ruleset：tick 300 铁拳 windup → tick 345 strike → tick 600 帷幕 fall。
    auto config = MakeConfig(31337u, 1000000);
    config.ruleset_version = phk::battle::kRulesetDanmakuMechanics;
    phk::battle::BossRaceSimulation sim(config);
    sim.AddPlayer("player_a", 0, 60000);
    sim.AddPlayer("player_b", 0, 60000);

    tick_to(sim, 300);
    const auto at_windup = sim.Snapshot();
    Check(!at_windup.boss.effects.empty(), "mechanics: iron fist appears at its offset tick");
    Check(at_windup.boss.effects[0].type == "iron_fist", "mechanics: effect type = iron_fist");
    Check(at_windup.boss.effects[0].state == "windup", "mechanics: starts in windup");
    Check(at_windup.boss.effects[0].t_remaining == 45, "mechanics: windup length = T1 (45)");
    Check(at_windup.boss.effects[0].length_milli == 200000, "mechanics: iron fist length carried on wire");

    tick_to(sim, 345);
    const auto at_strike = sim.Snapshot();
    Check(at_strike.boss.effects[0].state == "strike", "mechanics: windup -> strike at T1");
    Check(at_strike.boss.effects[0].t_remaining == 15, "mechanics: strike length = T2 (15)");

    tick_to(sim, 600);
    const auto at_fall = sim.Snapshot();
    bool curtain_seen = false;
    for (const auto& effect : at_fall.boss.effects) {
        if (effect.type == "black_curtain" && effect.state == "fall") curtain_seen = true;
    }
    Check(curtain_seen, "mechanics: black curtain falls at its offset tick");

    tick_to(sim, 650);
    const auto at_hold = sim.Snapshot();
    bool hold_seen = false;
    for (const auto& effect : at_hold.boss.effects) {
        if (effect.type == "black_curtain" && effect.state == "hold") hold_seen = true;
    }
    Check(hold_seen, "mechanics: black curtain reaches hold after T4");
    bool all_hidden = true;
    for (const auto& bullet : at_hold.bullets) {
        if (!bullet.hidden) all_hidden = false;
    }
    Check(all_hidden, "mechanics: bullets carry hidden=true during curtain hold");
}

int main() {
    if (const char* fixture_path = std::getenv("PHK_BOSS_RACE_FIXTURE"); fixture_path != nullptr) {
        WriteGoldenFixture(fixture_path);
        return 0;
    }

    TestPatternCatalog();
    TestFirstToDefeatWins();
    TestDeterministicBullets();
    TestPatternSchedulerCoversTenPatterns();
    TestRealBulletsCarryKindAndMotion();
    TestInputTickWindowOverflowGuard();
    TestBossCatalogTimelineExpansion();
    TestBossEmitterOffsetTriangular();
    TestBossSelectionIsDeterministicAndDistinct();
    TestUnknownBossFallsBackToDefault();
    TestPlayerLoadoutSelectsShotAndBomb();
    TestTickRateIs64AndPeriodsUnscaled();
    TestSnapshotCarriesP1WireFields();
    TestSpecialMechanicsGateAndEffects();

    if (g_failures != 0) {
        std::cerr << g_failures << " boss race test(s) failed\n";
        return 1;
    }
    std::cout << "boss race tests passed\n";
    return 0;
}
