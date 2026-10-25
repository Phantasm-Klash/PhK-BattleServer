// 局内计分（TNA `GameData` 形状）单测。
//
// 覆盖：
//   * 计分表形态（ScoreRules 冻结占位 + 13 自机擦弹乘区）；
//   * 擦弹得分 = graze_points × 自机乘区；
//   * 受击 → hit_count++ 且该符卡阶段作废（不给捕获分）；
//   * 符卡获取（跨 3 阶段各一次，含击破收尾的最后一阶段）；
//   * 击破奖励 + 结算奖励（残机/炸弹/剩余时间）；
//   * 默认 ruleset 只算击破 + 结算（无擦弹/符卡分）；
//   * score 不进 CanonicalStateHash（同世界、不同分数 → hash 相等）。

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "phk/battle/boss_race.hpp"
#include "phk/battle/danmaku/score_table.hpp"

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
    bool shoot
) {
    phk::battle::BattleInput input;
    input.match_id = match_id;
    input.player_id = player_id;
    input.tick = tick;
    input.seq = seq;
    input.shoot = shoot;
    return input;
}

phk::battle::BossRaceConfig MakeConfig(std::uint64_t seed, std::uint64_t boss_hp) {
    phk::battle::BossRaceConfig config;
    config.match_id = "match_score_000001";
    config.match_seed = seed;
    config.boss_max_hp = boss_hp;
    return config;
}

phk::battle::danmaku::BulletMotion MakeGrazeBullet() {
    // 屏幕坐标 (320,530) = 居中 milli (0,-10000)；玩家出生在 (0,60000)
    // → 距离 10px = 10000 milli，落在 (r_hit, r_graze] 的擦弹带内。
    phk::battle::danmaku::BulletMotion motion;
    motion.x = 320.0;
    motion.y = 530.0;
    motion.hit_dis = 4.0;
    return motion;
}

phk::battle::danmaku::BulletMotion MakeHitBullet() {
    // 屏幕坐标 (320,540) = 居中 milli (0,60000)，正好压在判定点上。
    phk::battle::danmaku::BulletMotion motion;
    motion.x = 320.0;
    motion.y = 540.0;
    motion.hit_dis = 4.0;
    return motion;
}

const phk::battle::BossRacePlayerSnapshot* FindPlayer(
    const phk::battle::BossRaceSnapshot& snapshot,
    const std::string& player_id
) {
    for (const auto& player : snapshot.players) {
        if (player.player_id == player_id) {
            return &player;
        }
    }
    return nullptr;
}

// 本 tick 内某玩家的 score_gain 增量之和（用于校验「增量之和 == 总分」）。
std::int64_t ScoreGainSum(const phk::battle::BossRaceSnapshot& snapshot, const std::string& player_id) {
    std::int64_t sum = 0;
    for (const auto& event : snapshot.events) {
        if (event.type == phk::battle::kScoreGainEvent && event.player_id == player_id) {
            sum += event.value;
        }
    }
    return sum;
}

void TestScoreTableShape() {
    const auto rules = phk::battle::danmaku::ScoreRulesFor();
    // 实测（R1）：0x564d3c / 0x5665e0 / 0x567088 三处擦弹加分点均为 +100。
    Check(rules.graze_points == 100, "rules: graze_points = 100 (measured @0x564d3c)");
    Check(rules.spell_capture_base == 100000, "rules: spell_capture_base placeholder");
    Check(rules.spell_capture_per_phase == 50000, "rules: spell_capture_per_phase placeholder");
    Check(rules.boss_defeat_bonus == 1000000, "rules: boss_defeat_bonus placeholder");
    Check(rules.life_bonus == 100000, "rules: life_bonus placeholder");
    Check(rules.bomb_bonus == 20000, "rules: bomb_bonus placeholder");
    Check(rules.extra_time_per_second == 1000, "rules: extra_time_per_second placeholder");

    using phk::battle::danmaku::GrazeMulForShotType;
    using phk::battle::danmaku::ShotCharacterOf;
    Check(ShotCharacterOf("ReimuA1") == std::string_view("Reimu"), "shot char: ReimuA1");
    Check(ShotCharacterOf("YuyukoB2") == std::string_view("Yuyuko"), "shot char: YuyukoB2");
    Check(ShotCharacterOf("RemilliaA2") == std::string_view("Remillia"), "shot char: RemilliaA2");
    Check(ShotCharacterOf("Reimu").empty(), "shot char: no suffix → empty");
    Check(ShotCharacterOf("ReimuC1").empty(), "shot char: unknown mode → empty");
    Check(ShotCharacterOf("").empty(), "shot char: empty input");

    // 13 自机 × 4 形态全部 1.0（冻结占位）；未知/空同样 1.0。
    const char* characters[] = {
        "Alice", "Aya", "Cirno", "Marisa", "Reimu", "Reisen", "Remillia",
        "Sakuya", "Sanae", "Seija", "Youmu", "Yukari", "Yuyuko",
    };
    for (const char* character : characters) {
        for (const char* suffix : {"A1", "A2", "B1", "B2"}) {
            const std::string shot = std::string(character) + suffix;
            Check(GrazeMulForShotType(shot) == 1.0, "graze mul placeholder 1.0: " + shot);
        }
    }
    Check(GrazeMulForShotType("") == 1.0, "graze mul: empty shot type → 1.0");
    Check(GrazeMulForShotType("NobodyA1") == 1.0, "graze mul: unknown character → 1.0");
}

