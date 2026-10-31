// BossRaceSimulation 的 §2.3 卡牌机制接线测试。
//
// 覆盖的是「票据发牌 → 每 tick 能量回复 → 出牌落账 → 快照下发 → 4 种效果」
// 这条真实链路，全部走产品入口（SeedHand / SubmitInput / Tick / Snapshot），
// 不直测 card_hand / card_effects 的纯函数（那些有独立测试）。
//
// 对照真值：
//   * 账本与校验顺序 = card_hand.cpp（Go validateAndActivateCardLocked）
//   * 执行语义      = BattleSimulation::ExecuteCastCard / RunCardEffect
//                     （simulation.cpp:2395-2534）+ TS boss_race.ts:1322-1460
//   * wire 形状     = battle_codec.ts parseHandList / parseEventList
//                     （hand[] 无 player_id；事件附加字段归进 data）
//
// 版权：东方同人逆向成果，仅用于自研玩法。

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "phk/battle/boss_race.hpp"
#include "phk/battle/card_hand.hpp"
#include "phk/battle/danmaku/bullet_motion.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& label) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << label << "\n";
    }
}

constexpr const char* kMatchId = "match_cards_000001";
constexpr double kEps = 1e-9;

phk::battle::BossRaceConfig MakeConfig(std::uint64_t match_seed = 424242u) {
    phk::battle::BossRaceConfig config;
    config.match_id = kMatchId;
    config.match_seed = match_seed;
    config.boss_max_hp = 6000;
    return config;
}

phk::battle::BattleInput MakeCardInput(std::uint64_t tick, std::uint64_t seq, int slot) {
    phk::battle::BattleInput input;
    input.match_id = kMatchId;
    input.player_id = "p1";
    input.tick = tick;
    input.seq = seq;
    input.card_slot = slot;
    return input;
}

// "使用"按钮：card_use + card_select（card_slot 保持 -1）。
phk::battle::BattleInput MakeCardUseInput(std::uint64_t tick, std::uint64_t seq, int select) {
    phk::battle::BattleInput input = MakeCardInput(tick, seq, -1);
    input.card_use = true;
    input.card_select = static_cast<std::int8_t>(select);
    return input;
}

// "抽卡"按钮。
phk::battle::BattleInput MakeCardDrawInput(std::uint64_t tick, std::uint64_t seq) {
    phk::battle::BattleInput input = MakeCardInput(tick, seq, -1);
    input.card_draw = true;
    return input;
}

// 两名玩家 → StartIfReady 才会把 state 推到 Running（ApplyPlayerTick 才跑）。
void AddTwoPlayers(phk::battle::BossRaceSimulation& sim) {
    sim.AddPlayer("p1", 0, 0);
    sim.AddPlayer("p2", 20000, 60000);
}

std::size_t CountEvents(const phk::battle::BossRaceSnapshot& snapshot, const std::string& type) {
    std::size_t count = 0;
    for (const phk::battle::BossEventWire& event : snapshot.events) {
        if (event.type == type) {
            ++count;
        }
    }
    return count;
}

const phk::battle::BossEventWire* FindEvent(
    const phk::battle::BossRaceSnapshot& snapshot,
    const std::string& type
) {
    for (const phk::battle::BossEventWire& event : snapshot.events) {
        if (event.type == type) {
            return &event;
        }
    }
    return nullptr;
}

