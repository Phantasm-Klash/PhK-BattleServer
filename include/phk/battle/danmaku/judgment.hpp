#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 服务端权威判定系统**骨架**（DANMAKU_CLIENT_ENGINE_PLAN §2.4）。
//
// P1 只落「纯函数 + 状态机定义 + 几何工具」：不持有全局状态、不依赖模拟对象、
// **不接线进 BossRaceSimulation::Tick**（判定接线是 P3 的活）。这样做的目的是
// 先把 §2.4 的接口与数值语义冻结下来，双端（C++ 权威 / TS 镜像）按同一份定义
// 实现，P3 接线时只改调用点。
//
// 单位约定（与快照 wire 一致）：
//   * 位置/半径/长度 = milli（1 px = 1000 milli）；
//   * 角度 = 毫度（0.01°，9000 = 90°）；
//   * 时间 = tick（64 Hz 基准，见计划 §2.1）。
//
// 铁拳 / 黑色帷幕的阶段时长（T1..T6）与半径/长度在 P4 会移到 boss_catalog 数据表；
// 本文件只给结构体 + 默认值，调用方（P3/P4）负责从数据表填参。
namespace phk::battle::danmaku {

// 二维整数坐标（milli）。
struct PointMilli {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

// ---------------------------------------------------------------------------
// 几何工具（整数运算，无浮点 → 三端逐位一致）
// ---------------------------------------------------------------------------

// 两点距离平方（int64 防溢出：milli 坐标 ±480000 的平方 ≈ 2.3e11）。
[[nodiscard]] std::int64_t DistanceSquaredMilli(PointMilli a, PointMilli b);

// 点到线段 (a→b) 的最短距离平方。a == b 时退化为点-点距离。
[[nodiscard]] std::int64_t PointSegmentDistanceSquaredMilli(PointMilli point, PointMilli a, PointMilli b);

// 点-圆命中：distance(point, center) <= radius。
[[nodiscard]] bool PointHitsCircle(PointMilli point, PointMilli center, std::int32_t radius_milli);

// 点-胶囊命中：点到线段 (a→b) 的最短距离 <= radius（铁拳 strike 的判定盒）。
[[nodiscard]] bool PointHitsCapsule(PointMilli point, PointMilli a, PointMilli b, std::int32_t radius_milli);

// ---------------------------------------------------------------------------
// 判定事件（进快照 events[]，§2.4.6）
// ---------------------------------------------------------------------------
struct JudgeEvent {
    std::uint64_t tick = 0;
    std::string type;  // 见各状态机注释里的字符串常量
};

// ---------------------------------------------------------------------------
// 铁拳 iron_fist（§2.4.4）：windup（telegraph）→ strike（胶囊推进）→ recover
// ---------------------------------------------------------------------------
enum class IronFistPhase {
    Idle,
    Windup,
    Strike,
    Recover,
};

struct IronFistParams {
    std::uint32_t windup_ticks = 45;   // T1：telegraph 期
    std::uint32_t strike_ticks = 15;   // T2：命中盒推进期
    std::uint32_t recover_ticks = 30;  // T3：硬直期
    // 命中半径（自机判定点 r_hit，原版惯例 ≈3px）。胶囊半宽同值。
    std::int32_t hit_radius_milli = 3000;
};

struct IronFistState {
    IronFistPhase phase = IronFistPhase::Idle;
    std::uint32_t t_remaining = 0;   // 当前阶段剩余 tick（含本 tick 未推进部分）
    std::uint32_t phase_elapsed = 0; // 当前阶段已过 tick（strike 期算推进比例）
    PointMilli origin;
    std::int32_t dir_x_milli = 0;    // 单位方向 ×1000（|dir| ≈ 1000）
    std::int32_t dir_y_milli = 0;
    std::int32_t length_milli = 0;   // 拳头总长度（strike 期逐 tick 推进到全长）
};

// 事件类型字符串（进快照 events[].type）。
inline constexpr const char* kIronFistWindupEvent = "iron_fist_windup";
inline constexpr const char* kIronFistStrikeEvent = "iron_fist_strike";
inline constexpr const char* kIronFistRecoverEvent = "iron_fist_recover";
inline constexpr const char* kIronFistEndEvent = "iron_fist_end";

// 起手：进入 Windup，t_remaining = T1。方向 (dx,dy) 由调用方给单位向量 ×1000。
void StartIronFist(
    IronFistState* state,
    PointMilli origin,
    std::int32_t dir_x_milli,
    std::int32_t dir_y_milli,
    std::int32_t length_milli,
    const IronFistParams& params
);

// 推进一 tick，返回本 tick 发生的阶段切换事件（可能为空）。
// Idle 状态下调用是 no-op（返回空）。
[[nodiscard]] std::vector<JudgeEvent> AdvanceIronFist(
    IronFistState* state,
    std::uint64_t tick,
    const IronFistParams& params
);

// strike 期当前拳尖位置（= origin + dir × length × 推进比例）；非 strike 期返回 origin。
[[nodiscard]] PointMilli IronFistStrikeTip(const IronFistState& state);

// 命中判定：仅 strike 期有效；自机判定点 vs 胶囊（origin → 拳尖，半宽 r_hit）。
[[nodiscard]] bool IronFistHitsPoint(
    const IronFistState& state,
    PointMilli point,
    const IronFistParams& params
);

// ---------------------------------------------------------------------------
// 黑色帷幕 black_curtain（§2.4.5）：fall（下落）→ hold（隐藏弹幕，判定照算）
//                                → sweep（边缘扫屏激光线）
// ---------------------------------------------------------------------------
enum class BlackCurtainPhase {
    Idle,
    Fall,
    Hold,
    Sweep,
};

struct BlackCurtainParams {
    std::uint32_t fall_ticks = 45;   // T4
    std::uint32_t hold_ticks = 60;   // T5（子弹 hidden=true，客户端不渲染）
    std::uint32_t sweep_ticks = 45;  // T6
    // sweep 边缘线的判定半厚（激光线宽 / 2）。
    std::int32_t edge_half_thickness_milli = 2500;
    // sweep 期边缘线从 start 扫到 end（屏幕上下沿）。
    std::int32_t sweep_start_y_milli = -480000;
    std::int32_t sweep_end_y_milli = 480000;
};

struct BlackCurtainState {
    BlackCurtainPhase phase = BlackCurtainPhase::Idle;
    std::uint32_t t_remaining = 0;
    std::uint32_t phase_elapsed = 0;
    std::int32_t edge_y_milli = 0;  // sweep 期扫屏边缘线的 y（fall/hold 期 = 帷幕下沿）
};

inline constexpr const char* kBlackCurtainFallEvent = "black_curtain_fall";
inline constexpr const char* kBlackCurtainHoldEvent = "black_curtain_hold";
inline constexpr const char* kBlackCurtainSweepEvent = "black_curtain_sweep";
inline constexpr const char* kBlackCurtainEndEvent = "black_curtain_end";

void StartBlackCurtain(BlackCurtainState* state, const BlackCurtainParams& params);

[[nodiscard]] std::vector<JudgeEvent> AdvanceBlackCurtain(
    BlackCurtainState* state,
    std::uint64_t tick,
    const BlackCurtainParams& params
);

// hold 期子弹 hidden=true：客户端不渲染，但服务端**判定照算**（§2.4.5）。
[[nodiscard]] bool BulletsHiddenDuringCurtain(const BlackCurtainState& state);

// sweep 期命中：自机判定点到边缘线的垂直距离 <= edge_half_thickness。
[[nodiscard]] bool BlackCurtainHitsPoint(
    const BlackCurtainState& state,
    PointMilli point,
    const BlackCurtainParams& params
);

}  // namespace phk::battle::danmaku