void TestGrazeScoreTimesShotMul() {
    auto config = MakeConfig(9301u, 1000000);
    config.ruleset_version = phk::battle::kRulesetDanmakuMechanics;
    phk::battle::PlayerLoadoutSpec loadout;
    loadout.player_id = "player_a";
    loadout.shot_type = "ReimuA1";
    config.loadouts.push_back(loadout);
    phk::battle::BossRaceSimulation sim(config);
    sim.AddPlayer("player_a", 0, 60000);
    sim.AddPlayer("player_b", 0, 60000);
    sim.Tick();  // tick 1 → Running

    Check(sim.InjectLiveBulletForTest("player_a", MakeGrazeBullet()), "graze score: bullet injected");
    const auto grazed = sim.Tick();  // tick 2
    const auto* a = FindPlayer(grazed, "player_a");
    Check(a != nullptr, "graze score: player_a in snapshot");
    const double mul = phk::battle::danmaku::GrazeMulForShotType("ReimuA1");
    const auto expected = static_cast<std::uint64_t>(
        static_cast<double>(phk::battle::danmaku::ScoreRulesFor().graze_points) * mul);
    Check(a->graze == 1, "graze score: graze counter incremented");
    Check(a->score == expected, "graze score: score = graze_points × shot multiplier");
    Check(ScoreGainSum(grazed, "player_a") == static_cast<std::int64_t>(expected),
          "graze score: score_gain event carries the increment");

    // 同一颗弹不重复计分。
    const auto after = sim.Tick();
    const auto* a_after = FindPlayer(after, "player_a");
    Check(a_after != nullptr && a_after->score == expected, "graze score: same bullet is not scored twice");

    // 无 loadout 的玩家：空 shot_type → 乘区 1.0，同分。
    auto plain_config = MakeConfig(9302u, 1000000);
    plain_config.ruleset_version = phk::battle::kRulesetDanmakuMechanics;
    phk::battle::BossRaceSimulation plain(plain_config);
    plain.AddPlayer("player_a", 0, 60000);
    plain.AddPlayer("player_b", 0, 60000);
    plain.Tick();
    Check(plain.InjectLiveBulletForTest("player_a", MakeGrazeBullet()), "graze score: plain bullet injected");
    const auto plain_snapshot = plain.Tick();
    const auto* plain_a = FindPlayer(plain_snapshot, "player_a");
    Check(plain_a != nullptr && plain_a->score == expected, "graze score: no loadout falls back to ×1.0");
}

