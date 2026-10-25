// 难度缩放（计划 v2 P4）钉帧测试。
//
// 三条契约：
//   1) 默认 ruleset（"tna-1.40"）完全不读 difficulty → 0/1/3 三档逐 tick
//      state_hash 必须完全相等（600 tick 长跨度，防止某处悄悄接进默认路径）。
//   2) tna-1.40-danmaku + 10 模式路径（boss_id 为空）：
//      每波弹数 = llround(base × density_mul)，base = 难度 1 的原值。
//   3) 发射间隔 = llround(45 × interval_mul)：难度 3 → 36 tick 网格、难度 0 → 52。
//
// 10 个模式的每波基数（src/danmaku/pattern_timeline.cpp，模式序 0..9）：
//   [16, 12, 7, 4, 2, 8, 6, 5, 10, 4]
// 调度从 wave 1 开始（tick = 45×k 时 pattern_index = k % 10），因此每波基数按
//   12, 7, 4, 2, 8, 6, 5, 10, 4, 16 循环。
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "phk/battle/boss_race.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (condition) {
        return;
    }
    std::cerr << "FAIL: " << message << "\n";
    ++g_failures;
}

phk::battle::BossRaceConfig MakeConfig(const std::string& ruleset, std::int32_t difficulty) {
    phk::battle::BossRaceConfig config;
    config.match_id = "match_difficulty_tests_000001";
    config.match_seed = 4242u;
    config.boss_max_hp = 1000000;
    config.ruleset_version = ruleset;
    config.difficulty = difficulty;
    return config;
}

void AddTwoPlayers(phk::battle::BossRaceSimulation& sim) {
    sim.AddPlayer("player_a", 0, 60000);
    sim.AddPlayer("player_b", 0, -60000);
}

// 1) 默认 ruleset：难度 0/1/3 逐 tick 哈希相等（600 tick）。
void TestDefaultRulesetIgnoresDifficulty() {
    phk::battle::BossRaceSimulation normal(MakeConfig("tna-1.40", 1));
    phk::battle::BossRaceSimulation easy(MakeConfig("tna-1.40", 0));
    phk::battle::BossRaceSimulation lunatic(MakeConfig("tna-1.40", 3));
    AddTwoPlayers(normal);
    AddTwoPlayers(easy);
    AddTwoPlayers(lunatic);

    bool easy_matches = true;
    bool lunatic_matches = true;
    for (std::uint64_t tick = 1; tick <= 600; ++tick) {
        const phk::battle::BossRaceSnapshot base = normal.Tick();
        const phk::battle::BossRaceSnapshot easy_snapshot = easy.Tick();
        const phk::battle::BossRaceSnapshot lunatic_snapshot = lunatic.Tick();
        if (base.state_hash != easy_snapshot.state_hash) {
            easy_matches = false;
            std::cerr << "  default ruleset: easy hash diverged at tick " << tick << "\n";
            break;
        }
        if (base.state_hash != lunatic_snapshot.state_hash) {
            lunatic_matches = false;
            std::cerr << "  default ruleset: lunatic hash diverged at tick " << tick << "\n";
            break;
        }
    }
    Check(easy_matches, "difficulty: default ruleset ignores difficulty 0 over 600 ticks");
    Check(lunatic_matches, "difficulty: default ruleset ignores difficulty 3 over 600 ticks");
    Check(easy.Snapshot().difficulty == 0, "difficulty: snapshot still echoes easy (0)");
    Check(lunatic.Snapshot().difficulty == 3, "difficulty: snapshot still echoes lunatic (3)");
    Check(normal.Snapshot().bullets.size() > 0, "difficulty: baseline actually spawned bullets");
}

// 采样一次「每波弹数」：跑 ticks 个 tick，返回出波 tick 列表与每玩家弹数序列。
// 用 bullet_id 去重统计新增（出界子弹会被移除，快照总数差值不可用）。
struct WaveSample {
    std::vector<std::uint64_t> ticks;
    std::vector<std::size_t> per_player_counts;
};

