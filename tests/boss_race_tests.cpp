// Tests for the MVP "mvp_boss_race" simulation: two players, identical Boss
// copies, ten deterministic bullet patterns, first-to-defeat wins.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

#include "phk/battle/boss_race.hpp"

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
    Check(ids.front() == "ring", "first pattern is ring");
    Check(ids.back() == "blossom", "last pattern is blossom");
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

}  // namespace

int main() {
    TestPatternCatalog();
    TestFirstToDefeatWins();
    TestDeterministicBullets();
    TestPatternSchedulerCoversTenPatterns();

    if (g_failures != 0) {
        std::cerr << g_failures << " boss race test(s) failed\n";
        return 1;
    }
    std::cout << "boss race tests passed\n";
    return 0;
}