const phk::battle::BossRacePlayerSnapshot* FindPlayer(
    const phk::battle::BossRaceSnapshot& snapshot,
    const std::string& player_id
) {
    for (const phk::battle::BossRacePlayerSnapshot& player : snapshot.players) {
        if (player.player_id == player_id) {
            return &player;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 1. 播种
// ---------------------------------------------------------------------------
void TestSeedHandState() {
    phk::battle::BossRaceSimulation sim(MakeConfig());
    AddTwoPlayers(sim);

    Check(sim.HandSize("p1") == 0, "seed: hand starts empty");
    Check(std::abs(sim.PlayerEnergy("p1") - 0.0) < kEps, "seed: energy starts at 0");
    Check(sim.LobbySeed() == 0, "seed: lobby seed starts unset");

    const std::vector<std::string> hand = {"focus_lens", "purge_bell"};
    const std::vector<std::string> deck = {"focus_lens", "purge_bell", "aegis_ward", "finale_arc"};
    Check(
        sim.SeedHand("p1", hand, deck, 5.5, "1234567890123"),
        "seed: SeedHand accepts a known player"
    );
    Check(sim.HandSize("p1") == 2, "seed: hand size matches the ticket hand");
    Check(std::abs(sim.PlayerEnergy("p1") - 5.5) < kEps, "seed: energy matches initial_energy");
    Check(sim.LobbySeed() == 1234567890123LL, "seed: lobby seed parsed from decimal text");

    // 未发牌的玩家不受影响（管线按玩家独立开关）。
    Check(sim.HandSize("p2") == 0, "seed: unseeded player keeps an empty hand");
    Check(std::abs(sim.PlayerEnergy("p2") - 0.0) < kEps, "seed: unseeded player does not accrue");

    // 未知玩家 → false，不产生任何副作用。
    Check(
        !sim.SeedHand("nobody", hand, deck, 1.0, ""),
        "seed: unknown player is rejected"
    );

    // 非十进制种子 → false（票据种子必须是 int64 十进制文本）。
    Check(
        !sim.SeedHand("p2", {"focus_lens"}, {"focus_lens"}, 1.0, "not-a-number"),
        "seed: non-decimal server seed is rejected"
    );

    // 同场第二张票据携带不同种子 = 大厅不变量违例 → 拒绝。
    Check(
        !sim.SeedHand("p2", {"focus_lens"}, {"focus_lens"}, 1.0, "999"),
        "seed: conflicting server seed is rejected"
    );
    // 相同种子可以重复发牌（幂等）。
    Check(
        sim.SeedHand("p2", {"focus_lens"}, {"focus_lens"}, 1.0, "1234567890123"),
        "seed: identical server seed is accepted"
    );

    // 空手牌必须安全：不崩、不自动补牌（单人局，走顶层别名路径）。
    phk::battle::BossRaceSimulation empty_sim(MakeConfig());
    empty_sim.AddPlayer("p1", 0, 0);
    Check(
        empty_sim.SeedHand("p1", {}, {}, 3.0, "7"),
        "seed: empty hand is accepted"
    );
    Check(empty_sim.HandSize("p1") == 0, "seed: empty hand stays empty");
    empty_sim.Tick();
    const phk::battle::BossRaceSnapshot snapshot = empty_sim.Tick();
    Check(snapshot.hand.empty(), "seed: empty hand yields an empty snapshot hand");
    Check(snapshot.deck_remaining == 0, "seed: empty deck reports zero remaining");
}

// ---------------------------------------------------------------------------
// 2. 每 tick 能量回复（且不超上限）
// ---------------------------------------------------------------------------
void TestEnergyRegenPerTickAndCap() {
    phk::battle::BossRaceSimulation sim(MakeConfig());
    AddTwoPlayers(sim);
    sim.SeedHand("p1", {"focus_lens"}, {"focus_lens"}, 0.0, "1");

    constexpr int kTicks = 400;
    for (int i = 0; i < kTicks; ++i) {
        sim.Tick();
    }
    const double expected = kTicks * phk::battle::kCardEnergyPerTick;
    Check(
        std::abs(sim.PlayerEnergy("p1") - expected) < 1e-6,
        "regen: energy accrues kCardEnergyPerTick per tick"
    );

    // 上限：10.0（kCardMaxEnergy）。
    phk::battle::BossRaceSimulation capped(MakeConfig());
    AddTwoPlayers(capped);
    capped.SeedHand("p1", {"focus_lens"}, {"focus_lens"}, phk::battle::kCardMaxEnergy - 0.001, "1");
    for (int i = 0; i < 200; ++i) {
        capped.Tick();
    }
    Check(
        std::abs(capped.PlayerEnergy("p1") - phk::battle::kCardMaxEnergy) < 1e-9,
        "regen: energy is capped at kCardMaxEnergy"
    );

    // 未发牌玩家不回复（默认路径零变化）。
    Check(std::abs(sim.PlayerEnergy("p2") - 0.0) < kEps, "regen: unseeded player never accrues");
}

// ---------------------------------------------------------------------------
// 3. 出牌落账：扣能量 + 写冷却 + 移槽 + 补抽
// ---------------------------------------------------------------------------
void TestCastBooksLedger() {
    phk::battle::BossRaceSimulation sim(MakeConfig());
    AddTwoPlayers(sim);
    // purge_bell：cost 3 / cooldown 480；hand = deck 前缀。
    sim.SeedHand("p1", {"purge_bell"}, {"purge_bell", "aegis_ward"}, 5.0, "42");
    sim.Tick();  // tick 1：进入 Running（能量 +0.0025）

    Check(
        sim.SubmitInput(MakeCardInput(2, 1, 0)),
        "ledger: card input is accepted for tick 2"
    );
    const phk::battle::BossRaceSnapshot snapshot = sim.Tick();  // tick 2

    // 能量：5.0 + 2×0.0025 - 3（回复先于扣费）。
    const double expected_energy = 5.0 + 2.0 * phk::battle::kCardEnergyPerTick - 3.0;
    Check(
        std::abs(sim.PlayerEnergy("p1") - expected_energy) < 1e-9,
        "ledger: energy = initial + regen - cost (regen happens before the cast)"
    );

    // 手牌：purge_bell 出掉，补抽 deck[1] = aegis_ward。
    Check(sim.HandSize("p1") == 1, "ledger: cast removes the slot and redraws one card");
    const phk::battle::BossRacePlayerSnapshot* p1 = FindPlayer(snapshot, "p1");
    Check(p1 != nullptr, "ledger: the snapshot carries p1");
    if (p1 != nullptr) {
        Check(
            p1->hand.size() == 1 && p1->hand[0].card_id == "aegis_ward",
            "ledger: the redrawn card is the next deck entry (players[].hand is authoritative)"
        );
        Check(p1->hand.size() == 1 && p1->hand[0].slot == 0, "ledger: players[].hand slot is the index");
        Check(
            std::abs(p1->energy - expected_energy) < 1e-9,
            "ledger: players[].energy mirrors the hand energy"
        );
    }
    Check(
        snapshot.hand.empty(),
        "ledger: the top-level hand alias stays empty in a two-player match"
    );
    Check(
        snapshot.deck_remaining == 0 && snapshot.draw_cooldown_ticks == 0,
        "ledger: top-level deck/draw scalars stay 0 in a two-player match"
    );

    // 事件：card_accepted 带 card_id / slot / value。
    const phk::battle::BossEventWire* accepted =
        FindEvent(snapshot, phk::battle::kCardAcceptedEvent);
    Check(accepted != nullptr, "ledger: card_accepted is emitted");
    if (accepted != nullptr) {
        Check(accepted->card_id == "purge_bell", "ledger: card_accepted carries the card id");
        Check(accepted->slot == 0, "ledger: card_accepted carries the slot");
        Check(accepted->player_id == "p1", "ledger: card_accepted carries the player");
    }

    // 冷却：下一 tick 再出同一张（补抽的是别的牌，故直接验冷却表口径）——
    // 换成让 deck 再抽回 purge_bell，验证 tick < cooldown_until 被拒。
    phk::battle::BossRaceSimulation cooldown_sim(MakeConfig());
    AddTwoPlayers(cooldown_sim);
    cooldown_sim.SeedHand("p1", {"purge_bell", "purge_bell"}, {"purge_bell", "purge_bell"}, 10.0, "42");
    cooldown_sim.Tick();  // tick 1
    cooldown_sim.SubmitInput(MakeCardInput(2, 1, 0));
    cooldown_sim.Tick();  // tick 2：第一张出掉，补抽一张 purge_bell 回来
    Check(cooldown_sim.HandSize("p1") == 2, "ledger: cooldown fixture keeps two cards");
    cooldown_sim.SubmitInput(MakeCardInput(3, 2, 0));
    const phk::battle::BossRaceSnapshot second = cooldown_sim.Tick();  // tick 3
    const phk::battle::BossEventWire* rejected =
        FindEvent(second, phk::battle::kCardRejectedEvent);
    Check(rejected != nullptr, "ledger: casting on cooldown is rejected");
    if (rejected != nullptr) {
        Check(
            rejected->reason == "cast_card_on_cooldown",
            "ledger: cooldown rejection reason is cast_card_on_cooldown"
        );
    }
}

// ---------------------------------------------------------------------------
// 4. -1 哨兵：不出牌、无事件、无状态变更
// ---------------------------------------------------------------------------
void TestSentinelPlaysNoCard() {
    phk::battle::BossRaceSimulation sim(MakeConfig());
    AddTwoPlayers(sim);
    sim.SeedHand("p1", {"focus_lens", "purge_bell"}, {"focus_lens", "purge_bell"}, 5.0, "42");
    sim.Tick();  // tick 1

    Check(sim.SubmitInput(MakeCardInput(2, 1, -1)), "sentinel: -1 input is accepted by SubmitInput");
    const phk::battle::BossRaceSnapshot snapshot = sim.Tick();  // tick 2

    Check(sim.HandSize("p1") == 2, "sentinel: hand is unchanged");
    Check(
        std::abs(sim.PlayerEnergy("p1") - (5.0 + 2.0 * phk::battle::kCardEnergyPerTick)) < 1e-9,
        "sentinel: energy only accrued, nothing was spent"
    );
    Check(
        CountEvents(snapshot, phk::battle::kCardAcceptedEvent) == 0 &&
            CountEvents(snapshot, phk::battle::kCardRejectedEvent) == 0 &&
            CountEvents(snapshot, phk::battle::kCardEffectAppliedEvent) == 0,
        "sentinel: no card events are emitted"
    );
}

// ---------------------------------------------------------------------------
// 5. 拒绝路径：越界 slot / 能量不足
// ---------------------------------------------------------------------------
void TestRejectionPaths() {
    // 越界 slot（手牌只有 1 张，slot 3 无牌可出）。
    phk::battle::BossRaceSimulation out_of_range(MakeConfig());
    AddTwoPlayers(out_of_range);
    out_of_range.SeedHand("p1", {"focus_lens"}, {"focus_lens"}, 10.0, "42");
    out_of_range.Tick();
    out_of_range.SubmitInput(MakeCardInput(2, 1, 3));
    const phk::battle::BossRaceSnapshot out_snapshot = out_of_range.Tick();
    const phk::battle::BossEventWire* out_rejected =
        FindEvent(out_snapshot, phk::battle::kCardRejectedEvent);
    Check(out_rejected != nullptr, "reject: out-of-range slot is rejected");
    if (out_rejected != nullptr) {
        Check(
            out_rejected->reason == "cast_card_slot_out_of_range",
            "reject: out-of-range reason is cast_card_slot_out_of_range"
        );
        Check(out_rejected->slot == 3, "reject: the rejected row keeps the offending slot");
    }
    Check(
        CountEvents(out_snapshot, phk::battle::kCardAcceptedEvent) == 0,
        "reject: a rejected cast never emits card_accepted"
    );
    Check(out_of_range.HandSize("p1") == 1, "reject: a rejected cast leaves the hand alone");

    // 能量不足（finale_arc cost 6，只给 1.0）。
    phk::battle::BossRaceSimulation poor(MakeConfig());
    AddTwoPlayers(poor);
    poor.SeedHand("p1", {"finale_arc"}, {"finale_arc"}, 1.0, "42");
    poor.Tick();
    poor.SubmitInput(MakeCardInput(2, 1, 0));
    const phk::battle::BossRaceSnapshot poor_snapshot = poor.Tick();
    const phk::battle::BossEventWire* poor_rejected =
        FindEvent(poor_snapshot, phk::battle::kCardRejectedEvent);
    Check(poor_rejected != nullptr, "reject: insufficient energy is rejected");
    if (poor_rejected != nullptr) {
        Check(
            poor_rejected->reason == "cast_card_insufficient_energy",
            "reject: energy reason is cast_card_insufficient_energy"
        );
    }
    Check(poor.HandSize("p1") == 1, "reject: insufficient energy keeps the card in hand");
}

// ---------------------------------------------------------------------------
// 6. 快照 hand 的 wire 形状：players[].hand 是权威，顶层 hand 是单人别名
// ---------------------------------------------------------------------------
void TestSnapshotHandShape() {
    // 6a. 两人局：只有 players[].hand 有值，顶层 hand 必须留空（单份广播）。
    {
        phk::battle::BossRaceSimulation sim(MakeConfig());
        AddTwoPlayers(sim);
        sim.SeedHand(
            "p1",
            {"focus_lens", "purge_bell", "aegis_ward"},
            {"focus_lens", "purge_bell", "aegis_ward", "finale_arc", "thunder_fall"},
            4.0,
            "42");
        sim.SeedHand("p2", {"finale_arc"}, {"finale_arc", "thunder_fall"}, 2.5, "42");

        const phk::battle::BossRaceSnapshot snapshot = sim.Snapshot();
        Check(
            snapshot.hand.empty(),
            "snapshot: the top-level hand alias is empty when players.size() > 1"
        );
        Check(
            snapshot.deck_remaining == 0 && snapshot.draw_cooldown_ticks == 0,
            "snapshot: top-level deck/draw scalars are 0 when players.size() > 1"
        );

        const phk::battle::BossRacePlayerSnapshot* p1 = FindPlayer(snapshot, "p1");
        Check(p1 != nullptr, "snapshot: p1 is in players[]");
        if (p1 != nullptr) {
            Check(p1->hand.size() == 3, "snapshot: players[].hand length matches that player's hand");
            for (std::size_t i = 0; i < p1->hand.size(); ++i) {
                Check(p1->hand[i].slot == static_cast<std::int32_t>(i), "snapshot: players[].hand slot == i");
                Check(!p1->hand[i].selected, "snapshot: players[].hand selected is false");
            }
            Check(p1->hand.size() == 3 && p1->hand[0].card_id == "focus_lens", "snapshot: p1 hand[0]");
            Check(p1->hand.size() == 3 && p1->hand[2].card_id == "aegis_ward", "snapshot: p1 hand[2]");
            Check(std::abs(p1->energy - 4.0) < 1e-9, "snapshot: players[].energy carries the hand energy");
        }

        // 每玩家手牌互不串味（这正是顶层扁平数组做不到的事）。
        const phk::battle::BossRacePlayerSnapshot* p2 = FindPlayer(snapshot, "p2");
        Check(p2 != nullptr, "snapshot: p2 is in players[]");
        if (p2 != nullptr) {
            Check(p2->hand.size() == 1, "snapshot: p2 has its own hand size");
            Check(
                p2->hand.size() == 1 && p2->hand[0].card_id == "finale_arc",
                "snapshot: p2 sees its own card, not p1's"
            );
            Check(std::abs(p2->energy - 2.5) < 1e-9, "snapshot: p2 has its own energy");
        }
    }

    // 6b. 单人局：顶层 hand 作为兼容别名填真值（老客户端立刻可用）。
    {
        phk::battle::BossRaceSimulation solo(MakeConfig());
        solo.AddPlayer("p1", 0, 0);
        const std::vector<std::string> hand = {"focus_lens", "purge_bell", "aegis_ward"};
        const std::vector<std::string> deck = {
            "focus_lens", "purge_bell", "aegis_ward", "finale_arc", "thunder_fall"};
        solo.SeedHand("p1", hand, deck, 4.0, "42");

        const phk::battle::BossRaceSnapshot snapshot = solo.Snapshot();
        Check(snapshot.hand.size() == hand.size(), "solo: top-level hand length matches the ticket hand");
        for (std::size_t i = 0; i < snapshot.hand.size() && i < hand.size(); ++i) {
            Check(snapshot.hand[i].card_id == hand[i], "solo: hand[i].card_id is in slot order");
            Check(snapshot.hand[i].slot == static_cast<std::int32_t>(i), "solo: hand[i].slot == i");
            Check(!snapshot.hand[i].selected, "solo: hand[i].selected is false");
        }
        // draw_cursor 播种时 = hand.size() = 3，deck 5 张 → 剩余 2。
        Check(snapshot.deck_remaining == 2, "solo: deck_remaining = deck.size() - draw_cursor");
        Check(snapshot.draw_cooldown_ticks == 0, "solo: draw_cooldown_ticks is 0");

        // 别名与权威同源。
        const phk::battle::BossRacePlayerSnapshot* p1 = FindPlayer(snapshot, "p1");
        Check(
            p1 != nullptr && p1->hand.size() == snapshot.hand.size(),
            "solo: the alias and players[].hand agree"
        );
    }
}

// ---------------------------------------------------------------------------
// 7. opponent_pattern：从 boss 原点朝对手生成扇形子弹
// ---------------------------------------------------------------------------
void TestOpponentPatternSpawnsBullets() {
    phk::battle::BossRaceSimulation sim(MakeConfig());
    AddTwoPlayers(sim);
    // mirror_lance：opponent_pattern，cost 4，7 发扇形。
    sim.SeedHand("p1", {"mirror_lance"}, {"mirror_lance"}, 10.0, "42");
    sim.Tick();  // tick 1

    Check(sim.Snapshot().bullets.empty(), "pattern: field starts empty");
    sim.SubmitInput(MakeCardInput(2, 1, 0));
    const phk::battle::BossRaceSnapshot snapshot = sim.Tick();  // tick 2

    Check(
        snapshot.bullets.size() == static_cast<std::size_t>(phk::battle::kCardOpponentPatternCount),
        "pattern: mirror_lance spawns kCardOpponentPatternCount bullets"
    );
    for (const phk::battle::BossRaceBullet& bullet : snapshot.bullets) {
        Check(bullet.owner_player_id == "p1", "pattern: the spawned bullets are owned by the caster");
    }
    Check(
        CountEvents(snapshot, phk::battle::kCardEffectAppliedEvent) == 1,
        "pattern: card_effect_applied is emitted"
    );
}

// ---------------------------------------------------------------------------
// 8. finisher：清屏 + boss 伤害
// ---------------------------------------------------------------------------
void TestFinisherClearsFieldAndDamagesBoss() {
    phk::battle::BossRaceSimulation sim(MakeConfig());
    AddTwoPlayers(sim);
    // finale_arc：finisher，cost 6，damage 60。
    sim.SeedHand("p1", {"finale_arc"}, {"finale_arc"}, 10.0, "42");
    sim.Tick();  // tick 1

    for (int i = 0; i < 5; ++i) {
        phk::battle::danmaku::BulletMotion motion;
        motion.x = 100.0 + 20.0 * i;
        motion.y = 300.0;
        motion.hit_dis = 4.0;
        Check(sim.InjectLiveBulletForTest("p1", motion), "finisher: injected bullet is accepted");
    }
    const phk::battle::BossRaceSnapshot before = sim.Tick();  // tick 2
    Check(before.bullets.size() == 5, "finisher: five bullets are on the field");
    Check(
        before.players[0].boss_current_hp == 6000,
        "finisher: boss is untouched before the cast"
    );

    sim.SubmitInput(MakeCardInput(3, 1, 0));
    const phk::battle::BossRaceSnapshot after = sim.Tick();  // tick 3

    Check(after.bullets.empty(), "finisher: every bullet is cleared");
    Check(
        after.players[0].boss_current_hp == 6000 - 60,
        "finisher: the card damage hits the caster's own boss copy"
    );
}

// ---------------------------------------------------------------------------
// 9. bullet_purge：选取种子必须来自票据（T7 H-2），不是 match_seed
// ---------------------------------------------------------------------------
std::vector<std::string> PurgeSurvivors(std::uint64_t match_seed, const std::string& ticket_seed) {
    phk::battle::BossRaceSimulation sim(MakeConfig(match_seed));
    AddTwoPlayers(sim);
    // frost_arc：bullet_purge，cost 2，purge_radius 100px，purge_count 12。
    sim.SeedHand("p1", {"frost_arc"}, {"frost_arc"}, 10.0, ticket_seed);
    sim.Tick();  // tick 1

    // 20 颗静止子弹，全部落在 (0,0) 周围 100px 内 → 候选 20 颗，清 12 留 8。
    for (int i = 0; i < 20; ++i) {
        phk::battle::danmaku::BulletMotion motion;
        motion.x = 320.0 + static_cast<double>((i % 5) - 2) * 12.0;
        motion.y = 480.0 + static_cast<double>((i / 5) - 2) * 12.0;
        motion.vx = 0.0;
        motion.vy = 0.0;
        motion.hit_dis = 4.0;
        sim.InjectLiveBulletForTest("p1", motion);
    }
    sim.SubmitInput(MakeCardInput(2, 1, 0));
    const phk::battle::BossRaceSnapshot snapshot = sim.Tick();  // tick 2

    std::vector<std::string> survivors;
    survivors.reserve(snapshot.bullets.size());
    for (const phk::battle::BossRaceBullet& bullet : snapshot.bullets) {
        survivors.push_back(bullet.bullet_id);
    }
    return survivors;
}

void TestBulletPurgeUsesTicketSeed() {
    const std::vector<std::string> a = PurgeSurvivors(111u, "987654321");
    const std::vector<std::string> b = PurgeSurvivors(999u, "987654321");  // 只换 match_seed
    const std::vector<std::string> c = PurgeSurvivors(111u, "1357911");    // 只换票据种子

    Check(a.size() == 8, "purge: 20 candidates with purge_count 12 leaves 8");
    Check(
        a == b,
        "purge: same ticket seed + different match_seed gives the same selection "
        "(T7 H-2: the seed comes from the ticket, not config_.match_seed)"
    );
    Check(
        a != c,
        "purge: a different ticket seed gives a different selection"
    );
}

// ---------------------------------------------------------------------------
// 10. card_use + card_select（"使用"按钮）等价于出牌该槽
// ---------------------------------------------------------------------------
void TestCardUseButtonCastsSelectedSlot() {
    phk::battle::BossRaceSimulation sim(MakeConfig());
    AddTwoPlayers(sim);
    // purge_bell：cost 3 / cooldown 480；hand = deck 前缀。
    // energy 给 5.0（低于 10.0 上限，避免回复被 cap 掉后算不准扣费）。
    sim.SeedHand(
        "p1",
        {"purge_bell", "focus_lens"},
        {"purge_bell", "focus_lens", "aegis_ward"},
        5.0,
        "42");
    sim.Tick();  // tick 1

    Check(sim.SubmitInput(MakeCardUseInput(2, 1, 0)), "use: card_use input is accepted");
    const phk::battle::BossRaceSnapshot snapshot = sim.Tick();  // tick 2

    const phk::battle::BossEventWire* accepted =
        FindEvent(snapshot, phk::battle::kCardAcceptedEvent);
    Check(accepted != nullptr, "use: card_use + card_select casts the selected slot");
    if (accepted != nullptr) {
        Check(accepted->card_id == "purge_bell", "use: the selected card is the one cast");
        Check(accepted->slot == 0, "use: the accepted row reports the selected slot");
    }
    // 扣一次费：5 + 2×0.0025 - 3。
    Check(
        std::abs(sim.PlayerEnergy("p1") - (5.0 + 2.0 * phk::battle::kCardEnergyPerTick - 3.0)) < 1e-9,
        "use: energy is charged exactly once"
    );
    // 出掉的槽被移除，且 ApplyCast 立刻从牌堆补抽一张（deck[draw_cursor]）。
    const phk::battle::BossRacePlayerSnapshot* p1 = FindPlayer(snapshot, "p1");
    Check(
        p1 != nullptr && p1->hand.size() == 2 && p1->hand[0].card_id == "focus_lens" &&
            p1->hand[1].card_id == "aegis_ward",
        "use: the cast slot is removed and one card is redrawn from the deck"
    );

    // card_slot 与 card_use 同 tick 同时出现 → 只出一次（card_slot 优先）。
    phk::battle::BossRaceSimulation both(MakeConfig());
    AddTwoPlayers(both);
    both.SeedHand(
        "p1",
        {"purge_bell", "focus_lens"},
        {"purge_bell", "focus_lens", "aegis_ward"},
        5.0,
        "42");
    both.Tick();  // tick 1
    phk::battle::BattleInput dual = MakeCardUseInput(2, 1, 1);
    dual.card_slot = 0;  // 同时给了 card_slot
    Check(both.SubmitInput(dual), "use: dual-intent input is accepted");
    const phk::battle::BossRaceSnapshot dual_snapshot = both.Tick();
    Check(
        CountEvents(dual_snapshot, phk::battle::kCardAcceptedEvent) == 1,
        "use: card_slot + card_use in the same tick casts exactly once"
    );
    const phk::battle::BossEventWire* dual_accepted =
        FindEvent(dual_snapshot, phk::battle::kCardAcceptedEvent);
    Check(
        dual_accepted != nullptr && dual_accepted->slot == 0,
        "use: card_slot wins over card_select when both are present"
    );
    Check(
        std::abs(both.PlayerEnergy("p1") - (5.0 + 2.0 * phk::battle::kCardEnergyPerTick - 3.0)) < 1e-9,
        "use: the dual-intent tick charges energy only once"
    );
}

// ---------------------------------------------------------------------------
// 11. card_draw（"抽卡"按钮）
// ---------------------------------------------------------------------------
void TestCardDrawButton() {
    // 11a. 正常抽一张：不消耗能量。
    {
        phk::battle::BossRaceSimulation sim(MakeConfig());
        AddTwoPlayers(sim);
        sim.SeedHand(
            "p1",
            {"focus_lens"},
            {"focus_lens", "purge_bell", "aegis_ward"},
            5.0,
            "42");
        sim.Tick();  // tick 1
        Check(sim.SubmitInput(MakeCardDrawInput(2, 1)), "draw: card_draw input is accepted");
        const phk::battle::BossRaceSnapshot snapshot = sim.Tick();  // tick 2

        Check(sim.HandSize("p1") == 2, "draw: one card is drawn from the deck");
        const phk::battle::BossRacePlayerSnapshot* p1 = FindPlayer(snapshot, "p1");
        Check(
            p1 != nullptr && p1->hand.size() == 2 && p1->hand[1].card_id == "purge_bell",
            "draw: the drawn card is the next deck entry (deck[draw_cursor])"
        );
        Check(
            std::abs(sim.PlayerEnergy("p1") - (5.0 + 2.0 * phk::battle::kCardEnergyPerTick)) < 1e-9,
            "draw: drawing does not cost energy"
        );
        const phk::battle::BossEventWire* drawn =
            FindEvent(snapshot, phk::battle::kCardDrawnEvent);
        Check(drawn != nullptr, "draw: card_drawn is emitted");
        if (drawn != nullptr) {
            Check(drawn->card_id == "purge_bell", "draw: card_drawn carries the card id");
            Check(drawn->slot == 1, "draw: card_drawn carries the new slot");
        }
    }

    // 11b. 手牌已满 → card_rejected(card_draw_hand_full)，手牌不变。
    {
        phk::battle::BossRaceSimulation sim(MakeConfig());
        AddTwoPlayers(sim);
        sim.SeedHand(
            "p1",
            {"focus_lens", "purge_bell", "aegis_ward", "finale_arc"},
            {"focus_lens", "purge_bell", "aegis_ward", "finale_arc", "thunder_fall"},
            10.0,
            "42");
        sim.Tick();
        sim.SubmitInput(MakeCardDrawInput(2, 1));
        const phk::battle::BossRaceSnapshot snapshot = sim.Tick();
        Check(sim.HandSize("p1") == 4, "draw: a full hand is unchanged");
        const phk::battle::BossEventWire* rejected =
            FindEvent(snapshot, phk::battle::kCardRejectedEvent);
        Check(rejected != nullptr, "draw: a full hand is rejected");
        if (rejected != nullptr) {
            Check(
                rejected->reason == "card_draw_hand_full",
                "draw: full-hand reason is card_draw_hand_full"
            );
        }
        Check(
            CountEvents(snapshot, phk::battle::kCardDrawnEvent) == 0,
            "draw: no card_drawn row when the hand is full"
        );
    }

    // 11c. 牌堆为空 → card_rejected(card_draw_deck_empty)。
    {
        phk::battle::BossRaceSimulation sim(MakeConfig());
        AddTwoPlayers(sim);
        sim.SeedHand("p1", {"focus_lens"}, {}, 10.0, "42");
        sim.Tick();
        sim.SubmitInput(MakeCardDrawInput(2, 1));
        const phk::battle::BossRaceSnapshot snapshot = sim.Tick();
        Check(sim.HandSize("p1") == 1, "draw: an empty deck leaves the hand alone");
        const phk::battle::BossEventWire* rejected =
            FindEvent(snapshot, phk::battle::kCardRejectedEvent);
        Check(rejected != nullptr, "draw: an empty deck is rejected");
        if (rejected != nullptr) {
            Check(
                rejected->reason == "card_draw_deck_empty",
                "draw: empty-deck reason is card_draw_deck_empty"
            );
        }
    }
}

// ---------------------------------------------------------------------------
// 12. card_select 单独出现 = UI 选中态回显，服务端不改任何状态
// ---------------------------------------------------------------------------
void TestCardSelectAloneIsNoOp() {
    phk::battle::BossRaceSimulation sim(MakeConfig());
    AddTwoPlayers(sim);
    sim.SeedHand("p1", {"focus_lens", "purge_bell"}, {"focus_lens", "purge_bell"}, 5.0, "42");
    sim.Tick();  // tick 1

    phk::battle::BattleInput input = MakeCardInput(2, 1, -1);
    input.card_select = 1;  // 只选中，不使用
    Check(sim.SubmitInput(input), "select: card_select alone is accepted");
    const phk::battle::BossRaceSnapshot snapshot = sim.Tick();  // tick 2

    Check(sim.HandSize("p1") == 2, "select: the hand is untouched");
    Check(
        std::abs(sim.PlayerEnergy("p1") - (5.0 + 2.0 * phk::battle::kCardEnergyPerTick)) < 1e-9,
        "select: no energy is spent"
    );
    Check(
        CountEvents(snapshot, phk::battle::kCardAcceptedEvent) == 0 &&
            CountEvents(snapshot, phk::battle::kCardRejectedEvent) == 0,
        "select: selecting alone emits no card events"
    );
    const phk::battle::BossRacePlayerSnapshot* p1 = FindPlayer(snapshot, "p1");
    Check(
        p1 != nullptr && p1->hand.size() == 2 && !p1->hand[0].selected && !p1->hand[1].selected,
        "select: the server never mirrors a selection (hand[].selected stays false)"
    );
}

// ---------------------------------------------------------------------------
// 13. held fallback 不得重放出牌/抽卡
// ---------------------------------------------------------------------------
void TestHeldFallbackDoesNotRecast() {
    phk::battle::BossRaceSimulation sim(MakeConfig());
    AddTwoPlayers(sim);
    sim.SeedHand(
        "p1",
        {"purge_bell", "focus_lens"},
        {"purge_bell", "focus_lens", "aegis_ward"},
        10.0,
        "42");
    sim.Tick();  // tick 1

    // tick 2：真实输入出掉第 0 槽（随后自动补抽一张）。
    Check(sim.SubmitInput(MakeCardInput(2, 1, 0)), "held: real cast input is accepted");
    sim.Tick();
    const std::size_t hand_after_cast = sim.HandSize("p1");
    const double energy_after_cast = sim.PlayerEnergy("p1");
    Check(hand_after_cast == 2, "held: the cast consumed one card and redrew one");

    // tick 3/4/5：不再发包 → last_input 仍是 card_slot=0，但绝不能重放。
    for (int i = 0; i < 3; ++i) {
        const phk::battle::BossRaceSnapshot snapshot = sim.Tick();
        Check(
            CountEvents(snapshot, phk::battle::kCardAcceptedEvent) == 0,
            "held: a held fallback input never re-casts"
        );
    }
    Check(sim.HandSize("p1") == hand_after_cast, "held: the hand does not shrink from held fallback");
    Check(
        sim.PlayerEnergy("p1") > energy_after_cast,
        "held: energy only accrues (nothing is spent on held fallback)"
    );
}

// ---------------------------------------------------------------------------
// 14. 默认路径（无票据发牌）行为完全不变
// ---------------------------------------------------------------------------
void TestDefaultPathIgnoresCardSlot() {
    phk::battle::BossRaceSimulation sim(MakeConfig());
    AddTwoPlayers(sim);
    sim.Tick();  // tick 1

    // 没有 SeedHand：card_slot 意图必须被静默丢弃（接线前的行为）。
    Check(sim.SubmitInput(MakeCardInput(2, 1, 0)), "default: input itself is still accepted");
    const phk::battle::BossRaceSnapshot snapshot = sim.Tick();  // tick 2

    // 抽卡 / "使用"按钮同理：未发牌时整条卡牌管线关闭。
    Check(sim.SubmitInput(MakeCardDrawInput(3, 2)), "default: card_draw input is accepted");
    Check(sim.SubmitInput(MakeCardUseInput(4, 3, 0)), "default: card_use input is accepted");
    const phk::battle::BossRaceSnapshot later = sim.Tick();  // tick 3
    const phk::battle::BossRaceSnapshot latest = sim.Tick();  // tick 4
    Check(
        CountEvents(later, phk::battle::kCardRejectedEvent) == 0 &&
            CountEvents(latest, phk::battle::kCardRejectedEvent) == 0,
        "default: card_draw / card_use never produce card_rejected without a seeded hand"
    );

    Check(snapshot.hand.empty(), "default: snapshot hand stays empty");
    Check(snapshot.deck_remaining == 0, "default: deck_remaining stays 0");
    Check(snapshot.draw_cooldown_ticks == 0, "default: draw_cooldown_ticks stays 0");
    Check(
        CountEvents(snapshot, phk::battle::kCardAcceptedEvent) == 0 &&
            CountEvents(snapshot, phk::battle::kCardRejectedEvent) == 0 &&
            CountEvents(snapshot, phk::battle::kCardEffectAppliedEvent) == 0,
        "default: no card events at all"
    );
    Check(std::abs(sim.PlayerEnergy("p1") - 0.0) < kEps, "default: energy stays 0");
    Check(sim.LobbySeed() == 0, "default: lobby seed stays unset");
}

}  // namespace

int main() {
    TestSeedHandState();
    TestEnergyRegenPerTickAndCap();
    TestCastBooksLedger();
    TestSentinelPlaysNoCard();
    TestRejectionPaths();
    TestSnapshotHandShape();
    TestOpponentPatternSpawnsBullets();
    TestFinisherClearsFieldAndDamagesBoss();
    TestBulletPurgeUsesTicketSeed();
    TestCardUseButtonCastsSelectedSlot();
    TestCardDrawButton();
    TestCardSelectAloneIsNoOp();
    TestHeldFallbackDoesNotRecast();
    TestDefaultPathIgnoresCardSlot();

    if (g_failures != 0) {
        std::cerr << g_failures << " boss race card test(s) failed\n";
        return 1;
    }
    std::cout << "boss race card tests passed\n";
    return 0;
}
