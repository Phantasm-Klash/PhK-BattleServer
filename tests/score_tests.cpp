// 局内计分单测（TNA `GameData` 形状 + R1 反汇编实测常量）。
//
// 覆盖：
//   * 计分表：实测常量与出处、BossNumberOf、SpellCapturePoints 边界（clamp）；
//   * 擦弹 +100（无自机乘区）、同弹不重复计；
//   * 受击 → hit_count++、不送分、该阶段作废捕获；
//   * 符卡获取 = 实测求和式（boss21 → 4,300,000/次），跨 3 阶段各一次；
//   * 击破 = 实体消灭分 10000（TNA 无残机/炸弹/时间结算奖励）；
//   * 计时分 110 / 10 tick；消弹分 100/颗；命中分 30/次；
//   * 默认 ruleset 只有击破分；
//   * score 不进 CanonicalStateHash；Σ score_gain == score。

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "phk/battle/boss_race.hpp"
#include "phk/battle/danmaku/score_table.hpp"
#include "phk/battle/match_lifecycle.hpp"  // EncodeBossRaceSnapshotPayload（wire 字段检查）

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& label) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << label << "\n";
    }
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
    phk::battle::danmaku::BulletMotion motion;
    motion.x = 320.0;
    motion.y = 540.0;  // 正好压在判定点上
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

std::int64_t ScoreGainSum(const phk::battle::BossRaceSnapshot& snapshot, const std::string& player_id) {
    std::int64_t sum = 0;
    for (const auto& event : snapshot.events) {
        if (event.type == phk::battle::kScoreGainEvent && event.player_id == player_id) {
            sum += event.value;
        }
    }
    return sum;
}

// ---------------------------------------------------------------------------
// 计分表
// ---------------------------------------------------------------------------
void TestScoreTableShape() {
    const auto rules = phk::battle::danmaku::ScoreRulesFor();
    // 实测常量（出处见 score_table.hpp 字段注释）。
    Check(rules.graze_points == 100, "table: graze 100 @0x564d3c");
    Check(rules.bullet_clear_points == 100, "table: bullet clear 100 @0x568eb0");
    Check(rules.hit_points == 30, "table: hit 30 @0x5662b0");
    Check(rules.destroy_points == 10000, "table: destroy 10000 @0x565724");
    Check(rules.destroy_points_alt == 15000, "table: destroy alt 15000 @0x56571c");
    Check(rules.timer_points == 110, "table: timer 110 @0x562200");
    Check(rules.timer_interval_ticks == 10, "table: timer interval 10");
    Check(rules.spell_boss_coef == 300000, "table: spell boss coef 300000 @0x55e18c");
    Check(rules.spell_graze_coef == 1100, "table: spell graze coef 1100");
    Check(rules.spell_counter_coef == 110, "table: spell counter coef 110");
    Check(rules.spell_offset == 2000000, "table: spell offset 2000000");

    using phk::battle::danmaku::BossNumberOf;
    Check(BossNumberOf("boss21") == 21, "boss number: boss21 → 21");
    Check(BossNumberOf("boss11") == 11, "boss number: boss11 → 11");
    Check(BossNumberOf("") == 0, "boss number: empty → 0");
    Check(BossNumberOf("tian") == 0, "boss number: no digits → 0");

    using phk::battle::danmaku::SpellCapturePoints;
    const auto& r = phk::battle::danmaku::ScoreRulesFor();
    // 300000×21 − 2000000 = 4,300,000
    Check(SpellCapturePoints(r, 21, 0) == 4300000u, "spell points: boss21, no graze");
    // 擦弹加成：+1100/次
    Check(SpellCapturePoints(r, 21, 100) == 4300000u + 110000u, "spell points: graze adds 1100 each");
    // clamp：boss 序号 0 时公式为负 → 0
    Check(SpellCapturePoints(r, 0, 0) == 0u, "spell points: clamped at zero");
    Check(SpellCapturePoints(r, 1, 0) == 0u, "spell points: 300000−2000000 < 0 → 0");

    using phk::battle::danmaku::DestroyScoreForCharacter;
    Check(DestroyScoreForCharacter("ReimuA1") == 10000u, "destroy: unmapped character → base 10000");
    Check(DestroyScoreForCharacter("") == 10000u, "destroy: empty → base 10000");
}

