// PhK-BattleServer：BossRaceConfig::all_features_enabled「全开」总开关接线测试。
//
// 覆盖（与主控任务清单一一对应）：
//   ① all_features_enabled=false → 逐字节回退到改动前（合成伤害 / 占位 lives /
//      events 空 / pattern-index phase）；
//   ② 默认 true（未显式设）→ 判定/擦弹/符卡/自机弹真实伤害确实生效，
//      即便 ruleset_version 仍是默认的 "tna-1.40"；
//   ③ 铁拳 / 黑色帷幕命中通过生产路径（Tick → AdvanceSpecialMechanics →
//      ApplyPlayerDamage）真的扣残机 + 给 120 tick 无敌帧 + 发 player_hit，
//      且同 tick 多来源/多帧只扣一次（无敌帧幂等）——不是纯函数单测；
//   ④ 产品路径发射的自机弹 penetrate>0 命中 Boss 后继续判定（比单段伤害更高）。
//
// 说明：所有几何判定都复用 danmaku/judgment.cpp 的整数 milli 纯函数，本测试
// 只做「接线是否生效」的黑盒断言，不重复钉纯函数数值。

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <tuple>

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

// 全开 config：故意**不设** all_features_enabled → 用默认 true。
// pattern_period_ticks 推到极远，关闭真实弹幕波次，隔离判定 / 特殊机制 /
// 自机弹路径，避免普通 boss 弹误伤玩家干扰断言。
phk::battle::BossRaceConfig MakeAllOnConfig(std::uint64_t seed, std::uint64_t boss_hp) {
    phk::battle::BossRaceConfig config;
    config.match_id = "match_all_features_" + std::to_string(seed);
    config.match_seed = seed;
    config.boss_max_hp = boss_hp;
    config.pattern_period_ticks = 1000000;
    // pattern_period_ticks 只抑制 10 模式；节点链路径有自己的周期，不受它管。
    // 本文件测的是铁拳/黑幕等机制的受击去重，必须保持「无外来弹幕」的环境，
    // 否则高密弹幕会先打中玩家，去重断言必挂。
    config.boss_id = "legacy_10mode";
    return config;
}

void AddTwoPlayers(phk::battle::BossRaceSimulation& sim, std::int32_t y_milli) {
    sim.AddPlayer("player_a", 0, y_milli);
    sim.AddPlayer("player_b", 0, y_milli);
}

bool HasEvent(const phk::battle::BossRaceSnapshot& snapshot, const char* type) {
    for (const auto& event : snapshot.events) {
        if (event.type == type) {
            return true;
        }
    }
    return false;
}

// ① legacy 回退：显式关全开开关，逐字节复现改动前行为。
void TestLegacyFallback() {
    phk::battle::BossRaceConfig config;
    config.all_features_enabled = false;
    config.match_id = "match_all_features_legacy";
    config.match_seed = 424242u;
    config.boss_max_hp = 1000000;
    phk::battle::BossRaceSimulation sim(config);
    AddTwoPlayers(sim, 60000);

    std::uint64_t seq = 0;
    phk::battle::BossRaceSnapshot last;
    for (std::uint64_t tick = 1; tick <= 20; ++tick) {
        Check(sim.SubmitInput(MakeInput(config.match_id, "player_a", tick, ++seq, true)),
              "legacy: shoot input accepted");
        last = sim.Tick();
    }
    Check(last.players[0].lives == phk::battle::kSnapshotDefaultLives,
          "legacy: lives stays at placeholder 1 (judgment disabled)");
    Check(last.players[0].boss_current_hp ==
              config.boss_max_hp - 20 * phk::battle::kBossRaceDamagePerShotTick,
          "legacy: synthetic per-tick shot damage reproduced byte-for-byte");
    Check(last.players[0].damage_dealt == 20 * phk::battle::kBossRaceDamagePerShotTick,
          "legacy: damage_dealt follows the synthetic path");
    Check(last.events.empty(), "legacy: no judgment / mechanic events");
    Check(last.boss.effects.empty(), "legacy: special mechanics not enabled");
    Check(last.boss.phase == static_cast<std::uint32_t>(sim.PatternIndexForTick(last.tick)),
          "legacy: boss.phase keeps the pattern-index placeholder");
    Check(last.players[0].graze == 0, "legacy: graze placeholder 0");
    Check(last.players[0].invulnerable_until_tick == 0, "legacy: invulnerable placeholder 0");
}

