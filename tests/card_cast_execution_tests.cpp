// 出牌执行器 tick 集成测试（T3 step4）。
//
// 全部经**真实入口**（AcceptInput / AcceptModeAction + Tick()），不直测
// card_hand/card_effects 纯函数——本文件验的是「校验后的出牌意图在 tick 内
// 被执行」这条接线：
//   - 输入通道（Go 同构）：真实客户端输入的 card_slot >= 0 → 执行；
//     -1 = 不出牌哨兵（无事件行、无状态变更、无子弹）。
//   - 动作通道：cast_card mode action → ApplyModeActionsForTick 内执行。
// 拒绝路径记 card_rejected；通过记 card_accepted + card_effect_applied。
//
// 事件行只进 CardCastEvents() 收集器，不进 event_trace_/event_stream_hash_
// /event_count_（见 task-queue/out/T3_step4_integration.md §5）。
//
// 版权：东方同人逆向成果，仅用于自研玩法。

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "phk/battle/simulation.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& label) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << label << "\n";
    }
}

using phk::battle::BattleInput;
using phk::battle::BattleModeAction;
using phk::battle::BattleSimulation;
using phk::battle::SimulationConfig;

constexpr const char* kMatchId = "match-cast-001";

SimulationConfig DuelConfig() {
    // 非 boss 模式：battle_input_enabled 恒真，无需 ready 流程。
    SimulationConfig config;
    config.match_id = kMatchId;
    config.mode_id = "duel";
    config.match_seed = 777;
    config.spawn_period_ticks = 5;
    config.max_bullets = 256;
    return config;
}

BattleInput MakeInput(
    const std::string& player_id,
    std::uint64_t tick,
    std::uint64_t seq,
    std::int32_t card_slot
) {
    BattleInput input;
    input.match_id = kMatchId;
    input.player_id = player_id;
    input.tick = tick;
    input.seq = seq;
    input.direction_bits = 0;
    input.slow = false;
    input.shoot = false;
    input.card_slot = card_slot;  // -1 = 不出牌哨兵（struct default）
    return input;
}

BattleModeAction MakeCastAction(
    const std::string& player_id,
    std::uint64_t seq,
    std::uint64_t tick,
    int slot
) {
    BattleModeAction action;
    action.match_id = kMatchId;
    action.player_id = player_id;
    action.tick = tick;
    action.seq = seq;
    action.action_id = "cast-" + player_id + "-" + std::to_string(seq);
    action.action_type = "cast_card";
    action.payload_json = "{\"card_slot\":" + std::to_string(slot) + "}";
    return action;
}

// 行查找助手：返回第一个指定类型的行指针（无则 nullptr）。
const phk::battle::CardCastEvent* FindEvent(
    const BattleSimulation& simulation,
    const std::string& event_type
) {
    for (const auto& row : simulation.CardCastEvents()) {
        if (row.event_type == event_type) {
            return &row;
        }
    }
    return nullptr;
}

std::size_t CountEvents(const BattleSimulation& simulation, const std::string& event_type) {
    std::size_t count = 0;
    for (const auto& row : simulation.CardCastEvents()) {
        if (row.event_type == event_type) {
            ++count;
        }
    }
    return count;
}

// ---- 播种 + -1 哨兵（输入通道） ----