WaveSample SampleWaves(std::int32_t difficulty, std::uint64_t ticks) {
    phk::battle::BossRaceSimulation sim(MakeConfig(phk::battle::kRulesetDanmakuMechanics, difficulty));
    AddTwoPlayers(sim);
    WaveSample sample;
    std::set<std::string> seen_bullet_ids;
    for (std::uint64_t tick = 1; tick <= ticks; ++tick) {
        const phk::battle::BossRaceSnapshot snapshot = sim.Tick();
        std::size_t fresh = 0;
        for (const phk::battle::BossRaceBullet& bullet : snapshot.bullets) {
            if (seen_bullet_ids.insert(bullet.bullet_id).second) {
                ++fresh;
            }
        }
        if (fresh != 0) {
            Check(fresh % 2 == 0, "density: wave delta is even (2 players share the same volley)");
            sample.ticks.push_back(snapshot.tick);
            sample.per_player_counts.push_back(fresh / 2);
        }
    }
    return sample;
}

// 2) 密度：难度 1 = 基数；难度 3 = llround(base×1.35)；难度 0 = llround(base×0.80)。
void TestDensityScaling() {
    const std::vector<std::size_t> base_per_wave = {12, 7, 4, 2, 8, 6, 5, 10, 4, 16};

    const WaveSample normal = SampleWaves(1, 460);
    Check(normal.ticks == std::vector<std::uint64_t>({45, 90, 135, 180, 225, 270, 315, 360, 405, 450}),
          "density: normal keeps the 45 tick wave grid");
    Check(normal.per_player_counts == base_per_wave, "density: normal per-wave counts = base");

    // 难度 3（×1.35）：12→16, 7→9, 4→5, 2→3, 8→11, 6→8, 5→7, 10→14, 4→5, 16→22,
    // 然后 wave 11/12 回到 12→16、7→9。
    const WaveSample lunatic = SampleWaves(3, 450);
    const std::vector<std::size_t> lunatic_expected = {16, 9, 5, 3, 11, 8, 7, 14, 5, 22, 16, 9};
    Check(lunatic.ticks == std::vector<std::uint64_t>({36, 72, 108, 144, 180, 216, 252, 288, 324, 360, 396, 432}),
          "density: lunatic interval = llround(45 x 0.80) = 36 tick grid");
    Check(lunatic.per_player_counts == lunatic_expected,
          "density: lunatic per-wave counts = llround(base x 1.35)");

    // 难度 0（×0.80）：12→10, 7→6, 4→3, 2→2, 8→6, 6→5, 5→4, 10→8, 4→3, 16→13。
    const WaveSample easy = SampleWaves(0, 530);
    const std::vector<std::size_t> easy_expected = {10, 6, 3, 2, 6, 5, 4, 8, 3, 13};
    Check(easy.ticks == std::vector<std::uint64_t>({52, 104, 156, 208, 260, 312, 364, 416, 468, 520}),
          "density: easy interval = llround(45 x 1.15) = 52 tick grid");
    Check(easy.per_player_counts == easy_expected,
          "density: easy per-wave counts = llround(base x 0.80)");
}

// 3) 间隔：相邻两次出波的 tick 差 = 缩放后的调度周期。
void TestIntervalSpacing() {
    for (const std::int32_t difficulty : {0, 1, 2, 3}) {
        const phk::battle::danmaku::DifficultyScale scale =
            phk::battle::danmaku::DifficultyScaleFor(difficulty);
        const std::uint64_t expected =
            phk::battle::danmaku::ScalePeriod(phk::battle::kBossRacePatternPeriodTicks, scale.interval_mul);
        const WaveSample sample = SampleWaves(difficulty, expected * 4 + 1);
        Check(sample.ticks.size() == 4,
              "interval: difficulty " + std::to_string(difficulty) + " emits 4 waves in 4 periods");
        if (sample.ticks.size() == 4) {
            for (std::size_t i = 1; i < sample.ticks.size(); ++i) {
                Check(sample.ticks[i] - sample.ticks[i - 1] == expected,
                      "interval: difficulty " + std::to_string(difficulty) +
                          " consecutive wave gap = " + std::to_string(expected));
            }
        }
    }
}

}  // namespace

int main() {
    TestDefaultRulesetIgnoresDifficulty();
    TestDensityScaling();
    TestIntervalSpacing();

    if (g_failures != 0) {
        std::cerr << g_failures << " difficulty scale test(s) failed\n";
        return 1;
    }
    std::cout << "difficulty scale tests passed\n";
    return 0;
}