// ② 默认 true：判定 / 擦弹 / 符卡 / 自机弹真实伤害在默认 ruleset 下也生效。
void TestAllFeaturesEnabled() {
    // 2a 结构性标志 + 自机弹路径（不再是每 tick 固定合成伤害）。
    {
        auto config = MakeAllOnConfig(9001u, 1000000);
        phk::battle::BossRaceSimulation sim(config);
        AddTwoPlayers(sim, 60000);
        Check(sim.Config().all_features_enabled,
              "all-on: default config enables all features");
        const auto first = sim.Tick();  // tick 1 → Running
        Check(first.players[0].lives == phk::battle::kDanmakuDefaultLives,
              "all-on: real lives (3) instead of placeholder 1");
        Check(first.players[0].spell_total == 1,
              "all-on: spell phase 0 active at start (spell_total=1)");
        Check(first.boss.phase == 0, "all-on: boss.phase uses spell-phase semantics (0 at start)");

        std::uint64_t seq = 0;
        phk::battle::BossRaceSnapshot last;
        for (std::uint64_t tick = 2; tick <= 20; ++tick) {
            Check(sim.SubmitInput(MakeInput(config.match_id, "player_a", tick, ++seq, true)),
                  "all-on: shoot input accepted");
            last = sim.Tick();
        }
        Check(last.players[0].damage_dealt > 0,
              "all-on: product my-bullets deal real damage to Boss");
        Check(last.players[0].damage_dealt < 20 * phk::battle::kBossRaceDamagePerShotTick,
              "all-on: no synthetic per-tick shot damage (path switched to my-bullets)");
    }

    // 2b 判定 / 命中：默认 ruleset 全开后，压在判定点上的 boss 弹会扣命。
    {
        auto config = MakeAllOnConfig(9002u, 1000000);
        phk::battle::BossRaceSimulation sim(config);
        AddTwoPlayers(sim, 60000);
        sim.Tick();  // tick 1 → Running
        phk::battle::danmaku::BulletMotion motion;
        motion.x = 320.0;
        motion.y = 540.0;   // 屏幕 (320,540) = 居中 (0,60000) = player_a 判定点
        motion.hit_dis = 4.0;
        Check(sim.InjectLiveBulletForTest("player_a", motion),
              "all-on: inject bullet on player hit point");
        const auto hit = sim.Tick();  // tick 2
        Check(hit.players[0].lives == phk::battle::kDanmakuDefaultLives - 1,
              "all-on: default ruleset now applies player hit (lives--)");
        Check(hit.players[0].invulnerable_until_tick ==
                  2 + phk::battle::kDanmakuInvulnerableTicks,
              "all-on: hit grants the 120-tick invulnerability window");
        Check(HasEvent(hit, phk::battle::kPlayerHitEvent),
              "all-on: player_hit event emitted");
        Check(hit.players[0].hit_count == 1, "all-on: hit_count incremented once");
    }

    // 2c 符卡 3 阶段：自机弹跨 66% 阈值 → spell_phase 推进 + 阶段事件。
    {
        auto config = MakeAllOnConfig(9003u, 300);
        phk::battle::BossRaceSimulation sim(config);
        AddTwoPlayers(sim, 60000);
        sim.Tick();  // tick 1 → Running
        // Boss 中心 (0,-60000)，静止弹，单次伤害 100 → hp 300→200，跨 66% 阈值。
        Check(sim.InjectMyBulletForTest("player_a", 0, -60000, 0, 0, 100, 0),
              "all-on: inject my-bullet at Boss center");
        const auto snap = sim.Tick();  // tick 2
        Check(snap.boss.phase >= 1, "all-on: spell phase advances past the 66% threshold");
        Check(snap.players[0].spell_total >= 2, "all-on: spell_total grows with the new phase");
        Check(HasEvent(snap, phk::battle::kSpellPhaseStartEvent),
              "all-on: spell_phase_start event emitted");
    }
}

