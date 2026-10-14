// 局内手牌状态与出牌校验实现。对照 service.go:405-420 / 3301-3337 / 4229-4240。
//
// 版权：东方同人逆向成果，仅用于自研玩法。

#include "phk/battle/card_hand.hpp"

#include <algorithm>
#include <cctype>

namespace phk::battle {
namespace {

// serverCardCatalog（service.go:405-420）——16 张，字段顺序：
// card_id / effect_kind / cost / cooldown / duration / damage / score_bonus /
// energy_gain / draw_cards / pattern_id / purge_radius / purge_count。
const CardDefinition kCatalog[] = {
    {"focus_lens", "self", 2, 240, 360, 12, 18, 0, 0, "", 0, 0},
    {"hitbox_charm", "self", 3, 420, 300, 14, 20, 0, 0, "", 0, 0},
    {"density_surge", "pattern", 4, 540, 420, 30, 32, 0, 0, "", 0, 0},
    {"tempo_break", "pattern", 3, 480, 300, 22, 26, 0, 0, "", 0, 0},
    {"bomb_amplifier", "self", 2, 360, 480, 16, 18, 0, 0, "", 0, 0},
    {"guard_seal", "self", 5, 720, 900, 18, 30, 0, 0, "", 0, 0},
    {"graze_engine", "economy", 2, 300, 360, 10, 20, 0, 0, "", 0, 0},
    {"draw_sigil", "economy", 1, 420, 1, 0, 12, 0.75, 1, "", 0, 0},
    {"aim_baffle", "pattern", 4, 600, 360, 28, 32, 0, 0, "", 0, 0},
    {"purge_charm", "self", 3, 540, 480, 18, 24, 0, 0, "", 0, 0},
    {"curve_prism", "pattern", 3, 500, 360, 24, 28, 0, 0, "", 0, 0},
    {"last_arc", "finisher", 6, 900, 420, 48, 60, 0, 0, "", 0, 0},
    {"mirror_lance", "opponent_pattern", 4, 540, 360, 26, 30, 0, 0, "mirror_lance", 0, 0},
    {"purge_bell", "bullet_purge", 3, 480, 1, 0, 22, 0, 0, "", 120, 24},
    {"aegis_ward", "invulnerable", 5, 720, 180, 0, 20, 0, 0, "", 0, 0},
    {"finale_arc", "finisher", 6, 900, 1, 60, 80, 0, 0, "", 0, 0},
};

}  // namespace

const CardDefinition* FindCardDefinition(const std::string& card_id) {
    for (const CardDefinition& definition : kCatalog) {
        if (definition.card_id == card_id) {
            return &definition;
        }
    }
    return nullptr;
}

std::string TrimCardId(const std::string& value) {
    const char* whitespace = " \t\n\r\v\f";
    const std::size_t first = value.find_first_not_of(whitespace);
    if (first == std::string::npos) {
        return "";
    }
    const std::size_t last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

CastValidationResult ValidateCast(const HandState& hand, int slot, std::int64_t tick) {
    CastValidationResult result;
    if (slot < 0 || slot >= kCardHandLimit) {
        result.reason = "cast_card_slot_out_of_range";
        return result;
    }
    if (static_cast<std::size_t>(slot) >= hand.card_ids.size()) {
        result.reason = "cast_card_slot_empty";
        return result;
    }
    const std::string card_id = TrimCardId(hand.card_ids[static_cast<std::size_t>(slot)]);
    const CardDefinition* definition = FindCardDefinition(card_id);
    if (definition == nullptr) {
        result.reason = "cast_card_unknown_card";
        return result;
    }
    const auto cooldown = hand.cooldowns.find(card_id);
    if (cooldown != hand.cooldowns.end() && tick < cooldown->second) {
        result.reason = "cast_card_on_cooldown";
        return result;
    }
    if (hand.energy + 0.0001 < definition->cost) {
        result.reason = "cast_card_insufficient_energy";
        return result;
    }
    result.accepted = true;
    result.definition = definition;
    result.card_id = card_id;
    result.removed_slot = static_cast<std::size_t>(slot);
    result.energy_after = std::max(0.0, hand.energy - definition->cost);
    result.cooldown_until = tick + definition->cooldown_ticks;
    return result;
}

bool DrawServerCard(HandState& hand) {
    if (hand.card_ids.size() >= static_cast<std::size_t>(kCardHandLimit) || hand.deck.empty()) {
        return false;
    }
    const std::string card_id = TrimCardId(hand.deck[hand.draw_cursor % hand.deck.size()]);
    ++hand.draw_cursor;
    if (card_id.empty()) {
        return false;
    }
    hand.card_ids.push_back(card_id);
    return true;
}

CastValidationResult ApplyCast(HandState& hand, int slot, std::int64_t tick) {
    const CastValidationResult result = ValidateCast(hand, slot, tick);
    if (!result.accepted) {
        return result;
    }
    hand.energy = result.energy_after;
    hand.cooldowns[result.card_id] = result.cooldown_until;
    hand.card_ids.erase(hand.card_ids.begin() + static_cast<std::ptrdiff_t>(result.removed_slot));
    DrawServerCard(hand);
    return result;
}

CardActivation BuildCardActivation(const CardDefinition& definition, std::int64_t tick) {
    CardActivation activation;
    activation.card_id = definition.card_id;
    activation.effect_kind = definition.effect_kind;
    activation.pattern_id = definition.pattern_id;
    activation.expires_tick = tick + definition.duration_ticks;
    activation.purge_radius = definition.purge_radius;
    activation.purge_count = definition.purge_count;
    return activation;
}

}  // namespace phk::battle
