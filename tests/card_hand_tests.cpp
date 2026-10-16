// 局内手牌状态与出牌校验测试——镜像 Go service.go:3301-3337 / 4229-4240
// 的每条拒绝路径与账本语义，并覆盖边界（冷却等值、能量 0.0001 容差、
// 空卡跳过、手牌上限、TrimSpace）。
//
// M-1（2026-10-14）边界规格（任务书）：slot 权威边界 = hand.size()。
//   - 2 张手牌：slot 2 / slot 7 拒绝（cast_card_slot_out_of_range）；
//     slot -1 = "不出牌"哨兵（accepted + plays_no_card，非拒绝）；
//     slot 0 / 1 接受。
//   - 4 张手牌：slot 3 接受；slot 4 拒绝。
//   - Go 原两级错误（outside hand >=4 / empty >=len(hand)）在 C++ 侧合并
//     为 cast_card_slot_out_of_range 一级；>3 由传输层先拦
//     （simulation.cpp:701 / :868-877）。
//
// 版权：东方同人逆向成果，仅用于自研玩法。

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "phk/battle/card_hand.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& label) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << label << "\n";
    }
}

using phk::battle::ApplyCast;
using phk::battle::BuildCardActivation;
using phk::battle::CastValidationResult;
using phk::battle::DrawServerCard;
using phk::battle::FindCardDefinition;
using phk::battle::HandState;
using phk::battle::kCardHandLimit;
using phk::battle::TrimCardId;
using phk::battle::ValidateCast;

HandState ArmedHand() {
    // 对应 Go 测试 armCard：手牌 = [mirror_lance, focus_lens x3]，能量 10。
    HandState hand;
    hand.card_ids = {"mirror_lance", "focus_lens", "focus_lens", "focus_lens"};
    hand.energy = 10.0;
    hand.deck = {"mirror_lance", "aegis_ward"};
    return hand;
}

// ---- 16 卡白名单目录 ----

void TestCatalogWhitelist() {
    const char* ids[16] = {
        "focus_lens", "hitbox_charm", "density_surge", "tempo_break",
        "bomb_amplifier", "guard_seal", "graze_engine", "draw_sigil",
        "aim_baffle", "purge_charm", "curve_prism", "last_arc",
        "mirror_lance", "purge_bell", "aegis_ward", "finale_arc",
    };
    for (const char* id : ids) {
        Check(FindCardDefinition(id) != nullptr, std::string("catalog must contain ") + id);
    }
    Check(FindCardDefinition("not_a_card") == nullptr, "catalog must reject unknown");
    Check(FindCardDefinition("") == nullptr, "catalog must reject empty");

    // 抽几张核对数值（service.go:405-420）。
    const auto* mirror = FindCardDefinition("mirror_lance");
    Check(mirror->effect_kind == "opponent_pattern" && mirror->cost == 4 && mirror->cooldown_ticks == 540 &&
              mirror->duration_ticks == 360 && mirror->damage == 26 && mirror->pattern_id == "mirror_lance",
          "mirror_lance definition");
    const auto* purge = FindCardDefinition("purge_bell");
    Check(purge->effect_kind == "bullet_purge" && purge->cost == 3 && purge->cooldown_ticks == 480 &&
              purge->duration_ticks == 1 && purge->purge_radius == 120 && purge->purge_count == 24,
          "purge_bell definition");
    const auto* aegis = FindCardDefinition("aegis_ward");
    Check(aegis->effect_kind == "invulnerable" && aegis->cost == 5 && aegis->duration_ticks == 180,
          "aegis_ward definition");
    const auto* draw = FindCardDefinition("draw_sigil");
    Check(draw->effect_kind == "economy" && draw->energy_gain == 0.75 && draw->draw_cards == 1,
          "draw_sigil definition");
}

// ---- 每条拒绝路径 ----