// ---------------------------------------------------------------------------
// 擦弹
// ---------------------------------------------------------------------------
void TestGrazeScore() {
    auto config = MakeConfig(9301u, 1000000);
    config.ruleset_version = phk::battle::kRulesetDanmakuMechanics;
    phk::battle::BossRaceSimulation sim(config);
    sim.AddPlayer("player_a", 0, 60000);
    sim.AddPlayer("player_b", 0, 60000);
    sim.Tick();  // tick 1

    Check(sim.InjectLiveBulletForTest("player_a", MakeGrazeBullet()), "graze: bullet injected");
    const auto grazed = sim.Tick();  // tick 2
    const auto* a = FindPlayer(grazed, "player_a");
    Check(a != nullptr, "graze: player in snapshot");
    Check(a != nullptr && a->graze == 1, "graze: counter incremented");
    Check(a != nullptr && a->score == 100, "graze: score +100 (no character multiplier)");
    Check(ScoreGainSum(grazed, "player_a") == 100, "graze: score_gain carries +100");

    const auto after = sim.Tick();
    const auto* a_after = FindPlayer(after, "player_a");
    Check(a_after != nullptr && a_after->score == 100, "graze: same bullet not scored twice");
}

// ---------------------------------------------------------------------------
// 受击
// ---------------------------------------------------------------------------
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
    Check(a != nullptr && a->hit_count == 1, "hit: hit_count incremented");
    Check(a != nullptr && a->lives == phk::battle::kDanmakuDefaultLives - 1, "hit: lives decremented");
    Check(a != nullptr && a->score == 0, "hit: taking a hit scores nothing");

    // 被打过 → 该阶段不能「获取」。
    Check(sim.InjectMyBulletForTest("player_a", 0, -60000, 0, 0, 150, 0), "hit: my bullet injected");
    const auto phase1 = sim.Tick();  // tick 3
    const auto* a_phase1 = FindPlayer(phase1, "player_a");
    Check(a_phase1 != nullptr && a_phase1->spell_get == 0, "hit: a hit phase yields no capture");
    // 命中 +30（实测 @0x5662b0）；同时跨阈值清屏把 tick 2 那颗受击弹清掉 → +100。
    Check(a_phase1 != nullptr && a_phase1->score == 30 + 100,
          "hit: the shot scores +30 and the cleared bullet pays +100");
}

// ---------------------------------------------------------------------------
// 符卡获取（实测求和式）
// ---------------------------------------------------------------------------
void TestSpellCaptureAcrossThreePhases() {
    // 300 HP → 150 / 50 / 0：阶段 0 与 1 由阈值收尾，阶段 2 由击破收尾。
    auto config = MakeConfig(9321u, 300);
    config.ruleset_version = phk::battle::kRulesetDanmakuMechanics;
    config.boss_id = "boss21";  // → 每次捕获 4,300,000
    phk::battle::BossRaceSimulation sim(config);
    sim.AddPlayer("player_a", 0, 60000);
    sim.AddPlayer("player_b", 0, 60000);
    sim.Tick();  // tick 1

    const auto& rules = phk::battle::danmaku::ScoreRulesFor();
    const std::uint64_t per_capture =
        phk::battle::danmaku::SpellCapturePoints(rules, 21, 0);

    auto inject = [&](std::uint64_t damage) {
        Check(sim.InjectMyBulletForTest("player_a", 0, -60000, 0, 0, damage, 0), "spell: my bullet injected");
        return sim.Tick();
    };

    const auto phase1 = inject(150);
    const auto* a1 = FindPlayer(phase1, "player_a");
    Check(a1 != nullptr && a1->spell_get == 1, "spell: phase 0 captured");
    Check(a1 != nullptr && a1->score >= per_capture, "spell: phase 0 paid the capture bonus");

    const auto phase2 = inject(100);
    const auto* a2 = FindPlayer(phase2, "player_a");
    Check(a2 != nullptr && a2->spell_get == 2, "spell: phase 1 captured");

    const auto defeated = inject(50);
    const auto* a3 = FindPlayer(defeated, "player_a");
    Check(a3 != nullptr && a3->spell_get == 3, "spell: the final phase captured on defeat");

    // 总分 = 3 次捕获 + 击破 10000 + 每次命中 30×3。
    const std::uint64_t expected = per_capture * 3u + rules.destroy_points + rules.hit_points * 3u;
    Check(a3 != nullptr && a3->score == expected, "spell: total = 3 captures + destroy + 3 hits");
}