void TestSeedAndSentinel() {
    BattleSimulation simulation(DuelConfig());
    Check(simulation.AddPlayer("p1", -20000, 0), "add p1");
    Check(simulation.AddPlayer("p2", 20000, 0), "add p2");
    Check(
        simulation.SeedPlayerHand(
            "p1",
            {"mirror_lance", "focus_lens", "focus_lens", "focus_lens"},
            10.0,
            "1234567890123456789"
        ),
        "seed p1 hand (seed > 2^53 as decimal text)"
    );
    Check(simulation.HandSize("p1") == 4, "hand size seeded to 4");
    Check(simulation.PlayerEnergy("p1") == 10.0, "energy seeded to 10");
    Check(simulation.LobbySeed() == 1234567890123456789LL, "lobby seed parsed from decimal text");
    Check(simulation.HandSize("p2") == 0, "p2 unseeded hand empty");
    Check(
        !simulation.SeedPlayerHand("p9", {"focus_lens"}, 1.0, "5"),
        "seeding unknown player rejected"
    );
    Check(
        !simulation.SeedPlayerHand("p2", {}, 1.0, "999"),
        "conflicting match seed rejected (first wins)"
    );
    Check(simulation.LobbySeed() == 1234567890123456789LL, "lobby seed unchanged after conflict");

    // -1 哨兵：真实输入、本 tick 不出牌 → 无事件行、无状态变更、无子弹。
    const auto before_bullets = simulation.BulletCount();
    Check(simulation.AcceptInput(MakeInput("p1", 1, 1, -1)).ok, "accept sentinel input");
    simulation.Tick();
    Check(simulation.CardCastEvents().empty(), "sentinel produces no event rows");
    Check(simulation.HandSize("p1") == 4, "sentinel leaves hand untouched");
    Check(simulation.PlayerEnergy("p1") == 10.0, "sentinel leaves energy untouched");
    Check(simulation.BulletCount() == before_bullets, "sentinel spawns no bullets");

    // 显式 cast_card 动作的 -1 在传输层即被拒（到不了执行器）。
    const auto sentinel_action = simulation.AcceptModeAction(MakeCastAction("p1", 2, 2, -1));
    Check(!sentinel_action.ok, "cast_card action with slot -1 rejected at transport");
    Check(sentinel_action.reason == "cast_card_slot_invalid", "reject reason cast_card_slot_invalid");
    Check(simulation.CardCastEvents().empty(), "transport-rejected action produces no rows");
}

// ---- 四张效果卡：动作通道合法 cast ----

void TestMirrorLanceCastViaModeAction() {
    BattleSimulation simulation(DuelConfig());
    Check(simulation.AddPlayer("p1", -20000, 0), "add p1");
    Check(simulation.AddPlayer("p2", 20000, 0), "add p2");
    Check(
        simulation.SeedPlayerHand(
            "p1",
            {"mirror_lance", "focus_lens", "focus_lens", "focus_lens"},
            10.0,
            "42"
        ),
        "seed p1 hand"
    );

    Check(simulation.AcceptModeAction(MakeCastAction("p1", 1, 1, 0)).ok, "accept cast slot 0");
    simulation.Tick();

    const auto accepted = FindEvent(simulation, "card_accepted");
    Check(accepted != nullptr, "card_accepted row emitted");
    if (accepted != nullptr) {
        Check(accepted->player_id == "p1", "card_accepted player");
        Check(accepted->card_id == "mirror_lance", "card_accepted card id");
        Check(accepted->event_value == 26, "card_accepted value = card damage (26)");
        Check(accepted->slot == 0, "card_accepted slot = 0");
        Check(accepted->tick == 1, "card_accepted tick = 1");
    }
    const auto effect = FindEvent(simulation, "card_effect_applied");
    Check(effect != nullptr, "card_effect_applied row emitted");
    if (effect != nullptr) {
        Check(effect->card_id == "mirror_lance", "effect row card id");
        Check(effect->event_value == 7, "opponent_pattern spawned 7 (event_value)");
        Check(effect->target_user_id == "p2", "opponent_pattern targets lowest other id");
    }
    Check(simulation.HandSize("p1") == 3, "played slot removed (deck empty: no draw)");
    Check(simulation.PlayerEnergy("p1") == 6.0, "energy 10 - cost 4");
    Check(CountEvents(simulation, "card_rejected") == 0, "no rejection on legal cast");
}