void TestHitCountAndSpellLoss() {
    auto config = MakeConfig(9311u, 300);
    config.ruleset_version = phk::battle::kRulesetDanmakuMechanics;
    phk::battle::BossRaceSimulation sim(config);
    sim.AddPlayer("player_a", 0, 60000);
    sim.AddPlayer("player_b", 0, 60000);
    sim.Tick();  // tick 1

    Check(sim.InjectLiveBulletForTest("player_a", MakeHitBullet()), "hit: bullet injected");
    const auto hit = sim.Tick();  // tick 2
    const auto* a = FindPlayer(hit, "player_a");
    Check(a != nullptr, "hit: player_a in snapshot");
    Check(a->hit_count == 1, "hit: hit_count incremented");
    Check(a->lives == phk::battle::kDanmakuDefaultLives - 1, "hit: lives decremented");
    Check(a->score == 0, "hit: taking a hit scores nothing");

    // 被打过 → 该阶段不能「获取」：跨 66% 阈值后 spell_get 仍为 0、不给符卡分。
    Check(sim.InjectMyBulletForTest("player_a", 0, -60000, 0, 0, 150, 0), "hit: my bullet injected");
    const auto phase1 = sim.Tick();  // tick 3
    const auto* a_phase1 = FindPlayer(phase1, "player_a");
    Check(a_phase1 != nullptr && a_phase1->boss_current_hp == 150, "hit: crossed the 66% threshold");
    Check(a_phase1 != nullptr && a_phase1->spell_total == 2, "hit: spell_total counts the new phase");
    Check(a_phase1 != nullptr && a_phase1->spell_get == 0, "hit: a hit phase yields no spell capture");
    Check(a_phase1 != nullptr && a_phase1->score == 0, "hit: no spell score for a hit phase");
}

void TestSpellCaptureAcrossThreePhases() {
    // 300 HP → 150 / 50 / 0：阶段 0 与 1 由阈值收尾，阶段 2 由击破收尾。
    auto config = MakeConfig(9321u, 300);
    config.ruleset_version = phk::battle::kRulesetDanmakuMechanics;
    phk::battle::BossRaceSimulation sim(config);
    sim.AddPlayer("player_a", 0, 60000);
    sim.AddPlayer("player_b", 0, 60000);
    sim.Tick();  // tick 1

    std::int64_t gained = 0;
    auto inject = [&](std::uint64_t damage) {
        Check(sim.InjectMyBulletForTest("player_a", 0, -60000, 0, 0, damage, 0), "spell: my bullet injected");
        const auto snapshot = sim.Tick();
        gained += ScoreGainSum(snapshot, "player_a");
        return snapshot;
    };

    const auto phase1 = inject(150);
    const auto* a1 = FindPlayer(phase1, "player_a");
    Check(a1 != nullptr && a1->spell_get == 1, "spell: phase 0 captured");
    Check(a1 != nullptr && a1->spell_total == 2, "spell: two phases counted");

    const auto phase2 = inject(100);
    const auto* a2 = FindPlayer(phase2, "player_a");
    Check(a2 != nullptr && a2->spell_get == 2, "spell: phase 1 captured");
    Check(a2 != nullptr && a2->spell_total == 3, "spell: three phases counted");

    const auto defeated = inject(50);
    const auto* a3 = FindPlayer(defeated, "player_a");
    Check(a3 != nullptr && a3->spell_get == 3, "spell: the final phase captured on defeat");
    Check(a3 != nullptr && a3->spell_total == 3, "spell: total stays at three phases");

    const auto rules = phk::battle::danmaku::ScoreRulesFor();
    const std::uint64_t captures =
        (rules.spell_capture_base + rules.spell_capture_per_phase * 1u) +
        (rules.spell_capture_base + rules.spell_capture_per_phase * 2u) +
        (rules.spell_capture_base + rules.spell_capture_per_phase * 3u);
    const std::uint64_t settlement =
        rules.life_bonus * phk::battle::kDanmakuDefaultLives +
        rules.bomb_bonus * phk::battle::kDanmakuDefaultBombs;
    const std::uint64_t expected = captures + rules.boss_defeat_bonus + settlement;
    Check(a3 != nullptr && a3->score == expected, "spell: score = 3 captures + defeat + settlement");
    Check(static_cast<std::uint64_t>(gained) == expected,
          "spell: score_gain increments sum to the final score");

    bool capture_event = false;
    for (const auto& event : defeated.events) {
        if (event.type == phk::battle::kSpellCaptureEvent && event.player_id == "player_a") {
            capture_event = true;
        }
    }
    Check(capture_event, "spell: spell_capture event still emitted on defeat");
}

