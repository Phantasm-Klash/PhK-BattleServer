#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// 局内计分规则（TNA 1.40 `GameData`(31) 的形状）。
//
// 逆向依据（docs/apk-reverse-notes/analysis/danmaku_engine_index.md）：
//   * `GameData`(31) 是局内计分单例，符号实证 `getScore/setScore`、
//     `getGrazeCount`、`getHitCount`、`getSpellGetCount/TotalCount`、
//     `getRemainLifeCount/BombCount`、`getExtraTime`；
//   * 加分入口 `GameLayer::addScore(int)` @0x537350；
//   * 擦弹计数入口 `MyShip::createGraze()`。
//
// ★★ 冻结占位（数值待 APK 实测校准）★★
//   逆向笔记只实证了「有这些计分项」，**没有逐常量实测分值**。本文件的全部数值
//   都是按 TNA 结构（擦弹 / 符卡获取 / 击破 / 残机 / 炸弹 / 剩余时间）写的
//   **冻结占位值**，不是从 APK 读出的真值。校准前禁止当作对齐依据引用。
//
// 难度**不影响**分值：TNA 的分数不随难度缩放（难度只缩放发射速度/密度/间隔，
// 见 difficulty_table.hpp）。故本文件不接收难度参数。
//
// 未实现项（本轮不做，仅留结构注释）：
//   * 道具分（点符）——`kGameItem` 的 `emitItemTenfu` / `emitItemKokufu` /
//     `emitItemBounus` 需要道具实体，本端尚未实现道具系统，故不计分；
//   * 擦弹上限、连击/倍率、结算时的高分榜投影——契约未覆盖，暂不建模。
namespace phk::battle::danmaku {

// 一局内的计分系数。全部为**冻结占位值**（见文件头说明）。
struct ScoreRules {
    // 单次擦弹的基础分（实际得分 = graze_points × 自机乘区，见 GrazeMulForShotType）。
    std::uint64_t graze_points = 500;
    // 符卡获取（不被弹的整段符卡）基础分。
    std::uint64_t spell_capture_base = 100000;
    // 符卡获取的「阶段越深越值钱」增量：第 n 段（0 起）= base + per_phase × (n+1)。
    std::uint64_t spell_capture_per_phase = 50000;
    // 击破 Boss 的一次性奖励（每玩家各自的 Boss 副本各算一次）。
    std::uint64_t boss_defeat_bonus = 1000000;
    // 结算：每剩余残机 / 每剩余炸弹的奖励。
    std::uint64_t life_bonus = 100000;
    std::uint64_t bomb_bonus = 20000;
    // 结算：每剩余秒（64 tick = 1 秒，见 kBossRaceTickRateHz）的时间奖励。
    std::uint64_t extra_time_per_second = 1000;
};

// 取当前生效的计分规则。
//
// 暂无参数（数值是冻结占位，没有 ruleset/难度维度）；保留函数形态作为扩展点：
// 未来若实测出「不同 ruleset 不同分值」或「按难度改分值」，只改这里的分派。
[[nodiscard]] constexpr ScoreRules ScoreRulesFor() noexcept {
    return ScoreRules{};
}

// 自机特性乘区表：13 个自机（与 player_loadout.hpp 的 PlayerShotTable() 角色集一致）。
// ★ 全部 1.0 是冻结占位（「自机特性乘区，数值待实测」）——TNA 里不同自机的擦弹
//   收益确有差异（角色特性），但逆向笔记没有逐角色实测值。
struct GrazeShotTypeMul {
    const char* character = "";
    double mul = 1.0;
};

inline constexpr std::array<GrazeShotTypeMul, 13> kGrazeMulByCharacter = {{
    {"Alice", 1.0},
    {"Aya", 1.0},
    {"Cirno", 1.0},
    {"Marisa", 1.0},
    {"Reimu", 1.0},
    {"Reisen", 1.0},
    {"Remillia", 1.0},  // 数据表拼写（player_loadout.hpp 同名，勿改成 Remilia）
    {"Sakuya", 1.0},
    {"Sanae", 1.0},
    {"Seija", 1.0},
    {"Youmu", 1.0},
    {"Yukari", 1.0},
    {"Yuyuko", 1.0},
}};

// shot_type 名（"ReimuA1" / "YuyukoB2" …）→ 自机名（"Reimu" / "Yuyuko"）。
// 规则：去掉末尾的「模式 + 变体」两字符（[AB][12]）；不符合该形态返回空。
[[nodiscard]] constexpr std::string_view ShotCharacterOf(std::string_view shot_type) noexcept {
    if (shot_type.size() < 3) {
        return {};
    }
    const char mode = shot_type[shot_type.size() - 2];
    const char variant = shot_type[shot_type.size() - 1];
    if ((mode != 'A' && mode != 'B') || (variant != '1' && variant != '2')) {
        return {};
    }
    return shot_type.substr(0, shot_type.size() - 2);
}

// shot_type → 擦弹得分乘区。表内按自机名索引（由 shot_type 前缀解析），
// 未知/空 shot_type 缺省 1.0（= 无特性加成，行为与未配置 loadout 一致）。
[[nodiscard]] constexpr double GrazeMulForShotType(std::string_view shot_type) noexcept {
    const std::string_view character = ShotCharacterOf(shot_type);
    if (character.empty()) {
        return 1.0;
    }
    for (const GrazeShotTypeMul& row : kGrazeMulByCharacter) {
        if (character == std::string_view(row.character)) {
            return row.mul;
        }
    }
    return 1.0;
}

}  // namespace phk::battle::danmaku