void TestPurgeBellCastViaInputChannel() {
    BattleSimulation simulation(DuelConfig());
    Check(simulation.AddPlayer("p1", 0, 0), "add p1 at origin");
    Check(simulation.AddPlayer("p2", 20000, 0), "add p2");
    Check(
        simulation.SeedPlayerHand("p1", {"purge_bell", "focus_lens", "focus_lens", "focus_lens"}, 10.0, "7"),
        "seed p1 hand"
    );

    // tick 5：SpawnBulletsForTick 在 (0,0) 附近生成 4 颗 duel 子弹。
    simulation.Tick();  // tick 1..4 无生成（spawn_period_ticks=5）
    simulation.Tick();
    simulation.Tick();
    simulation.Tick();
    simulation.Tick();
    const std::size_t spawned = simulation.BulletCount();
    Check(spawned == 4, "4 bullets spawned at tick 5");
    Check(
        simulation.BulletCount() >= 4,
        "purge preconditions: bullets on field"
    );

    // 输入通道：真实输入 card_slot=0 出 purge_bell（caster 在 (0,0)，全在半径内）。
    Check(simulation.AcceptInput(MakeInput("p1", 6, 1, 0)).ok, "accept cast input slot 0");
    simulation.Tick();  // tick 6 执行

    const auto accepted = FindEvent(simulation, "card_accepted");
    Check(accepted != nullptr && accepted->card_id == "purge_bell", "card_accepted purge_bell");
    const auto effect = FindEvent(simulation, "card_effect_applied");
    Check(effect != nullptr, "card_effect_applied row emitted");
    if (effect != nullptr) {
        Check(effect->event_value == static_cast<std::int64_t>(spawned), "purge removed all in-radius bullets");
    }
    Check(simulation.BulletCount() == 0, "field cleared by purge");
    Check(simulation.PlayerEnergy("p1") == 7.0, "energy 10 - cost 3");
    Check(simulation.HandSize("p1") == 3, "played slot removed");
}

void TestAegisWardCastViaModeAction() {
    BattleSimulation simulation(DuelConfig());
    Check(simulation.AddPlayer("p1", -20000, 0), "add p1");
    Check(simulation.AddPlayer("p2", 20000, 0), "add p2");
    Check(
        simulation.SeedPlayerHand("p1", {"aegis_ward", "focus_lens", "focus_lens", "focus_lens"}, 10.0, "9"),
        "seed p1 hand"
    );

    Check(simulation.AcceptModeAction(MakeCastAction("p1", 1, 1, 0)).ok, "accept cast slot 0");
    simulation.Tick();

    const auto effect = FindEvent(simulation, "card_effect_applied");
    Check(effect != nullptr && effect->card_id == "aegis_ward", "card_effect_applied aegis_ward");
    Check(effect != nullptr && effect->event_value == 0, "invulnerable event_value = 0");
    Check(
        simulation.PlayerInvulnerableUntil("p1") == 1 + 180,
        "invulnerable_until = tick + duration (181)"
    );
    Check(simulation.PlayerEnergy("p1") == 5.0, "energy 10 - cost 5");
}

void TestFinaleArcClearsField() {
    BattleSimulation simulation(DuelConfig());
    Check(simulation.AddPlayer("p1", 0, 0), "add p1");
    Check(simulation.AddPlayer("p2", 20000, 0), "add p2");
    Check(
        simulation.SeedPlayerHand("p1", {"finale_arc", "focus_lens", "focus_lens", "focus_lens"}, 10.0, "11"),
        "seed p1 hand"
    );

    for (int i = 0; i < 5; ++i) {
        simulation.Tick();  // tick 5 生成 4 颗子弹
    }
    const std::int64_t field = static_cast<std::int64_t>(simulation.BulletCount());
    Check(field == 4, "4 bullets on field before finisher");

    // 出牌 tick = 6。
    Check(simulation.AcceptModeAction(MakeCastAction("p1", 1, 6, 0)).ok, "accept cast at tick 6");
    simulation.Tick();

    const auto effect = FindEvent(simulation, "card_effect_applied");
    Check(effect != nullptr && effect->card_id == "finale_arc", "card_effect_applied finale_arc");
    Check(effect != nullptr && effect->event_value == field, "finisher cleared every bullet (event_value = field)");
    Check(simulation.BulletCount() == 0, "field cleared by finisher");
    Check(simulation.PlayerEnergy("p1") == 4.0, "energy 10 - cost 6");
}

// ---- 拒绝路径（每条例断 card_rejected 行 + 状态零变更） ----