void TestDefeatAndSettlementDefaultRuleset() {
    auto config = MakeConfig(9331u, 100);  // 10 伤害/tick → 第 10 tick 击破
    phk::battle::BossRaceSimulation sim(config);
    sim.AddPlayer("player_a", 0, 60000);
    sim.AddPlayer("player_b", 0, 60000);

    std::uint64_t seq_a = 0;
    std::uint64_t seq_b = 0;
    for (std::uint64_t tick = 1;
         tick <= 200 && sim.State() != phk::battle::BossRaceState::Finished;
         ++tick) {
        sim.SubmitInput(MakeInput(config.match_id, "player_a", tick, ++seq_a, true));
        sim.SubmitInput(MakeInput(config.match_id, "player_b", tick, ++seq_b, false));
        sim.Tick();
    }
    Check(sim.State() == phk::battle::BossRaceState::Finished, "settlement: race finished");

    const auto snapshot = sim.Snapshot();
    const auto* a = FindPlayer(snapshot, "player_a");
    const auto* b = FindPlayer(snapshot, "player_b");
    const auto rules = phk::battle::danmaku::ScoreRulesFor();
    const std::uint64_t settlement =
        rules.life_bonus * phk::battle::kDanmakuDefaultLives +
        rules.bomb_bonus * phk::battle::kDanmakuDefaultBombs;
    Check(a != nullptr && a->score == rules.boss_defeat_bonus + settlement,
          "settlement: winner gets defeat bonus + settlement");
    Check(b != nullptr && b->score == settlement, "settlement: loser still gets the settlement bonus");
    Check(a != nullptr && a->hit_count == 0 && a->spell_get == 0 && a->spell_total == 0,
          "settlement: the default ruleset has no hit/spell counters");
}

void TestSettlementTimeBonus() {
    auto config = MakeConfig(9341u, 100);
    config.max_ticks = 640;  // 10 秒 = 640 tick；第 10 tick 击破 → 剩 9 秒
    phk::battle::BossRaceSimulation sim(config);
    sim.AddPlayer("player_a", 0, 60000);
    sim.AddPlayer("player_b", 0, 60000);

    std::uint64_t seq = 0;
    for (std::uint64_t tick = 1;
         tick <= 200 && sim.State() != phk::battle::BossRaceState::Finished;
         ++tick) {
        sim.SubmitInput(MakeInput(config.match_id, "player_a", tick, ++seq, true));
        sim.Tick();
    }
    Check(sim.State() == phk::battle::BossRaceState::Finished, "time bonus: race finished");

    const auto snapshot = sim.Snapshot();
    const auto* a = FindPlayer(snapshot, "player_a");
    const auto rules = phk::battle::danmaku::ScoreRulesFor();
    const std::uint64_t settlement =
        rules.life_bonus * phk::battle::kDanmakuDefaultLives +
        rules.bomb_bonus * phk::battle::kDanmakuDefaultBombs;
    const std::uint64_t seconds_left = (config.max_ticks - snapshot.winner_tick) / 64u;
    Check(seconds_left == 9, "time bonus: 640 - 10 ticks leaves nine whole seconds");
    const std::uint64_t expected =
        rules.boss_defeat_bonus + settlement + rules.extra_time_per_second * seconds_left;
    Check(a != nullptr && a->score == expected, "time bonus: extra_time_per_second × remaining seconds");

    // 超时（finish_tick >= max_ticks）不给时间项。
    auto timed_out_config = MakeConfig(9342u, 100);
    timed_out_config.max_ticks = 5;
    phk::battle::BossRaceSimulation timed_out(timed_out_config);
    timed_out.AddPlayer("player_a", 0, 60000);
    timed_out.AddPlayer("player_b", 0, 60000);
    std::uint64_t timed_out_seq = 0;
    for (std::uint64_t tick = 1;
         tick <= 200 && timed_out.State() != phk::battle::BossRaceState::Finished;
         ++tick) {
        timed_out.SubmitInput(MakeInput(timed_out_config.match_id, "player_a", tick, ++timed_out_seq, true));
        timed_out.Tick();
    }
    const auto timed_out_snapshot = timed_out.Snapshot();
    const auto* timed_out_a = FindPlayer(timed_out_snapshot, "player_a");
    Check(timed_out_a != nullptr && timed_out_a->score == rules.boss_defeat_bonus + settlement,
          "time bonus: an over-time finish gets no time bonus");
}