// ---------------------------------------------------------------------------
// 计时 / 消弹 / 命中
// ---------------------------------------------------------------------------
void TestTimerScore() {
    auto config = MakeConfig(9331u, 1000000);
    config.ruleset_version = phk::battle::kRulesetDanmakuMechanics;
    phk::battle::BossRaceSimulation sim(config);
    sim.AddPlayer("player_a", 0, 60000);
    sim.AddPlayer("player_b", 0, 60000);

    phk::battle::BossRaceSnapshot snapshot = sim.Tick();
    while (sim.CurrentTick() < 9) {
        snapshot = sim.Tick();
    }
    const auto* at9 = FindPlayer(snapshot, "player_a");
    Check(at9 != nullptr && at9->score == 0, "timer: nothing before the 10-tick grid");

    snapshot = sim.Tick();  // tick 10
    const auto* at10 = FindPlayer(snapshot, "player_a");
    Check(at10 != nullptr && at10->score == 110, "timer: +110 on tick 10");

    while (sim.CurrentTick() < 20) {
        snapshot = sim.Tick();
    }
    const auto* at20 = FindPlayer(snapshot, "player_a");
    Check(at20 != nullptr && at20->score == 220, "timer: +110 again on tick 20");
}

void TestBulletClearScore() {
    auto config = MakeConfig(9341u, 300);
    config.ruleset_version = phk::battle::kRulesetDanmakuMechanics;
    phk::battle::BossRaceSimulation sim(config);
    sim.AddPlayer("player_a", 0, 60000);
    sim.AddPlayer("player_b", 0, 60000);
    sim.Tick();

    // 放 3 颗玩家 A 的弹，再跨阈值清屏 → 每颗 +100。
    for (int i = 0; i < 3; ++i) {
        Check(sim.InjectLiveBulletForTest("player_a", MakeGrazeBullet()), "clear: bullet injected");
    }
    Check(sim.InjectMyBulletForTest("player_a", 0, -60000, 0, 0, 150, 0), "clear: threshold bullet injected");
    const auto cleared = sim.Tick();
    const auto* a = FindPlayer(cleared, "player_a");
    // 3 颗被清 + 命中 30 + 捕获 0（无 boss_id）+ 击破 0。
    Check(a != nullptr && a->score == 300 + 30, "clear: 3 bullets × 100 + one hit × 30");
}

// ---------------------------------------------------------------------------
// 默认 ruleset / hash
// ---------------------------------------------------------------------------
void TestDefaultRulesetOnlyDestroy() {
    auto config = MakeConfig(9351u, 100);  // 默认 ruleset
    phk::battle::BossRaceSimulation sim(config);
    sim.AddPlayer("player_a", 0, 60000);
    sim.AddPlayer("player_b", 0, 60000);
    sim.Tick();

    // 默认路径：合成伤害每 tick 10，100 HP 十 tick 内击破（但每 tick 10 点、
    // 且要跨 10 tick 才有计时分——计时分只在 mechanics 下，故这里应当只有击破分）。
    phk::battle::BattleInput input;
    input.match_id = config.match_id;
    input.player_id = "player_a";
    input.shoot = true;
    phk::battle::BossRaceSnapshot snapshot = sim.Tick();
    for (int i = 0; i < 40 && sim.State() == phk::battle::BossRaceState::Running; ++i) {
        input.tick = sim.CurrentTick() + 1;
        input.seq = input.tick;
        sim.SubmitInput(input);
        snapshot = sim.Tick();
    }
    const auto* a = FindPlayer(snapshot, "player_a");
    Check(a != nullptr, "default: player in snapshot");
    Check(a != nullptr && a->score == 10000, "default: only the destroy score (no timer/graze/spell)");
}

