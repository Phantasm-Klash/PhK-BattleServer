// 局内手牌状态与出牌校验测试——镜像 Go service.go:3301-3337 / 4229-4240
// 的每条拒绝路径与账本语义，并覆盖边界（冷却等值、能量 0.0001 容差、
// 空卡跳过、手牌上限、TrimSpace）。
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

void TestRejectionSlotOutOfRange() {
    HandState hand = ArmedHand();
    // M-1：-1 是"不出牌"哨兵，不是拒绝（no_card=true、reason 为空）。
    const auto sentinel = ValidateCast(hand, -1, 1);
    Check(sentinel.no_card && !sentinel.accepted && sentinel.reason.empty(),
          "slot -1: no-card sentinel, not a rejection");
    Check(ValidateCast(hand, kCardHandLimit, 1).reason == "cast_card_slot_out_of_range", "slot 4 rejected");
    Check(ValidateCast(hand, 99, 1).reason == "cast_card_slot_out_of_range", "slot 99 rejected");
}

// M-1（T7 §3）：cast 校验以 hand.size() 为界；任何 >= hand.size() 的槽位
// 都走拒绝路径（out_of_range / empty 两段共同构成上界），杜绝越界索引手牌。
void TestCastSlotBoundedByHandSize() {
    HandState two;
    two.card_ids = {"mirror_lance", "focus_lens"};
    two.energy = 10.0;

    Check(ValidateCast(two, -1, 1).no_card, "2-card hand: slot -1 = no card");
    Check(ValidateCast(two, 0, 1).accepted, "2-card hand: slot 0 accepted");
    Check(ValidateCast(two, 1, 1).accepted, "2-card hand: slot 1 accepted");
    Check(ValidateCast(two, 2, 1).reason == "cast_card_slot_empty", "2-card hand: slot 2 rejected (empty)");
    Check(ValidateCast(two, 7, 1).reason == "cast_card_slot_out_of_range", "2-card hand: slot 7 rejected (out_of_range)");
    Check(!ValidateCast(two, 2, 1).accepted && !ValidateCast(two, 7, 1).accepted, "2-card hand: slots 2/7 both rejected");

    const auto applied = ApplyCast(two, -1, 1);
    Check(applied.no_card && two.card_ids.size() == 2u && two.energy == 10.0 && two.draw_cursor == 0,
          "sentinel cast leaves hand untouched");

    HandState four = ArmedHand();  // 4 张满手
    Check(ValidateCast(four, 3, 1).accepted, "4-card hand: slot 3 accepted");
    Check(ValidateCast(four, 4, 1).reason == "cast_card_slot_out_of_range", "4-card hand: slot 4 rejected (out_of_range)");
    Check(!ValidateCast(four, 4, 1).accepted, "4-card hand: slot 4 rejected");
}

void TestRejectionSlotEmpty() {
    HandState hand;
    hand.card_ids = {"mirror_lance"};  // 手牌只有 1 张，槽 1..3 为空
    hand.energy = 10.0;
    Check(ValidateCast(hand, 0, 1).accepted, "slot 0 accepted with 1 card");
    Check(ValidateCast(hand, 1, 1).reason == "cast_card_slot_empty", "slot 1 rejected empty");
    Check(ValidateCast(hand, 3, 1).reason == "cast_card_slot_empty", "slot 3 rejected empty");
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

int main() {
    TestCatalogWhitelist();
    TestRejectionSlotOutOfRange();
    TestCastSlotBoundedByHandSize();
    TestRejectionSlotEmpty();
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
    TestBuildCardActivation();

    if (g_failures != 0) {
        std::cerr << g_failures << " card_hand test(s) failed\n";
        return 1;
    }
    std::cout << "card_hand tests passed\n";
    return 0;
}