// ③ 特殊机制命中接线（本轮核心）：生产路径真的扣残机 + 无敌帧 + 事件，
//    且同一机制连续多 tick / 多来源只扣一次（ApplyPlayerDamage 无敌帧幂等）。
//    通过 config.special_mechanics 的 interval=0 分别隔离铁拳 / 帷幕，
//    玩家放在 y=32000：既落在铁拳 strike 胶囊推进路径上，也恰好是帷幕
//    sweep 边缘线某 tick 扫过的 y（edge 步长 21333，判定半厚 2500）。
void TestSpecialMechanicsHitsPlayer() {
    auto scan = [](phk::battle::BossRaceSimulation& sim, std::uint64_t ticks) {
        int hit_events = 0;
        std::uint32_t min_lives = phk::battle::kDanmakuDefaultLives;
        std::uint64_t max_invuln = 0;
        for (std::uint64_t tick = 1; tick <= ticks; ++tick) {
            const auto snapshot = sim.Tick();
            for (const auto& event : snapshot.events) {
                // 只数 player_a（players[0]）；两名玩家同 y 时帷幕会同时命中两人。
                if (event.type == phk::battle::kPlayerHitEvent &&
                    event.player_id == "player_a") {
                    ++hit_events;
                }
            }
            min_lives = std::min(min_lives, snapshot.players[0].lives);
            max_invuln = std::max(max_invuln, snapshot.players[0].invulnerable_until_tick);
        }
        return std::make_tuple(hit_events, min_lives, max_invuln);
    };

    // 铁拳：禁用帷幕排期（interval=0 → 永不起手）。
    {
        auto config = MakeAllOnConfig(9101u, 1000000);
        config.special_mechanics.curtain_interval_ticks = 0;
        phk::battle::BossRaceSimulation sim(config);
        AddTwoPlayers(sim, 32000);
        // 铁拳 offset=300 → windup 300..345 → strike 345..360，跑到 380 覆盖全程。
        const auto result = scan(sim, 380);
        Check(std::get<0>(result) == 1,
              "iron fist: exactly one player_hit (invuln frame de-dups the 15 strike ticks)");
        Check(std::get<1>(result) == phk::battle::kDanmakuDefaultLives - 1,
              "iron fist: strike costs exactly one life via the production path");
        Check(std::get<2>(result) > 0,
              "iron fist: hit grants an invulnerability window");
    }

    // 黑色帷幕：禁用铁拳排期（interval=0 → 永不起手）。
    {
        auto config = MakeAllOnConfig(9102u, 1000000);
        config.special_mechanics.iron_fist_interval_ticks = 0;
        phk::battle::BossRaceSimulation sim(config);
        AddTwoPlayers(sim, 32000);
        // 帷幕 offset=600 → fall 600..645 → hold 645..705 → sweep 705..750，跑到 780。
        const auto result = scan(sim, 780);
        Check(std::get<0>(result) == 1,
              "black curtain: exactly one player_hit (invuln frame de-dups sweep ticks)");
        Check(std::get<1>(result) == phk::battle::kDanmakuDefaultLives - 1,
              "black curtain: sweep costs exactly one life via the production path");
        Check(std::get<2>(result) > 0,
              "black curtain: hit grants an invulnerability window");
    }
}

// ④ 自机弹穿透产品路径：config.my_bullet_penetrate 由 SpawnMyBulletsForPlayer
//    写入 MyBullet::penetrate，JudgeMyBullets 命中后递减并继续判定 →
//    同一发弹造成多段伤害，总伤害严格大于单段（penetrate=0）基线。
void TestMyBulletPenetrateProductPath() {
    auto run = [](std::int32_t penetrate) {
        auto config = MakeAllOnConfig(9201u, 1000000);
        config.my_bullet_penetrate = penetrate;
        phk::battle::BossRaceSimulation sim(config);
        AddTwoPlayers(sim, 60000);
        std::uint64_t seq = 0;
        phk::battle::BossRaceSnapshot last;
        for (std::uint64_t tick = 1; tick <= 48; ++tick) {
            sim.SubmitInput(MakeInput(config.match_id, "player_a", tick, ++seq, true));
            last = sim.Tick();
        }
        return last.players[0].damage_dealt;
    };
    const std::uint64_t single = run(0);
    const std::uint64_t penetrating = run(3);
    Check(single > 0, "penetrate: product my-bullets hit Boss (penetrate=0 baseline)");
    Check(penetrating > single,
          "penetrate: penetrate>0 keeps judging after impact (more damage than single-hit)");
}

}  // namespace

int main() {
    TestLegacyFallback();
    TestAllFeaturesEnabled();
    TestSpecialMechanicsHitsPlayer();
    TestMyBulletPenetrateProductPath();

    if (g_failures != 0) {
        std::fprintf(stderr, "boss_race_all_features_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::cout << "boss_race_all_features_tests: all passed\n";
    return 0;
}