// M-1：4 张满手牌上，>hand.size()-1 的 slot 一律 out_of_range；-1 是哨兵。
void TestRejectionSlotOutOfRange() {
    HandState hand = ArmedHand();
    // -1 = "本 tick 不出牌"哨兵（protocol.hpp:49 struct default）：非拒绝、
    // 无卡定义、无拒绝码；ApplyCast 也不得改状态。
    const auto sentinel = ValidateCast(hand, -1, 1);
    Check(sentinel.accepted, "slot -1 sentinel accepted (not an error)");
    Check(sentinel.plays_no_card, "slot -1 plays_no_card");
    Check(sentinel.definition == nullptr, "slot -1 has no definition");
    Check(sentinel.reason.empty(), "slot -1 has no rejection reason");
    const HandState before = hand;
    const auto applied = ApplyCast(hand, -1, 1);
    Check(applied.accepted && applied.plays_no_card, "ApplyCast(-1) sentinel result");
    Check(hand.card_ids == before.card_ids && hand.energy == before.energy &&
              hand.cooldowns == before.cooldowns && hand.draw_cursor == before.draw_cursor,
          "ApplyCast(-1) must leave hand untouched");

    Check(ValidateCast(hand, kCardHandLimit, 1).reason == "cast_card_slot_out_of_range", "slot 4 rejected");
    Check(ValidateCast(hand, 99, 1).reason == "cast_card_slot_out_of_range", "slot 99 rejected");
}

// M-1：权威边界 = hand.size()。2 张手牌 → slot 2/7 拒、slot -1 哨兵、0/1 过。
void TestSlotBoundIsHandSize() {
    HandState hand;
    hand.card_ids = {"mirror_lance", "focus_lens"};
    hand.energy = 10.0;

    Check(ValidateCast(hand, 0, 1).accepted && !ValidateCast(hand, 0, 1).plays_no_card, "slot 0 accepted");
    Check(ValidateCast(hand, 1, 1).accepted && !ValidateCast(hand, 1, 1).plays_no_card, "slot 1 accepted");
    Check(ValidateCast(hand, 2, 1).reason == "cast_card_slot_out_of_range", "slot 2 rejected (2-card hand)");
    Check(ValidateCast(hand, 7, 1).reason == "cast_card_slot_out_of_range", "slot 7 rejected (2-card hand)");
    Check(ValidateCast(hand, 2, 1).plays_no_card == false, "slot 2 is a rejection, not the sentinel");

    const auto sentinel = ValidateCast(hand, -1, 1);
    Check(sentinel.accepted && sentinel.plays_no_card, "slot -1 sentinel plays nothing");
}

// M-1：4 张手牌 → slot 3 接受、slot 4 拒绝（旧代码同样成立，回归保护）。
void TestSlotBoundFullHand() {
    HandState hand = ArmedHand();
    Check(ValidateCast(hand, 3, 1).accepted && !ValidateCast(hand, 3, 1).plays_no_card, "slot 3 accepted (4-card hand)");
    Check(ValidateCast(hand, 4, 1).reason == "cast_card_slot_out_of_range", "slot 4 rejected (4-card hand)");
}

void TestRejectionUnknownCard() {
    HandState hand = ArmedHand();
    hand.card_ids[0] = "not_a_card";
    Check(ValidateCast(hand, 0, 1).reason == "cast_card_unknown_card", "unknown card rejected");
}

void TestRejectionOnCooldown() {
    HandState hand = ArmedHand();
    hand.cooldowns["mirror_lance"] = 100;  // 100 tick 前才能再出
    Check(ValidateCast(hand, 0, 50).reason == "cast_card_on_cooldown", "tick 50 < 100 rejected");
    Check(ValidateCast(hand, 0, 99).reason == "cast_card_on_cooldown", "tick 99 < 100 rejected");
    Check(ValidateCast(hand, 0, 100).accepted, "tick == 100 accepted (Go: tick < cooldown 才拒)");
}

