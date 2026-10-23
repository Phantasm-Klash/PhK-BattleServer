#include "phk/battle/danmaku/judgment.hpp"

#include <algorithm>
#include <cstdint>

// 判定系统骨架实现（计划 §2.4）。纯函数 + 显式状态推进，无全局状态、无浮点：
// 所有几何判定用 int64 整数运算，保证 C++ 权威端与 TS 镜像端逐位一致。
//
// 注意：点-线段距离用整数除法取最近点，最近点有 ≤1 milli 的截断误差
// （距离误差 ≤ ~1.4 milli）。判定半径是 3000 milli 量级，该误差可忽略；
// P3 若需要更严格，可改成「比较 dot 与 len² 的有理数」形式。
namespace phk::battle::danmaku {
namespace {

std::int64_t Square(std::int64_t value) { return value * value; }

std::int32_t ClampToInt32(std::int64_t value) {
    if (value < static_cast<std::int64_t>(INT32_MIN)) return INT32_MIN;
    if (value > static_cast<std::int64_t>(INT32_MAX)) return INT32_MAX;
    return static_cast<std::int32_t>(value);
}

}  // namespace

std::int64_t DistanceSquaredMilli(PointMilli a, PointMilli b) {
    const std::int64_t dx = static_cast<std::int64_t>(a.x) - b.x;
    const std::int64_t dy = static_cast<std::int64_t>(a.y) - b.y;
    return Square(dx) + Square(dy);
}

std::int64_t PointSegmentDistanceSquaredMilli(PointMilli point, PointMilli a, PointMilli b) {
    const std::int64_t abx = static_cast<std::int64_t>(b.x) - a.x;
    const std::int64_t aby = static_cast<std::int64_t>(b.y) - a.y;
    const std::int64_t apx = static_cast<std::int64_t>(point.x) - a.x;
    const std::int64_t apy = static_cast<std::int64_t>(point.y) - a.y;
    const std::int64_t len2 = Square(abx) + Square(aby);
    if (len2 == 0) {
        return Square(apx) + Square(apy);  // 退化线段 = 点
    }
    const std::int64_t dot = apx * abx + apy * aby;
    if (dot <= 0) {
        return Square(apx) + Square(apy);  // 最近点 = a
    }
    if (dot >= len2) {
        const std::int64_t bpx = static_cast<std::int64_t>(point.x) - b.x;
        const std::int64_t bpy = static_cast<std::int64_t>(point.y) - b.y;
        return Square(bpx) + Square(bpy);  // 最近点 = b
    }
    // 最近点 = a + ab * dot / len²（整数除法，≤1 milli 截断）。
    const std::int64_t cx = static_cast<std::int64_t>(a.x) + (abx * dot) / len2;
    const std::int64_t cy = static_cast<std::int64_t>(a.y) + (aby * dot) / len2;
    const std::int64_t dx = static_cast<std::int64_t>(point.x) - cx;
    const std::int64_t dy = static_cast<std::int64_t>(point.y) - cy;
    return Square(dx) + Square(dy);
}

bool PointInCircle(
    std::int32_t px,
    std::int32_t py,
    std::int32_t cx,
    std::int32_t cy,
    std::int32_t radius_milli
) {
    if (radius_milli < 0) return false;
    return DistanceSquaredMilli(PointMilli{px, py}, PointMilli{cx, cy}) <= Square(radius_milli);
}

bool PointInCapsule(
    std::int32_t px,
    std::int32_t py,
    std::int32_t x0,
    std::int32_t y0,
    std::int32_t dx_milli,
    std::int32_t dy_milli,
    std::int32_t length_milli,
    std::int32_t radius_milli
) {
    if (radius_milli < 0) return false;
    // 方向是单位向量 ×1000：终点 = 起点 + dir × length / 1000。
    PointMilli tip;
    tip.x = ClampToInt32(static_cast<std::int64_t>(x0) +
                         static_cast<std::int64_t>(dx_milli) * length_milli / 1000);
    tip.y = ClampToInt32(static_cast<std::int64_t>(y0) +
                         static_cast<std::int64_t>(dy_milli) * length_milli / 1000);
    return PointSegmentDistanceSquaredMilli(PointMilli{px, py}, PointMilli{x0, y0}, tip) <=
        Square(radius_milli);
}

bool CurtainSweepHit(
    std::int32_t px,
    std::int32_t py,
    std::int32_t edge_y_milli,
    std::int32_t radius_milli
) {
    (void)px;  // 边缘线横贯全屏：命中只取决于 y。
    if (radius_milli < 0) return false;
    const std::int64_t dy = static_cast<std::int64_t>(py) - edge_y_milli;
    return dy <= radius_milli && dy >= -static_cast<std::int64_t>(radius_milli);
}

// ---------------------------------------------------------------------------
// 铁拳
// ---------------------------------------------------------------------------
void StartIronFist(
    IronFistState* state,
    PointMilli origin,
    std::int32_t dir_x_milli,
    std::int32_t dir_y_milli,
    std::int32_t length_milli,
    const IronFistParams& params
) {
    if (state == nullptr) return;
    state->phase = IronFistPhase::Windup;
    state->t_remaining = params.windup_ticks;
    state->phase_elapsed = 0;
    state->origin = origin;
    state->dir_x_milli = dir_x_milli;
    state->dir_y_milli = dir_y_milli;
    state->length_milli = length_milli;
}

std::vector<JudgeEvent> AdvanceIronFist(
    IronFistState* state,
    std::uint64_t tick,
    const IronFistParams& params
) {
    std::vector<JudgeEvent> events;
    if (state == nullptr || state->phase == IronFistPhase::Idle) {
        return events;
    }
    ++state->phase_elapsed;
    if (state->t_remaining > 0) {
        --state->t_remaining;
    }
    if (state->t_remaining > 0) {
        return events;
    }

    // 阶段切换（t_remaining 归零的那一 tick 立即切）。
    switch (state->phase) {
        case IronFistPhase::Windup:
            state->phase = IronFistPhase::Strike;
            state->t_remaining = params.strike_ticks;
            state->phase_elapsed = 0;
            events.push_back(JudgeEvent{tick, kIronFistStrikeEvent});
            break;
        case IronFistPhase::Strike:
            state->phase = IronFistPhase::Recover;
            state->t_remaining = params.recover_ticks;
            state->phase_elapsed = 0;
            events.push_back(JudgeEvent{tick, kIronFistRecoverEvent});
            break;
        case IronFistPhase::Recover:
            state->phase = IronFistPhase::Idle;
            state->t_remaining = 0;
            state->phase_elapsed = 0;
            events.push_back(JudgeEvent{tick, kIronFistEndEvent});
            break;
        case IronFistPhase::Idle:
            break;
    }
    return events;
}

PointMilli IronFistStrikeTip(const IronFistState& state) {
    if (state.phase != IronFistPhase::Strike) {
        return state.origin;
    }
    // 推进比例 = 已过 tick / 阶段总时长。总时长由 t_remaining + phase_elapsed
    // 反推（Advance 保证二者之和恒等于该阶段总 tick 数）。
    const std::uint32_t total = state.t_remaining + state.phase_elapsed;
    if (total == 0) return state.origin;
    const std::int64_t reach = static_cast<std::int64_t>(state.length_milli) *
        static_cast<std::int64_t>(state.phase_elapsed) / static_cast<std::int64_t>(total);
    PointMilli tip;
    tip.x = ClampToInt32(static_cast<std::int64_t>(state.origin.x) +
                         static_cast<std::int64_t>(state.dir_x_milli) * reach / 1000);
    tip.y = ClampToInt32(static_cast<std::int64_t>(state.origin.y) +
                         static_cast<std::int64_t>(state.dir_y_milli) * reach / 1000);
    return tip;
}

bool IronFistHitsPoint(
    const IronFistState& state,
    PointMilli point,
    const IronFistParams& params
) {
    if (state.phase != IronFistPhase::Strike) {
        return false;
    }
    const PointMilli tip = IronFistStrikeTip(state);
    // 直接用 origin → tip 两点做胶囊判定（拳尖已按推进比例算好，不再二次换算长度）。
    return PointSegmentDistanceSquaredMilli(point, state.origin, tip) <=
        Square(params.hit_radius_milli);
}

// ---------------------------------------------------------------------------
// 黑色帷幕
// ---------------------------------------------------------------------------
void StartBlackCurtain(BlackCurtainState* state, const BlackCurtainParams& params) {
    if (state == nullptr) return;
    state->phase = BlackCurtainPhase::Fall;
    state->t_remaining = params.fall_ticks;
    state->phase_elapsed = 0;
    state->edge_y_milli = params.sweep_start_y_milli;
}

std::vector<JudgeEvent> AdvanceBlackCurtain(
    BlackCurtainState* state,
    std::uint64_t tick,
    const BlackCurtainParams& params
) {
    std::vector<JudgeEvent> events;
    if (state == nullptr || state->phase == BlackCurtainPhase::Idle) {
        return events;
    }
    ++state->phase_elapsed;
    if (state->t_remaining > 0) {
        --state->t_remaining;
    }
    if (state->t_remaining > 0) {
        return events;
    }

    switch (state->phase) {
        case BlackCurtainPhase::Fall:
            state->phase = BlackCurtainPhase::Hold;
            state->t_remaining = params.hold_ticks;
            state->phase_elapsed = 0;
            events.push_back(JudgeEvent{tick, kBlackCurtainHoldEvent});
            break;
        case BlackCurtainPhase::Hold:
            state->phase = BlackCurtainPhase::Sweep;
            state->t_remaining = params.sweep_ticks;
            state->phase_elapsed = 0;
            state->edge_y_milli = params.sweep_start_y_milli;
            events.push_back(JudgeEvent{tick, kBlackCurtainSweepEvent});
            break;
        case BlackCurtainPhase::Sweep:
            state->phase = BlackCurtainPhase::Idle;
            state->t_remaining = 0;
            state->phase_elapsed = 0;
            events.push_back(JudgeEvent{tick, kBlackCurtainEndEvent});
            break;
        case BlackCurtainPhase::Idle:
            break;
    }
    return events;
}

bool BulletsHiddenDuringCurtain(const BlackCurtainState& state) {
    return state.phase == BlackCurtainPhase::Hold;
}

bool BlackCurtainHitsPoint(
    const BlackCurtainState& state,
    PointMilli point,
    const BlackCurtainParams& params
) {
    if (state.phase != BlackCurtainPhase::Sweep) {
        return false;
    }
    const std::uint32_t total = state.t_remaining + state.phase_elapsed;
    std::int32_t edge = state.edge_y_milli;
    if (total != 0) {
        const std::int64_t span = static_cast<std::int64_t>(params.sweep_end_y_milli) -
            params.sweep_start_y_milli;
        edge = ClampToInt32(static_cast<std::int64_t>(params.sweep_start_y_milli) +
                            span * static_cast<std::int64_t>(state.phase_elapsed) /
                                static_cast<std::int64_t>(total));
    }
    return CurtainSweepHit(point.x, point.y, edge, params.edge_half_thickness_milli);
}

}  // namespace phk::battle::danmaku
