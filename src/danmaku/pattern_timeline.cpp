#include "phk/battle/danmaku/pattern_timeline.hpp"

#include <cstddef>
#include <utility>

namespace phk::battle::danmaku {
namespace {

SpawnCommand MakeCommand(
    std::int32_t kind,
    double base_angle_deg,
    double spread_deg,
    std::int16_t count,
    float speed
) {
    SpawnCommand cmd;
    cmd.bullet_type = kind;
    cmd.frame_index = 0;
    cmd.spawn_x = kSpawnFollowEmitter;
    cmd.spawn_y = kSpawnFollowEmitter;
    cmd.base_angle_deg = static_cast<float>(base_angle_deg);
    cmd.count = count;
    cmd.spread_or_speed = static_cast<float>(spread_deg);
    cmd.f24 = speed;
    return cmd;
}

BulletTemplate MakeTemplate(int kind, int func_no, double speed) {
    BulletTemplate t;
    t.bullet_kind = kind;
    t.bullet_func_no = func_no;
    t.speed = speed;
    return t;
}

// 10 个真实模式。每个都引用真实弹种（kome/kusabi/ring/...）与真实运动学族。
std::vector<PatternDef> BuildLibrary() {
    std::vector<PatternDef> lib;

    // 1. kome_ring —— A 族：16 向整圆，直线（kome 弹种）。
    {
        PatternDef d;
        d.id = "kome_ring";
        d.family = "A 初始化/直线";
        d.period_ticks = 45;
        d.waves.push_back({0, MakeCommand(45 /*kome_01*/, 0.0, 0.0, 16, 3.0f),
                           MakeTemplate(45, 0, 3.0), AimMode::None, 0.0});
        lib.push_back(std::move(d));
    }

    // 2. kusabi_gap_ring —— A 族：两波错位环，形成缺口（kusabi 弹种）。
    {
        PatternDef d;
        d.id = "kusabi_gap_ring";
        d.family = "A 初始化/直线";
        d.period_ticks = 45;
        d.waves.push_back({0, MakeCommand(9 /*kusabi_01*/, 0.0, 0.0, 12, 2.8f),
                           MakeTemplate(9, 0, 2.8), AimMode::None, 0.0});
        d.waves.push_back({0, MakeCommand(9, 15.0, 0.0, 12, 2.8f),
                           MakeTemplate(9, 0, 2.8), AimMode::None, 0.0});
        lib.push_back(std::move(d));
    }

    // 3. arrow_aimed_fan —— A 族 + 自机瞄准：7 向扇形（arrow 弹种）。
    {
        PatternDef d;
        d.id = "arrow_aimed_fan";
        d.family = "A 初始化/直线";
        d.period_ticks = 45;
        d.waves.push_back({0, MakeCommand(90 /*arrow_01*/, 90.0, 30.0, 7, 4.4f),
                           MakeTemplate(90, 0, 4.4), AimMode::AtShip, 0.0});
        lib.push_back(std::move(d));
    }

    // 4. ring_spiral —— A 族 + 螺旋：每 tick 旋转基础角（ring 弹种）。
    {
        PatternDef d;
        d.id = "ring_spiral";
        d.family = "A 初始化/直线";
        d.period_ticks = 45;
        d.waves.push_back({0, MakeCommand(27 /*ring_01*/, 0.0, 0.0, 4, 3.4f),
                           MakeTemplate(27, 0, 3.4), AimMode::None, 13.0});
        lib.push_back(std::move(d));
    }

    // 5. kome_homing —— D 族：朝自机限速转向（func2，kome 弹种）。
    {
        PatternDef d;
        d.id = "kome_homing";
        d.family = "D 追踪/限速转向";
        d.period_ticks = 45;
        BulletTemplate t = MakeTemplate(45, 2, 2.6);
        t.life = 1;          // func2 需 life>=1 才转向
        t.angle_init = 90.0; // A0：初始参考角
        t.angle_acc = 3.0;   // 单帧最大转角（度）
        d.waves.push_back({0, MakeCommand(45, 90.0, 12.0, 2, 2.6f), t, AimMode::None, 0.0});
        lib.push_back(std::move(d));
    }

    // 6. heart_sine_speed —— C 族：v = V1 + XC·sin(A0 + X·T)（func25，heart 弹种）。
    {
        PatternDef d;
        d.id = "heart_sine_speed";
        d.family = "C 正弦调速";
        d.period_ticks = 45;
        BulletTemplate t = MakeTemplate(174 /*heart_01*/, 25, 3.0);
        t.angle_init = 0.0;
        t.xacc = 6.0;       // X：正弦频率
        t.xacc_cap = 1.5;   // XC：正弦幅度
        d.waves.push_back({0, MakeCommand(174, 90.0, 0.0, 8, 3.0f), t, AimMode::None, 0.0});
        lib.push_back(std::move(d));
    }

    // 7. kunai_bounce —— F 族：边界反弹（func1，kunai 弹种）。
    {
        PatternDef d;
        d.id = "kunai_bounce";
        d.family = "F 边界反弹";
        d.period_ticks = 45;
        d.waves.push_back({0, MakeCommand(126 /*kunai_01*/, 60.0, 20.0, 6, 3.2f),
                           MakeTemplate(126, 1, 3.2), AimMode::None, 0.0});
        lib.push_back(std::move(d));
    }

    // 8. sizuku_sine_radius —— C 族：正弦半径（func81，sizuku 弹种）。
    {
        PatternDef d;
        d.id = "sizuku_sine_radius";
        d.family = "C 正弦半径";
        d.period_ticks = 45;
        BulletTemplate t = MakeTemplate(99 /*sizuku_01*/, 81, 2.4);
        t.angle_init = 0.5;   // A0
        t.f598 = 0.6;         // F98
        t.prev_angle = 4.0;   // PA
        t.xacc_cap = 1.2;     // XC
        t.p2 = 1;
        d.waves.push_back({0, MakeCommand(99, 90.0, 8.0, 5, 2.4f), t, AimMode::None, 0.0});
        lib.push_back(std::move(d));
    }

    // 9. stama_mirror —— F 族：按 P1 奇偶镜像（func32，stama 弹种）。
    {
        PatternDef d;
        d.id = "stama_mirror";
        d.family = "F 镜像";
        d.period_ticks = 45;
        BulletTemplate t = MakeTemplate(36 /*stama_01*/, 32, 3.0);
        t.angle_init = 45.0;  // A0
        t.p1 = 1;             // 奇 → 镜像
        t.p2 = 30;            // 偏移
        d.waves.push_back({0, MakeCommand(36, 0.0, 0.0, 10, 3.0f), t, AimMode::None, 0.0});
        lib.push_back(std::move(d));
    }

    // 10. chou_four_dir —— E/L 族：四方向（func61，chou 弹种）。
    {
        PatternDef d;
        d.id = "chou_four_dir";
        d.family = "E 周期瞄准";
        d.period_ticks = 45;
        d.waves.push_back({0, MakeCommand(156 /*chou_01*/, 0.0, 0.0, 4, 3.0f),
                           MakeTemplate(156, 61, 3.0), AimMode::None, 0.0});
        lib.push_back(std::move(d));
    }

    return lib;
}

}  // namespace

const std::vector<PatternDef>& BossRacePatternLibrary() {
    static const std::vector<PatternDef> kLibrary = BuildLibrary();
    return kLibrary;
}

std::vector<BulletMotion> EmitPatternTick(
    const PatternDef& def,
    std::uint64_t tick,
    const EmitContext& ctx,
    const MotionTargets& targets
) {
    std::vector<BulletMotion> out;
    const std::uint32_t period = (def.period_ticks != 0) ? def.period_ticks : 45;
    const std::uint32_t phase = static_cast<std::uint32_t>(tick % period);

    for (const PatternWave& wave : def.waves) {
        if (wave.tick_offset != phase) {
            continue;
        }
        SpawnCommand cmd = wave.command;

        // 瞄准 / 螺旋：覆盖 base_angle_deg。
        if (wave.aim == AimMode::AtShip && targets.has_my_ship) {
            cmd.base_angle_deg = static_cast<float>(
                AngleToPoint(ctx.emitter_x, ctx.emitter_y, targets.my_ship_x, targets.my_ship_y));
        } else if (wave.aim == AimMode::AtEnemy && targets.has_enemy) {
            cmd.base_angle_deg = static_cast<float>(
                AngleToPoint(ctx.emitter_x, ctx.emitter_y, targets.enemy_x, targets.enemy_y));
        }
        if (wave.spin_per_tick_deg != 0.0) {
            cmd.base_angle_deg = static_cast<float>(
                NormalizeDeg(static_cast<double>(cmd.base_angle_deg) +
                             wave.spin_per_tick_deg * static_cast<double>(tick)));
        }

        std::vector<BulletMotion> emitted = EmitRadial(cmd, wave.tmpl, ctx);
        out.insert(out.end(), emitted.begin(), emitted.end());
    }
    return out;
}

}  // namespace phk::battle::danmaku