void TestRejectionPaths() {
    // 未知卡（目录外 id 由票据校验保证不会下发，执行器仍须拒绝）。
    {
        BattleSimulation simulation(DuelConfig());
        Check(simulation.AddPlayer("p1", 0, 0), "add p1");
        Check(simulation.SeedPlayerHand("p1", {"not_a_card"}, 10.0, "3"), "seed unknown card hand");
        Check(simulation.AcceptModeAction(MakeCastAction("p1", 1, 1, 0)).ok, "accept cast");
        simulation.Tick();
        const auto rejected = FindEvent(simulation, "card_rejected");
        Check(rejected != nullptr, "unknown card -> card_rejected");
        Check(
            rejected != nullptr && rejected->reason == "cast_card_unknown_card",
            "unknown card reason"
        );
        Check(simulation.HandSize("p1") == 1, "rejected cast leaves hand untouched");
        Check(simulation.PlayerEnergy("p1") == 10.0, "rejected cast leaves energy untouched");
        Check(CountEvents(simulation, "card_accepted") == 0, "no card_accepted on rejection");
    }
    // slot 越界（手牌 1 张，slot 3）。
    {
        BattleSimulation simulation(DuelConfig());
        Check(simulation.AddPlayer("p1", 0, 0), "add p1");
        Check(simulation.SeedPlayerHand("p1", {"focus_lens"}, 10.0, "3"), "seed hand size 1");
        Check(simulation.AcceptModeAction(MakeCastAction("p1", 1, 1, 3)).ok, "accept cast slot 3");
        simulation.Tick();
        const auto rejected = FindEvent(simulation, "card_rejected");
        Check(
            rejected != nullptr && rejected->reason == "cast_card_slot_out_of_range",
            "slot out of range reason"
        );
        Check(simulation.HandSize("p1") == 1, "rejected cast leaves hand untouched");
    }
    // 空手牌（slot 0 亦越界）。
    {
        BattleSimulation simulation(DuelConfig());
        Check(simulation.AddPlayer("p1", 0, 0), "add p1");
        Check(simulation.SeedPlayerHand("p1", {}, 10.0, "3"), "seed empty hand");
        Check(simulation.AcceptModeAction(MakeCastAction("p1", 1, 1, 0)).ok, "accept cast slot 0");
        simulation.Tick();
        const auto rejected = FindEvent(simulation, "card_rejected");
        Check(
            rejected != nullptr && rejected->reason == "cast_card_slot_out_of_range",
            "empty hand -> out of range"
        );
    }
    // 冷却中（同卡二次出牌）。
    {
        BattleSimulation simulation(DuelConfig());
        Check(simulation.AddPlayer("p1", 0, 0), "add p1");
        Check(simulation.AddPlayer("p2", 20000, 0), "add p2");
        Check(
            simulation.SeedPlayerHand("p1", {"mirror_lance", "mirror_lance"}, 10.0, "3"),
            "seed two mirror_lance"
        );
        Check(simulation.AcceptModeAction(MakeCastAction("p1", 1, 1, 0)).ok, "accept first cast");
        simulation.Tick();
        Check(FindEvent(simulation, "card_accepted") != nullptr, "first cast accepted");
        simulation.ClearCardCastEvents();
        Check(simulation.AcceptModeAction(MakeCastAction("p1", 2, 2, 0)).ok, "accept second cast");
        simulation.Tick();
        const auto rejected = FindEvent(simulation, "card_rejected");
        Check(
            rejected != nullptr && rejected->reason == "cast_card_on_cooldown",
            "second cast on cooldown"
        );
    }
    // 能量不足。
    {
        BattleSimulation simulation(DuelConfig());
        Check(simulation.AddPlayer("p1", 0, 0), "add p1");
        Check(
            simulation.SeedPlayerHand("p1", {"mirror_lance"}, 0.0, "3"),
            "seed zero energy"
        );
        Check(simulation.AcceptModeAction(MakeCastAction("p1", 1, 1, 0)).ok, "accept cast");
        simulation.Tick();
        const auto rejected = FindEvent(simulation, "card_rejected");
        Check(
            rejected != nullptr && rejected->reason == "cast_card_insufficient_energy",
            "insufficient energy reason"
        );
        Check(simulation.HandSize("p1") == 1, "rejected cast leaves hand untouched");
    }
}

// ---- boss 伤害与 boss_defeated 行（lobby 已识别并路由 draft） ----

