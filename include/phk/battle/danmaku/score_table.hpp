#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// 局内计分规则（TNA 1.40 `GameData`(31) 形状 + 反汇编实测常量）。
//
// 实测方法（capstone 脚本在 docs/apk-reverse-notes/tools，目标二进制
// `/root/gotouhou/.rev/libMyGame_arm64.so`）：
//   `GameLayer::addScore(int)` @0x537350 被编译器**完全内联**——函数体只有 4 条
//   指令（`ldr w8,[x0,#1308] / add w8,w8,w1 / str w8,[x0,#1308] / ret`），只累加到
//   `GameLayer+0x51c` 的私有 int，从不碰 GameData；因此 BL 扫描命中 0 条。改用
//   「访问 [xN,#1308]」特征扫描，59 处剔噪后得到 9 个真实加分点。
//
// ★ 本文件的常量是**实测值**（每项出处见字段注释），不是占位。
// ★ 负面证据（同样重要）：残机 / 炸弹 / 剩余时间在 TNA 里**没有计分代码**——
//   `getRemainLifeCount`/`getRemainBombCount` 全二进制各只有 1 个调用者（暂停
//   菜单存档），`getExtraTime` 只被结算标题用于显示格式化，gameOver/gameClear
//   一条加分指令都没有。故本结构不设这三项。
// ★ 未实现：道具分（`kGameItem` 点符，需要道具实体）；对象分值（虚方法返回值，
//   数据驱动，需解析敌机/道具数据表）。
namespace phk::battle::danmaku {

struct ScoreRules {
    // 擦弹：+100 / 次。实测 @0x564d3c / 0x5665e0 / 0x567088（子弹 / 光束 / 激光
    // 三条碰撞路径同值，`add w10, w10, #0x64`）。擦弹计数与加分在同一处。
    std::uint64_t graze_points = 100;
    // 消弹：+100 / 颗。实测 @0x568eb0（`bulletAllClearAndItem`）。
    std::uint64_t bullet_clear_points = 100;
    // 命中：+30 / 次。实测 @0x5662b0（`checkCollision`）。
    std::uint64_t hit_points = 30;
    // 实体消灭：+10000；`SelectChara ∈ {15,16}` 时 +15000。
    // 实测 @0x565724 / 0x56571c。15/16 与 13 个自机名的映射未实测，
    // 故 {@link DestroyScoreForCharacter} 当前一律返回 base。
    std::uint64_t destroy_points = 10000;
    std::uint64_t destroy_points_alt = 15000;
    // 计时：每 10 tick +110。实测 @0x562200（`[GameLayer+0x524] % 10 == 0`）。
    std::uint64_t timer_points = 110;
    std::uint64_t timer_interval_ticks = 10;
    // 符卡获取：实测是一条**求和式**（`setSpellBonus` @0x55e18c..0x55e1bc，
    // 累加点 @0x55e2ec），不是「base + per_phase × 阶段」：
    //   bonus = 300000×Boss序号 + 110×[+0x398] + 1100×擦弹数 − 2000000
    // `[+0x398]` 是每帧递减的计数，写入点未定位 ⇒ 其贡献按 0 计（counter 参数）。
    std::uint64_t spell_boss_coef = 300000;
    std::uint64_t spell_graze_coef = 1100;
    std::uint64_t spell_counter_coef = 110;
    std::uint64_t spell_offset = 2000000;
};

// 取当前生效的计分规则（无 ruleset/难度维度：TNA 的分数不随难度缩放）。
[[nodiscard]] constexpr ScoreRules ScoreRulesFor() noexcept {
    return ScoreRules{};
}

// 无 boss_id 时的默认 Boss 序号。取 11 = 原版第一个 Boss 弹幕函数编号
// （`setBossBullet11`）：默认竞速局走合成 10 模式、没有具体 boss，但符卡奖励
// 必须**生效**（用户 2026-10-27 明确要求），故按「第一号 Boss」计。
// ★ 占位默认值，不是实测映射：真值待 APK 的关卡→Boss 序号表落地后替换。
inline constexpr std::uint64_t kDefaultBossNumber = 11;

// 原版 Boss 序号（`+0x348`，`setSelectBossNum` 写入）——从 boss_id 的数字部分
// 推断："boss21" → 21、"boss11" → 11。空/无数字 → {@link kDefaultBossNumber}。
//
// 反汇编实证（0x55e18c..0x55e1bc）：符卡奖励 = 300000×[+0x348] + 110×[+0x398]
// + 1100×[+0x514] − 2000000。其中 +0x514 是擦弹计数（本端有），+0x398 是每帧
// 递减的计数（写入点未定位，按 0 计）。Boss 序号量级为 11..22（与
// setBossBullet11..22 的命名一致），故 300000×11 − 2000000 = 1,300,000 > 0，
// 符卡奖励为正——这也是「默认值取 11」的依据之一。
[[nodiscard]] constexpr std::uint64_t BossNumberOf(std::string_view boss_id) noexcept {
    std::uint64_t value = 0;
    bool any = false;
    for (const char c : boss_id) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + static_cast<std::uint64_t>(c - '0');
            any = true;
        } else if (any) {
            break;
        }
    }
    return any ? value : kDefaultBossNumber;
}

// 符卡获取分：实测求和式，clamp ≥ 0（Boss 序号小、擦弹少时公式为负；原作不扣分）。
[[nodiscard]] constexpr std::uint64_t SpellCapturePoints(
    const ScoreRules& rules,
    std::uint64_t boss_number,
    std::uint64_t graze,
    std::uint64_t counter = 0
) noexcept {
    const std::uint64_t sum = rules.spell_boss_coef * boss_number +
        rules.spell_graze_coef * graze + rules.spell_counter_coef * counter;
    return sum > rules.spell_offset ? sum - rules.spell_offset : 0;
}

// 实体消灭分：`SelectChara ∈ {15,16}` → 15000，否则 10000。
// ★ 15/16 与 13 个自机名的映射未实测 ⇒ 当前一律返回 base（结构保留，待映射）。
[[nodiscard]] constexpr std::uint64_t DestroyScoreForCharacter(std::string_view shot_type) noexcept {
    (void)shot_type;
    return ScoreRules{}.destroy_points;
}

// ---------------------------------------------------------------------------
// 历史结构：TNA 实测无对应机制，保留待证伪/证实
// ---------------------------------------------------------------------------
// 自机特性乘区：实测三个擦弹加分点用的是同一立即数 100，擦弹路径上没有任何按
// 自机分支的代码 ⇒ 乘区恒 1.0。唯一实测到的自机差异在实体消灭分（见上）。
// 本表保留是为了「若将来发现别的按自机分支的加分点，有地方挂」。
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

// shot_type → 擦弹得分乘区（当前恒 1.0，见上方说明）。
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