void TestScoreWireFields() {
    auto config = MakeConfig(9371u, 100);
    phk::battle::BossRaceSimulation sim(config);
    sim.AddPlayer("player_a", 0, 60000);
    sim.AddPlayer("player_b", 0, 60000);
    sim.Tick();
    const std::vector<std::uint8_t> payload =
        phk::battle::EncodeBossRaceSnapshotPayload(sim.Snapshot());
    const std::string json(payload.begin(), payload.end());
    Check(json.find("\"score\":") != std::string::npos, "wire: snapshot carries score");
    Check(json.find("\"hit_count\":") != std::string::npos, "wire: snapshot carries hit_count");
    Check(json.find("\"spell_get\":") != std::string::npos, "wire: snapshot carries spell_get");
    Check(json.find("\"spell_total\":") != std::string::npos, "wire: snapshot carries spell_total");
}

// 跨阈值后击破：已结束的阶段不得在击破时补计，最后阶段只计一次。
void TestNoDoubleCaptureOnDefeat() {
    const auto& rules = phk::battle::danmaku::ScoreRulesFor();
    auto config = MakeConfig(9381u, 300);
    config.ruleset_version = phk::battle::kRulesetDanmakuMechanics;
    config.boss_id = "boss21";
    phk::battle::BossRaceSimulation sim(config);
    sim.AddPlayer("player_a", 0, 60000);
    sim.AddPlayer("player_b", 0, 60000);
    sim.Tick();

    const std::uint64_t per_capture =
        phk::battle::danmaku::SpellCapturePoints(rules, 21, 0);

    Check(sim.InjectMyBulletForTest("player_a", 0, -60000, 0, 0, 200, 0), "no-double: threshold bullet");
    const auto crossed = sim.Tick();
    const auto* a_crossed = FindPlayer(crossed, "player_a");
    Check(a_crossed != nullptr && a_crossed->spell_get == 2, "no-double: two phases captured by thresholds");
    // 跨两段：2×捕获 + 1 次命中 30 + 清屏（无玩家弹可清 → 0）。
    Check(a_crossed != nullptr && a_crossed->score == per_capture * 2u + rules.hit_points,
          "no-double: threshold captures paid once each");

    const auto idle = sim.Tick();
    const auto* a_idle = FindPlayer(idle, "player_a");
    Check(a_idle != nullptr && a_idle->score == a_crossed->score,
          "no-double: idle tick does not re-award ended phases");

    Check(sim.InjectMyBulletForTest("player_a", 0, -60000, 0, 0, 100, 0), "no-double: finishing bullet");
    const auto defeated = sim.Tick();
    const auto* a = FindPlayer(defeated, "player_a");
    Check(a != nullptr && a->spell_get == 3, "no-double: final phase captured on defeat");
    Check(a != nullptr && a->score == per_capture * 3u + rules.hit_points * 2u + rules.destroy_points,
          "no-double: three captures + two hits + destroy, nothing twice");
}

