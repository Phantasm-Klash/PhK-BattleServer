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
// ★★ 部分实测（R1，2026-10-24）★★
//   对 libMyGame_arm64.so 做了全 .text 的 `addScore` 内联点扫描（addScore 本身
//   只有 4 条指令、无任何 BL 调用者，已被完全内联），共找到 9 个写
//   `GameLayer+0x51c` 的加分点。**只有 `graze_points` 拿到了可直接读出立即数的
//   实测值**；其余 6 项在二进制里没有对应常量（原因逐条写在各字段上）。
//   仍未实测的项一律保留原占位值，**禁止当作对齐依据引用**。
//
//   实测汇总（GameLayer+0x51c 的 9 个加分点）：
//     +100   ×3  擦弹          0x564d3c / 0x5665e0 / 0x567088
//     +100   ×N  消弹（每颗）   0x568eb0  GameLayer::bulletAllClearAndItem
//     +30        命中          0x5662b0
//     +10000/+15000 实体消灭    0x565724 / 0x56571c（SelectChara∈{15,16} 取后者）
//     +110/10tick 计时          0x562200  GameLayer::update
//     公式        符卡获取      0x55e2ec  见 spell_capture_base 注释
//     未知        对象分值     0x56512c / 0x5662a0（虚方法返回值，数据驱动）
//
// 难度**不影响**分值：TNA 的分数不随难度缩放（难度只缩放发射速度/密度/间隔，
// 见 difficulty_table.hpp）。故本文件不接收难度参数。
//
// 未实现项（本轮不做，仅留结构注释）：
//   * 道具分（点符）——`kGameItem` 的 `emitItemTenfu` / `emitItemKokufu` /
//     `emitItemBounus` 需要道具实体，本端尚未实现道具系统，故不计分；
//   * 擦弹上限、连击/倍率、结算时的高分榜投影——契约未覆盖，暂不建模。
namespace phk::battle::danmaku {

// 一局内的计分系数。仅 `graze_points` 为实测值，其余仍是冻结占位（见字段注释）。
struct ScoreRules {
    // 单次擦弹的基础分（实际得分 = graze_points × 自机乘区，见 GrazeMulForShotType）。
    // 实测 @0x564d3c / 0x5665e0 / 0x567088（GameLayer::checkCollision 内联加分，
    // `add w10, w10, #0x64`）：单次擦弹 **+100**，同时 graze 计数 +1
    // （`[GameLayer+0x50c]`=1292 即 GameData::setGrazeCount 的来源）。置信度：实测。
    std::uint64_t graze_points = 100;
    // 符卡获取（不被弹的整段符卡）基础分。
    // ★ 未实测，仍为占位。APK 里的符卡奖励**不是**「base + per_phase×(n+1)」形状，
    //   而是 GameLayer::setSpellBonus @0x55e18c..0x55e1bc 的一条一次性求和：
    //     bonus = 300000×[+0x348] + 110×[+0x398] + 1100×[+0x514] − 2000000
    //   （0x348 = 选择的 Boss 序号，setSelectBossNum 写入；0x398 / 0x514 为局内
    //    计数，语义未完全确定），累加点 @0x55e2ec。本结构表达不了该公式，
    //   故数值保持占位。
    std::uint64_t spell_capture_base = 100000;
    // 符卡获取的「阶段越深越值钱」增量：第 n 段（0 起）= base + per_phase × (n+1)。
    // ★ 未实测，仍为占位。实测公式里没有「阶段」维度（见上一条），此字段无对应物。
    std::uint64_t spell_capture_per_phase = 50000;
    // 击破 Boss 的一次性奖励（每玩家各自的 Boss 副本各算一次）。
    // ★ 未实测，仍为占位。checkCollision 里唯一能读出立即数的实体消灭/拾取加分是
    //   **10000**（SelectChara∈{15,16} 时 **15000**），@0x565724 / 0x56571c；
    //   但该分支挂在「实体 kind」的 switch 上，究竟是击破还是拾取未完全确定，
    //   且全二进制未见 Boss 专属的更高常量，故不改此值。
    std::uint64_t boss_defeat_bonus = 1000000;
    // 结算：每剩余残机 / 每剩余炸弹的奖励。
    // ★ 未实测，仍为占位。实测 GameLayer::gameOver @0x5371dc / gameClear @0x5647f4
    //   只把累计分写回 GameData::setScore，**没有任何残机/炸弹结算加成**；
    //   GameData::getRemainLifeCount / getRemainBombCount 全二进制仅被
    //   PauseMenu::saveBossRushRetryData 用于存档。TNA 很可能没有这两项奖励。
    std::uint64_t life_bonus = 100000;
    std::uint64_t bomb_bonus = 20000;
    // 结算：每剩余秒（64 tick = 1 秒，见 kBossRaceTickRateHz）的时间奖励。
    // ★ 未实测，仍为占位。实测 GameData::getExtraTime 全二进制仅被
    //   GameOverLayer::displayScoreTitle @0x575f18 读取，只做 `extraTime / 60.0`
    //   的**显示**格式化，不参与加分。TNA 很可能没有时间奖励。
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
// ★ R1 补充实测：三个擦弹加分点（0x564d3c / 0x5665e0 / 0x567088）用的是**同一个
//   立即数 100**，擦弹路径上没有任何按自机分支的代码；唯一实测到的自机差异在
//   「实体消灭分」（SelectChara∈{15,16} → 15000，否则 10000），与擦弹无关。
//   故本表是否该存在本身存疑——保留结构，数值仍为占位。
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
