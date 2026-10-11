#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "phk/battle/danmaku/emitter.hpp"

// 弹幕波次时间线：把「一次齐射」组织成按 tick 触发的模式。
//
// 千夜追忆 1.40 逆向成果（setBossBullet* 硬编码序列 + createBullets1 发射语义），
// 仅用于自研玩法。这里的 10 个模式全部引用**真实弹种**（bullet_kind）与
// **真实运动学族**（bullet_func），取代原 boss_race.cpp 里的合成弹幕。
namespace phk::battle::danmaku {

enum class AimMode {
    None,     // 使用 command.base_angle_deg
    AtShip,   // 每 tick 朝自机
    AtEnemy,  // 每 tick 朝敌机（Boss）
};

// 一个波次 = 一次 createBullets1 齐射 + 运动学模板。
struct PatternWave {
    std::uint32_t tick_offset = 0;      // 相对 pattern 起始的 tick
    SpawnCommand command;
    BulletTemplate tmpl;
    AimMode aim = AimMode::None;
    double spin_per_tick_deg = 0.0;     // base_angle += spin * tick（螺旋）
};

// 一个模式 = 若干波次 + 周期。
struct PatternDef {
    std::string_view id;        // 真实模式名
    std::string_view family;    // 运动学族（A..L）
    std::uint32_t period_ticks = 45;
    std::vector<PatternWave> waves;
};

// 10 个真实模式（顺序即 BossRace 调度顺序）。
[[nodiscard]] const std::vector<PatternDef>& BossRacePatternLibrary();

// 计算某 tick 该模式应发射的全部子弹（已应用角度/瞄准/螺旋，未做首帧 update）。
[[nodiscard]] std::vector<BulletMotion> EmitPatternTick(
    const PatternDef& def,
    std::uint64_t tick,
    const EmitContext& ctx,
    const MotionTargets& targets
);

}  // namespace phk::battle::danmaku