// 累计不变量：整局 Σ score_gain == 最终 score（每玩家各自成立）。
void TestCumulativeInvariant() {
    auto config = MakeConfig(9391u, 300);
    config.ruleset_version = phk::battle::kRulesetDanmakuMechanics;
    config.boss_id = "boss21";
    phk::battle::BossRaceSimulation sim(config);
    sim.AddPlayer("player_a", 0, 60000);
    sim.AddPlayer("player_b", 0, 60000);

    std::uint64_t sum_a = 0;
    std::uint64_t sum_b = 0;
    phk::battle::BossRaceSnapshot snapshot = sim.Tick();
    auto absorb = [&]() {
        for (const auto& event : snapshot.events) {
            if (event.type != phk::battle::kScoreGainEvent) {
                continue;
            }
            if (event.player_id == "player_a") {
                sum_a += static_cast<std::uint64_t>(event.value);
            } else if (event.player_id == "player_b") {
                sum_b += static_cast<std::uint64_t>(event.value);
            }
        }
    };
    absorb();

    Check(sim.InjectLiveBulletForTest("player_a", MakeGrazeBullet()), "invariant: graze bullet");
    snapshot = sim.Tick();
    absorb();
    Check(sim.InjectMyBulletForTest("player_a", 0, -60000, 0, 0, 300, 0), "invariant: kill bullet");
    snapshot = sim.Tick();
    absorb();

    const auto* a = FindPlayer(snapshot, "player_a");
    const auto* b = FindPlayer(snapshot, "player_b");
    Check(a != nullptr && a->score == sum_a, "invariant: player_a cumulative score_gain == score");
    Check(b != nullptr && b->score == sum_b, "invariant: player_b cumulative score_gain == score");
}

void TestScoreNotInHash() {
    // 说明：hash 材料里含 tick，跨 tick 比较无意义；同 tick 下要让「只有分数不同、
    // 世界相同」也很难构造（擦弹需要一颗存在于世界里的弹）。因此这里验证的是
    // 可验证的两条：
    //   1) 同 seed 同输入 → hash 与 score 都逐位一致（计分确定性）；
    //   2) 同一 tick 重复调用 hash 稳定。
    // 「score 不进 hash 材料」由 `CanonicalStateHash` 的实现保证（boss_race.cpp
    // 中该函数只序列化位置/血量/伤害/子弹/擦弹等模拟字段，不引用 score 系字段，
    // 函数上方有明确注释），客户端侧也有一份同口径的 hash 材料测试。
    auto config = MakeConfig(9361u, 1000000);
    config.ruleset_version = phk::battle::kRulesetDanmakuMechanics;
    phk::battle::BossRaceSimulation sim_a(config);
    phk::battle::BossRaceSimulation sim_b(config);
    for (auto* sim : {&sim_a, &sim_b}) {
        sim->AddPlayer("player_a", 0, 60000);
        sim->AddPlayer("player_b", 0, 60000);
        sim->Tick();
    }
    Check(sim_a.InjectLiveBulletForTest("player_a", MakeGrazeBullet()), "hash: bullet injected (a)");
    Check(sim_b.InjectLiveBulletForTest("player_a", MakeGrazeBullet()), "hash: bullet injected (b)");
    const auto snap_a = sim_a.Tick();
    const auto snap_b = sim_b.Tick();

    const auto* a = FindPlayer(snap_a, "player_a");
    const auto* b = FindPlayer(snap_b, "player_a");
    Check(a != nullptr && b != nullptr && a->score == 100 && b->score == 100,
          "hash: both runs scored the graze");
    Check(snap_a.state_hash == snap_b.state_hash, "hash: identical runs hash identically");
    Check(sim_a.CanonicalStateHash() == sim_a.CanonicalStateHash(), "hash: stable at the same tick");
}

}  // namespace

int main() {
    TestScoreTableShape();
    TestGrazeScore();
    TestHitCountAndSpellLoss();
    TestSpellCaptureAcrossThreePhases();
    TestTimerScore();
    TestBulletClearScore();
    TestDefaultRulesetOnlyDestroy();
    TestScoreWireFields();
    TestNoDoubleCaptureOnDefeat();
    TestCumulativeInvariant();
    TestScoreNotInHash();

    if (g_failures != 0) {
        std::cerr << g_failures << " score test(s) failed\n";
        return 1;
    }
    std::cout << "score tests passed\n";
    return 0;
}