void TestBossDamageAndDefeatRow() {
    SimulationConfig config;
    config.match_id = kMatchId;
    config.mode_id = "world_boss";
    config.match_seed = 555;
    config.boss_max_hp = 100;  // finale_arc(60) + last_arc(48) 足够击杀
    config.spawn_period_ticks = 60;
    BattleSimulation simulation(config);
    for (int index = 1; index <= 4; ++index) {
        Check(
            simulation.AddPlayer("p" + std::to_string(index), 0, 0),
            "add boss player p" + std::to_string(index)
        );
    }
    for (int index = 1; index <= 4; ++index) {
        BattleModeAction ready;
        ready.match_id = kMatchId;
        ready.player_id = "p" + std::to_string(index);
        ready.tick = 1;
        ready.seq = static_cast<std::uint64_t>(index);
        ready.action_id = "ready-" + std::to_string(index);
        ready.action_type = "ready";
        ready.payload_json = "{\"ready\":true}";
        Check(simulation.AcceptModeAction(ready).ok, "accept ready p" + std::to_string(index));
    }
    // 两张不同的 finisher（冷却按 card_id 独立，可连出）：
    // finale_arc 60 伤害 / last_arc 48 伤害。
    Check(
        simulation.SeedPlayerHand("p1", {"finale_arc", "last_arc"}, 10.0, "31337"),
        "seed p1 finisher hand"
    );

    simulation.Tick();  // tick 1：ready 生效，boss 战斗开始
    const auto summary1 = simulation.Summary();
    Check(summary1.boss_current_hp == 100, "boss full hp after ready tick");

    // 第一击（输入通道，tick 2）：finale_arc 60 伤害，boss 剩 40。
    // 注意：p1 的 last_seq 已被 tick 1 的 ready 动作推进到 1，输入 seq 必须 > 1。
    Check(simulation.AcceptInput(MakeInput("p1", 2, 2, 0)).ok, "accept cast input at tick 2");
    simulation.Tick();  // tick 2
    const auto summary2 = simulation.Summary();
    Check(summary2.boss_current_hp == 40, "boss hp 100 - 60 = 40");
    Check(summary2.boss_damage_total == 60, "boss damage total 60");
    Check(CountEvents(simulation, "boss_defeated") == 0, "no boss_defeated yet");

    // 第二击（tick 3）：重新播种手牌+能量（模拟下一轮能量回复；冷却按
    // card_id 独立，last_arc 未冷却）后出 last_arc，48 伤害 clamp 到剩余
    // 40 → 击杀。
    Check(
        simulation.SeedPlayerHand("p1", {"last_arc"}, 10.0, "31337"),
        "re-seed p1 with last_arc"
    );
    Check(simulation.AcceptInput(MakeInput("p1", 3, 3, 0)).ok, "accept second cast input at tick 3");
    simulation.Tick();  // tick 3：执行第二击
    const auto summary3 = simulation.Summary();
    Check(summary3.boss_current_hp == 0, "boss hp clamped to 0");
    Check(summary3.boss_damage_total == 100, "damage clamped to remaining hp");
    Check(summary3.boss_defeated == 1, "boss defeated flag");
    Check(summary3.boss_defeated_tick == 3, "boss defeated tick = 3");
    const auto defeated = FindEvent(simulation, "boss_defeated");
    Check(defeated != nullptr, "boss_defeated row emitted");
    if (defeated != nullptr) {
        Check(defeated->reason.find("world-boss:") == 0, "boss_defeated reason carries boss instance id");
        Check(defeated->tick == 3, "boss_defeated tick");
        Check(defeated->player_id == "p1", "boss_defeated player");
    }
    bool trace_has_defeat = false;
    for (const auto& line : summary3.event_trace) {
        if (line.find("boss_defeated|") == 0) {
            trace_has_defeat = true;
        }
    }
    Check(trace_has_defeat, "boss_defeated also in replay event trace");
}

}  // namespace

int main() {
    TestSeedAndSentinel();
    TestMirrorLanceCastViaModeAction();
    TestPurgeBellCastViaInputChannel();
    TestAegisWardCastViaModeAction();
    TestFinaleArcClearsField();
    TestRejectionPaths();
    TestBossDamageAndDefeatRow();

    if (g_failures != 0) {
        std::cerr << g_failures << " card cast execution test(s) failed\n";
        return 1;
    }
    std::cout << "card cast execution tests passed\n";
    return 0;
}