void TestRejectionInsufficientEnergy() {
    HandState hand = ArmedHand();
    hand.energy = 3.0;  // mirror_lance cost 4
    Check(ValidateCast(hand, 0, 1).reason == "cast_card_insufficient_energy", "energy 3 < 4 rejected");

    hand.energy = 3.9999;  // +0.0001 容差边界：3.9999+0.0001 = 4.0 >= 4 → 通过
    Check(ValidateCast(hand, 0, 1).accepted, "energy 3.9999 within tolerance accepted");

    hand.energy = 3.9998;  // 3.9998+0.0001 = 3.9999 < 4 → 拒绝
    Check(ValidateCast(hand, 0, 1).reason == "cast_card_insufficient_energy", "energy 3.9998 rejected");
}

// ---- 成功路径账本 ----

void TestApplyCastBooksEnergyCooldownRemovalAndDraw() {
    HandState hand = ArmedHand();
    hand.card_ids = {"mirror_lance", "focus_lens"};
    hand.deck = {"aegis_ward"};

    const auto result = ApplyCast(hand, 0, 10);

    Check(result.accepted, "cast accepted");
    Check(result.card_id == "mirror_lance", "card id");
    Check(std::fabs(hand.energy - 6.0) < 1e-9, "energy 10-4=6");
    Check(hand.cooldowns["mirror_lance"] == 10 + 540, "cooldown = tick+540");
    Check(hand.card_ids.size() == 2u && hand.card_ids[0] == "focus_lens" && hand.card_ids[1] == "aegis_ward",
          "hand: removed slot 0, drew aegis_ward");
    Check(hand.draw_cursor == 1, "draw cursor advanced");
}

void TestApplyCastEnergyFloorAtZero() {
    HandState hand = ArmedHand();
    hand.energy = 3.9999;  // cost 4 的容差内通过；扣费后 max(0, -0.0001) = 0
    const auto result = ApplyCast(hand, 0, 1);
    Check(result.accepted, "accepted within tolerance");
    Check(hand.energy == 0.0, "energy floored at 0");
}

void TestApplyCastNoDeckNoRefill() {
    HandState hand = ArmedHand();
    hand.card_ids = {"mirror_lance"};
    hand.deck = {};
    const auto result = ApplyCast(hand, 0, 1);
    Check(result.accepted && hand.card_ids.empty() && hand.draw_cursor == 0, "no deck: no refill");
}

void TestApplyCastRejectedLeavesHandUntouched() {
    HandState hand = ArmedHand();
    hand.cooldowns["mirror_lance"] = 500;
    const auto before = hand;
    const auto result = ApplyCast(hand, 0, 1);
    Check(!result.accepted, "rejected");
    Check(hand.card_ids == before.card_ids && hand.energy == before.energy && hand.draw_cursor == before.draw_cursor,
          "rejected cast must not mutate hand");
}

// TrimSpace：手牌里的空白 id 仍可出牌（Go strings.TrimSpace）。
void TestApplyCastTrimsWhitespace() {
    HandState hand = ArmedHand();
    hand.card_ids[0] = "  mirror_lance\t";
    const auto result = ApplyCast(hand, 0, 1);
    Check(result.accepted && result.card_id == "mirror_lance", "trimmed card accepted");
    Check(hand.cooldowns.count("mirror_lance") == 1u, "cooldown keyed on trimmed id");
    Check(TrimCardId(" \t\n x \r ") == "x", "TrimCardId basic");
    Check(TrimCardId("   ").empty(), "TrimCardId all-space");
}

// ---- drawServerCard ----

void TestDrawServerCardHandCap() {
    HandState hand = ArmedHand();  // 手牌已满 4 张
    Check(!DrawServerCard(hand), "full hand: no draw");
    Check(hand.draw_cursor == 0, "cursor not advanced on full hand");
}

