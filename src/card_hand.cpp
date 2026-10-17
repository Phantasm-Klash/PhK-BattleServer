// 局内手牌状态与出牌校验实现。对照 service.go:405-420 / 3301-3337 / 4229-4240。
//
// M-1（2026-10-14）：slot 权威边界由 kCardHandLimit(4) 改为 hand.card_ids.size()；
// slot == -1 为"不出牌"哨兵（BattleInput.card_slot struct default，protocol.hpp:49）；
// 越界（含 Go 原 outside-hand 与 empty 两级）统一走
// cast_card_slot_out_of_range → 调用方 card_rejected 路径。
//
// 版权：东方同人逆向成果，仅用于自研玩法。

#include "phk/battle/card_hand.hpp"

#include <algorithm>
#include <cctype>

namespace phk::battle {
namespace {

// serverCardCatalog（service.go:405-420）——20 张（16 -> 20 扩充，2026-10-17：
// frost_arc / ember_lance / iron_veil / thunder_fall 追加在末尾），字段顺序：
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
    {"frost_arc", "bullet_purge", 2, 360, 1, 0, 18, 0, 0, "", 100, 12},
    {"ember_lance", "opponent_pattern", 3, 300, 240, 20, 24, 0, 0, "ember_lance", 0, 0},
    {"iron_veil", "invulnerable", 4, 480, 120, 0, 16, 0, 0, "", 0, 0},
    {"thunder_fall", "finisher", 4, 420, 1, 30, 40, 0, 0, "", 0, 0},
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
    // M-1：-1 是 wire struct default（protocol.hpp:49）= "本 tick 不出牌"。
    // 不是错误：accepted=true 但 plays_no_card=true，无卡定义、无拒绝码。
    // 调用方（未来 executor）见此结果时既不产生 cast 效果也不发 card_rejected；
    // ApplyCast 见此结果保持 HandState 不变。
    if (slot == -1) {
        result.accepted = true;
        result.plays_no_card = true;
        result.energy_after = hand.energy;
        return result;
    }
    // M-1：权威边界 = 实际手牌长度（0..hand.size()-1），不再用固定的
    // kCardHandLimit=4。合并了 Go 的两级错误：
    //   - Go "card slot is outside hand"（slot<0 || slot>=4）——>3 的 slot
    //     由传输层先拦（simulation.cpp:701 wire input -1..7；
    //     :868-877 cast_card payload 0..7）；
    //   - Go "card slot is empty"（slot>=len(hand)）——对战端手牌由 loadout
    //     下发、当前恒不满 4，slot>=hand.size() 即无牌可出，越界即拒。
    if (slot < 0 || static_cast<std::size_t>(slot) >= hand.card_ids.size()) {
        result.reason = "cast_card_slot_out_of_range";
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
    // 拒绝：原样返回，HandState 不变（调用方走 card_rejected）。
    // 哨兵 -1：accepted 但无牌可出，同样不改任何状态。
    if (!result.accepted || result.plays_no_card) {
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
