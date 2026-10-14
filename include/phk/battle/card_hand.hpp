// 局内手牌状态与出牌校验（Step 3）。
//
// 真值来源（2026-10-14 逐行对照 Gensoulkyo/runtime/core/service.go）：
//   - 卡池目录 serverCardCatalog（service.go:405-420，16 张）
//   - validateAndActivateCardLocked（service.go:3301-3337）——校验顺序与
//     账本变更逐项照抄
//   - drawServerCard（service.go:4229-4240）——补抽规则
//   - 常量 cardHandLimit=4 / cardMaxEnergy=10.0（service.go:48/50）
//
// 拒绝 reason 采用对战端现有风格（snake_case，前缀 cast_card_*），与
// simulation.cpp 的 invalid_card_slot / cast_card_slot_missing 一致。
//
// 已知事实（记录，不在本层修复）：Go 大厅侧**从不预填手牌**——全仓对
// player.Hand 只有两处写入（cast 时移除、draw 时追加），真实对局手牌恒空，
// Go 测试靠 armCard 手动填。对战端按 T3 §2 由 loadout 直接下发初始手牌。
//
// 版权：东方同人逆向成果，仅用于自研玩法。
#ifndef PHK_BATTLE_CARD_HAND_HPP
#define PHK_BATTLE_CARD_HAND_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "phk/battle/card_effects.hpp"

namespace phk::battle {

// 常量（service.go:48/50）。
constexpr int kCardHandLimit = 4;
constexpr double kCardMaxEnergy = 10.0;

// 卡池目录单条（service.go:405-420 字段全集；缺省值同 Go 零值）。
struct CardDefinition {
    std::string card_id;
    std::string effect_kind;
    double cost = 0.0;
    std::int64_t cooldown_ticks = 0;
    std::int64_t duration_ticks = 0;
    std::int32_t damage = 0;
    double score_bonus = 0.0;
    double energy_gain = 0.0;
    std::int32_t draw_cards = 0;
    std::string pattern_id;
    double purge_radius = 0.0;
    std::int64_t purge_count = 0;
};

// 16 卡白名单目录（按 card_id 查找；不存在返回 nullptr）。
[[nodiscard]] const CardDefinition* FindCardDefinition(const std::string& card_id);

// 局内手牌状态（对应 Go playerState 的卡牌子集）。
struct HandState {
    std::vector<std::string> card_ids;                 // player.Hand
    double energy = 0.0;                               // player.Energy
    std::map<std::string, std::int64_t> cooldowns;     // cardID → 可再次出牌的 tick
    std::size_t draw_cursor = 0;                       // player.DrawCursor
    std::vector<std::string> deck;                     // DeckSnapshot.CardIDs
};

// 出牌校验结果。accepted=false 时 reason 为拒绝码（空串 = 通过）。
struct CastValidationResult {
    bool accepted = false;
    std::string reason;
    const CardDefinition* definition = nullptr;
    std::string card_id;             // TrimSpace 后的卡 id（校验通过时有效）
    std::size_t removed_slot = 0;    // 从手牌移除的槽位
    double energy_after = 0.0;       // 扣费后的能量（含 max(0, ·) 地板）
    std::int64_t cooldown_until = 0; // tick + cooldown_ticks
};

// Go validateAndActivateCardLocked 的纯函数版：只读校验，不改 HandState。
// 校验顺序与 Go 完全一致：
//   1. slot 越界 [0, kCardHandLimit)        → cast_card_slot_out_of_range
//   2. slot 超出当前手牌长度                 → cast_card_slot_empty
//   3. TrimSpace 后不在 16 卡目录            → cast_card_unknown_card
//   4. tick < cooldowns[cardID]             → cast_card_on_cooldown
//   5. energy + 0.0001 < cost               → cast_card_insufficient_energy
[[nodiscard]] CastValidationResult ValidateCast(const HandState& hand, int slot, std::int64_t tick);

// Go drawServerCard：手牌未满且牌堆非空时抽一张（游标取模轮转；空卡 id
// 跳过但不入牌）。返回是否入牌。
[[nodiscard]] bool DrawServerCard(HandState& hand);

// Go applyCardRequestLocked 的纯函数壳：ValidateCast 通过则落账
// （扣能量/写冷却/移除出牌槽/补抽一张），拒绝时 HandState 保持不变。
[[nodiscard]] CastValidationResult ApplyCast(HandState& hand, int slot, std::int64_t tick);

// 由卡池定义构建效果激活参数（桥接 card_effects 层）。
[[nodiscard]] CardActivation BuildCardActivation(const CardDefinition& definition, std::int64_t tick);

// 去除首尾空白（Go strings.TrimSpace 的 ASCII 子集：空格/\t/\n/\r/\v/\f）。
[[nodiscard]] std::string TrimCardId(const std::string& value);

}  // namespace phk::battle

#endif  // PHK_BATTLE_CARD_HAND_HPP