void TestDrawServerCardSkipsEmptyCardId() {
    HandState hand;
    hand.card_ids = {"mirror_lance"};
    hand.deck = {"", "focus_lens"};
    Check(!DrawServerCard(hand), "empty deck entry skipped (cursor 0)");
    Check(hand.draw_cursor == 1, "cursor advanced past empty");
    Check(DrawServerCard(hand), "second draw succeeds");
    Check(hand.card_ids.size() == 2u && hand.card_ids[1] == "focus_lens", "focus_lens drawn");
}

void TestDrawServerCardCyclesDeck() {
    HandState hand;
    hand.deck = {"a", "b"};
    Check(DrawServerCard(hand) && DrawServerCard(hand) && DrawServerCard(hand) && DrawServerCard(hand), "4 draws");
    Check(hand.card_ids.size() == 4u && hand.card_ids[0] == "a" && hand.card_ids[1] == "b" &&
              hand.card_ids[2] == "a" && hand.card_ids[3] == "b",
          "deck cycles a,b,a,b");
}

// ---- BuildCardActivation 桥接 ----

void TestBuildCardActivation() {
    const auto* mirror = FindCardDefinition("mirror_lance");
    const auto activation = BuildCardActivation(*mirror, 100);
    Check(activation.card_id == "mirror_lance" && activation.effect_kind == "opponent_pattern", "activation kind");
    Check(activation.pattern_id == "mirror_lance", "activation pattern");
    Check(activation.expires_tick == 100 + 360, "activation expires = tick+duration");

    const auto* purge = FindCardDefinition("purge_bell");
    const auto purge_activation = BuildCardActivation(*purge, 100);
    Check(purge_activation.effect_kind == "bullet_purge" && purge_activation.purge_radius == 120 &&
              purge_activation.purge_count == 24 && purge_activation.expires_tick == 101,
          "purge activation");
}

}  // namespace

// W-1 phase 2: the lobby seeds HandState like SeedPlayerHand does for a deck
// ticket (deck = full ordered deck, draw_cursor = hand size); a cast must
// redraw deck[draw_cursor] and advance the cursor exactly once, matching the
// Go drawServerCard semantics.
void TestDeckSeededHandRedrawResumesAtLobbyCursor() {
    HandState hand;
    hand.deck = {
        "mirror_lance", "focus_lens", "graze_engine", "guard_seal",
        "aegis_ward", "finale_arc"};
    hand.card_ids = {
        hand.deck[0], hand.deck[1], hand.deck[2], hand.deck[3]};
    hand.energy = 10.0;
    hand.draw_cursor = hand.card_ids.size();
    const auto result = ApplyCast(hand, 0, 100);
    Check(result.accepted, "deck-seeded cast accepted");
    Check(hand.card_ids.size() == 4, "deck-seeded hand redraws to the limit");
    Check(hand.card_ids.back() == "aegis_ward", "redraw resumes at the lobby cursor (deck[4])");
    Check(hand.draw_cursor == 5, "cursor advanced exactly once");
}

int main() {
    TestCatalogWhitelist();
    TestRejectionSlotOutOfRange();
    TestSlotBoundIsHandSize();
    TestSlotBoundFullHand();
    TestRejectionUnknownCard();
    TestRejectionOnCooldown();
    TestRejectionInsufficientEnergy();
    TestApplyCastBooksEnergyCooldownRemovalAndDraw();
    TestApplyCastEnergyFloorAtZero();
    TestApplyCastNoDeckNoRefill();
    TestApplyCastRejectedLeavesHandUntouched();
    TestApplyCastTrimsWhitespace();
    TestDrawServerCardHandCap();
    TestDrawServerCardSkipsEmptyCardId();
    TestDrawServerCardCyclesDeck();
    TestDeckSeededHandRedrawResumesAtLobbyCursor();
    TestBuildCardActivation();

    if (g_failures != 0) {
        std::cerr << g_failures << " card_hand test(s) failed\n";
        return 1;
    }
    std::cout << "card_hand tests passed\n";
    return 0;
}