void TestDefaultRulesetHasNoGrazeOrSpellScore() {
    auto config = MakeConfig(9351u, 1000000);  // 默认 ruleset
    phk::battle::BossRaceSimulation sim(config);
    sim.AddPlayer("player_a", 0, 60000);
    sim.AddPlayer("player_b", 0, 60000);
    sim.Tick();

    Check(sim.InjectLiveBulletForTest("player_a", MakeGrazeBullet()), "default: graze bullet injected");
    const auto snapshot = sim.Tick();
    const auto* a = FindPlayer(snapshot, "player_a");
    Check(a != nullptr && a->graze == 0, "default: no graze counting");
    Check(a != nullptr && a->score == 0, "default: no graze score");
    Check(ScoreGainSum(snapshot, "player_a") == 0, "default: no score_gain event");
    Check(a != nullptr && a->spell_total == 0, "default: no spell phases tracked");
}

void TestScoreIsNotInStateHash() {
    // 同一 seed / 同一输入 / 同一世界：只差结算的时间项 → 分数不同、hash 相同。
    auto make = [](std::uint64_t max_ticks) {
        auto config = MakeConfig(9361u, 100);
        config.max_ticks = max_ticks;
        return config;
    };
    auto run = [](phk::battle::BossRaceSimulation& sim) {
        std::uint64_t seq = 0;
        for (std::uint64_t tick = 1;
             tick <= 200 && sim.State() != phk::battle::BossRaceState::Finished;
             ++tick) {
            sim.SubmitInput(MakeInput(sim.Config().match_id, "player_a", tick, ++seq, true));
            sim.Tick();
        }
    };

    phk::battle::BossRaceSimulation no_cap(make(0));
    phk::battle::BossRaceSimulation with_cap(make(640));
    for (phk::battle::BossRaceSimulation* sim : {&no_cap, &with_cap}) {
        sim->AddPlayer("player_a", 0, 60000);
        sim->AddPlayer("player_b", 0, 60000);
        run(*sim);
    }

    const auto no_cap_snapshot = no_cap.Snapshot();
    const auto with_cap_snapshot = with_cap.Snapshot();
    const auto* a_no_cap = FindPlayer(no_cap_snapshot, "player_a");
    const auto* a_with_cap = FindPlayer(with_cap_snapshot, "player_a");
    Check(a_no_cap != nullptr && a_no_cap->score > 0, "hash: score is computed (non-zero)");
    Check(a_with_cap != nullptr && a_with_cap->score > a_no_cap->score,
          "hash: the time bonus makes the scores differ");
    Check(no_cap.CanonicalStateHash() == with_cap.CanonicalStateHash(),
          "hash: score is excluded from CanonicalStateHash");

    // 同 seed 两次完全相同的 run：hash 相等且 score > 0（擦弹分已计入）。
    auto tna_config = MakeConfig(9362u, 1000000);
    tna_config.ruleset_version = phk::battle::kRulesetDanmakuMechanics;
    phk::battle::BossRaceSimulation first(tna_config);
    phk::battle::BossRaceSimulation second(tna_config);
    for (phk::battle::BossRaceSimulation* sim : {&first, &second}) {
        sim->AddPlayer("player_a", 0, 60000);
        sim->AddPlayer("player_b", 0, 60000);
        sim->Tick();
        sim->InjectLiveBulletForTest("player_a", MakeGrazeBullet());
        sim->Tick();
    }
    const auto first_snapshot = first.Snapshot();
    const auto* first_a = FindPlayer(first_snapshot, "player_a");
    Check(first_a != nullptr && first_a->score > 0, "hash: identical runs score grazes");
    Check(first.CanonicalStateHash() == second.CanonicalStateHash(),
          "hash: identical runs share the same hash");
}

}  // namespace

int main() {
    TestScoreTableShape();
    TestGrazeScoreTimesShotMul();
    TestHitCountAndSpellLoss();
    TestSpellCaptureAcrossThreePhases();
    TestDefeatAndSettlementDefaultRuleset();
    TestSettlementTimeBonus();
    TestDefaultRulesetHasNoGrazeOrSpellScore();
    TestScoreIsNotInStateHash();

    if (g_failures != 0) {
        std::cerr << g_failures << " score test(s) failed\n";
        return 1;
    }
    std::cout << "score tests passed\n";
    return 0;
}
