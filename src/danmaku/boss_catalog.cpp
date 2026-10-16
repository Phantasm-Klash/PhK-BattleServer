// Boss 目录数据（线程A「游戏内容」）。
//
// 归属与组合理由（同 boss_catalog.hpp 头注释）：
//   * `hui`（辉） = 1.40 舞台表第 5 项「輝」（0xac595f，JP/CN 共用 7 项：
//     妖永風地輝紺怪）。輝 stage 在千夜帖中对应永远亭/月都阵营（辉夜、永琳、
//     妹红…），其符卡以正弦半径、反弹、镜像等「规律中的变化」为主 —— 阶段组
//     因此选 kusabi_gap_ring（缺口环，永琳型秩序）→ sizuku_sine_radius（正弦
//     半径，辉夜型波纹）→ kunai_bounce（反弹，妹红型速度）→ heart_sine_speed
//     （正弦调速）→ stama_mirror（镜像），HP 24000、垂直往复移动（月之都的
//     「升降」意象）。
//   * `tian`（天） = 1.40 二进制无独立舞台名串，但成就「通关［紅〜天］所有关卡」
//     与开发者注释证实「天」关卡存在（名称串不在本二进制，按任务书回退路径：
//     组合已移植 pattern 库）。天 stage 在原作处于流程后段，「天」意象 = 高天原
//     式的整圆齐射与追踪压迫 —— 阶段组选 kome_ring（米弹整圆）→
//     arrow_aimed_fan（瞄准扇）→ ring_spiral（螺旋）→ kome_homing（追踪）→
//     chou_four_dir（四向），HP 20000、水平往复移动（「天」的横向广袒）。
//   * 数值（HP/幅度/周期/波次数）为工程标定值，三引擎必须完全一致；默认路径
//     （boss_id 未设置）不走本目录，逐字节保持现状。
#include "phk/battle/danmaku/boss_catalog.hpp"

#include <algorithm>

namespace phk::battle::danmaku {
namespace {

// 把阶段组的 pattern id 解析为库内 index（库顺序固定，见 pattern_timeline.cpp）。
std::size_t PatternIndexById(std::string_view pattern_id) {
    const std::vector<PatternDef>& library = BossRacePatternLibrary();
    for (std::size_t i = 0; i < library.size(); ++i) {
        if (library[i].id == pattern_id) {
            return i;
        }
    }
    return 0;  // 找不到回退库首项（调用方在构造期已校验，运行期不应发生）
}

std::vector<BossCatalogEntry> BuildCatalog() {
    std::vector<BossCatalogEntry> catalog;

    // tian（天）—— 见上方组合理由。
    {
        BossCatalogEntry entry;
        entry.id = "tian";
        entry.display = "天";
        entry.boss_max_hp = 20000;
        entry.movement.kind = BossMovement::SwayX;
        entry.movement.amplitude_milli = 60000;  // 2*60000/300 = 400，整数整除
        entry.movement.period_ticks = 600;
        entry.movement.half_ticks = 300;
        entry.phases = {
            {"kome_ring", 4},         // 180 tick
            {"arrow_aimed_fan", 3},   // 135 tick
            {"ring_spiral", 4},       // 180 tick
            {"kome_homing", 3},       // 135 tick
            {"chou_four_dir", 4},     // 180 tick
        };
        catalog.push_back(std::move(entry));
    }

    // hui（辉）—— 见上方组合理由。
    {
        BossCatalogEntry entry;
        entry.id = "hui";
        entry.display = "辉";
        entry.boss_max_hp = 24000;
        entry.movement.kind = BossMovement::BobY;
        entry.movement.amplitude_milli = 30000;  // 2*30000/240 = 250，整数整除
        entry.movement.period_ticks = 480;
        entry.movement.half_ticks = 240;
        entry.phases = {
            {"kusabi_gap_ring", 3},     // 135 tick
            {"sizuku_sine_radius", 4},  // 180 tick
            {"kunai_bounce", 3},        // 135 tick
            {"heart_sine_speed", 4},    // 180 tick
            {"stama_mirror", 3},        // 135 tick
        };
        catalog.push_back(std::move(entry));
    }

    return catalog;
}

}  // namespace

const std::vector<BossCatalogEntry>& BossCatalog() {
    static const std::vector<BossCatalogEntry> catalog = BuildCatalog();
    return catalog;
}

const BossCatalogEntry* FindBossCatalogEntry(std::string_view boss_id) {
    if (boss_id.empty()) {
        return nullptr;
    }
    for (const BossCatalogEntry& entry : BossCatalog()) {
        if (entry.id == boss_id) {
            return &entry;
        }
    }
    return nullptr;
}

bool IsKnownBossId(std::string_view boss_id) {
    return FindBossCatalogEntry(boss_id) != nullptr;
}

std::size_t BossTimelineLength(const BossCatalogEntry& entry) {
    std::size_t total = 0;
    for (const BossPatternPhase& phase : entry.phases) {
        total += phase.waves;
    }
    return total;
}

std::size_t BossTimelinePatternIndexAt(const BossCatalogEntry& entry, std::size_t wave_index) {
    const std::size_t length = BossTimelineLength(entry);
    if (length == 0) {
        return 0;
    }
    std::size_t cursor = wave_index % length;
    for (const BossPatternPhase& phase : entry.phases) {
        if (cursor < phase.waves) {
            return PatternIndexById(phase.pattern_id);
        }
        cursor -= phase.waves;
    }
    return 0;
}

void BossEmitterOffsetMilli(
    const BossMovementSpec& movement,
    std::uint64_t tick,
    std::int32_t* out_x_milli,
    std::int32_t* out_y_milli
) {
    std::int32_t x = 0;
    std::int32_t y = 0;
    if (movement.kind != BossMovement::Static && movement.amplitude_milli > 0 &&
        movement.period_ticks > 0 && movement.half_ticks > 0) {
        // 整数三角波：tick=0 -> -amplitude；tick=half -> +amplitude；tick=period -> -amplitude。
        std::uint64_t period = movement.period_ticks;
        std::uint64_t tri = static_cast<std::uint64_t>(tick) % period;
        if (tri > movement.half_ticks) {
            tri = period - tri;
        }
        const std::int64_t step =
            (2 * static_cast<std::int64_t>(movement.amplitude_milli)) /
            static_cast<std::int64_t>(movement.half_ticks);
        const std::int64_t offset =
            -static_cast<std::int64_t>(movement.amplitude_milli) + step * static_cast<std::int64_t>(tri);
        if (movement.kind == BossMovement::SwayX) {
            x = static_cast<std::int32_t>(offset);
        } else {
            y = static_cast<std::int32_t>(offset);
        }
    }
    if (out_x_milli != nullptr) {
        *out_x_milli = x;
    }
    if (out_y_milli != nullptr) {
        *out_y_milli = y;
    }
}

}  // namespace phk::battle::danmaku
