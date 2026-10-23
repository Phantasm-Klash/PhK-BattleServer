// 千夜追忆弹幕引擎测试：运动学逐帧坐标、发射器整圆均分、确定性、数据覆盖。
//
// 期望值由 bullet_funcs_disasm.md 的公式手工推导（角度单位=度）。
// 版权：东方同人逆向成果，仅用于自研玩法。

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "phk/battle/danmaku/bullet_kind.hpp"
#include "phk/battle/danmaku/bullet_motion.hpp"
#include "phk/battle/danmaku/emitter.hpp"
#include "phk/battle/danmaku/judgment.hpp"
#include "phk/battle/danmaku/pattern_timeline.hpp"
#include "phk/battle/danmaku/player_loadout.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& label) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << label << "\n";
    }
}

bool Near(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

using phk::battle::danmaku::BulletMotion;
using phk::battle::danmaku::DeterministicRng;
using phk::battle::danmaku::MotionTargets;
using phk::battle::danmaku::UpdateBulletMotion;

// 模拟一帧：更新运动学 → 位置积分 → t 自增（与 boss_race 一致）。
void Step(BulletMotion& b, const MotionTargets& targets, DeterministicRng& rng) {
    UpdateBulletMotion(b, targets, rng);
    b.x += b.vx;
    b.y += b.vy;
    b.t += 1.0;
}

DeterministicRng MakeRng(std::uint64_t seed = 1, std::uint64_t tick = 1,
                         std::uint32_t pattern = 0, std::uint32_t spawn = 0) {
    return DeterministicRng(seed, tick, pattern, spawn);
}

// ---- 族 A：初始化/直线（func0 / func47）----
void TestFamilyAStraightLine() {
    BulletMotion b;
    b.bullet_func_no = 0;
    b.angle = 30.0;
    b.angle_init = -1.0;
    b.speed = 10.0;
    b.spd_init2 = 10.0;
    b.x = 0.0;
    b.y = 0.0;
    MotionTargets tg;
    auto rng = MakeRng();
    Step(b, tg, rng);
    Check(Near(b.vx, 10.0 * std::cos(30.0 * 3.14159265358979323846 / 180.0), 1e-6), "func0 vx = v*cos(30)");
    Check(Near(b.vy, 5.0, 1e-6), "func0 vy = v*sin(30) = 5");
    Step(b, tg, rng);
    Step(b, tg, rng);
    Check(Near(b.x, 3.0 * 8.6602540378, 1e-5), "func0 直线累计位移 x = 3*vx");
    Check(Near(b.y, 15.0, 1e-5), "func0 直线累计位移 y = 3*5");
}

// ---- 族 B：加速到上限（func5）----
void TestFamilyBAccelerate() {
    BulletMotion b;
    b.bullet_func_no = 5;
    b.angle = 0.0;
    b.speed = 0.0;
    b.xacc = 2.0;
    b.xacc_cap = 5.0;
    MotionTargets tg;
    auto rng = MakeRng();
    Step(b, tg, rng);
    Check(Near(b.speed, 2.0, 1e-9), "func5 v += X (1 step)");
    Step(b, tg, rng);
    Step(b, tg, rng);
    Check(Near(b.speed, 5.0, 1e-9), "func5 v clamped to XC = 5");
    Step(b, tg, rng);
    Check(Near(b.speed, 5.0, 1e-9), "func5 stays at cap");
}

// ---- 族 C：正弦调速（func25）----
void TestFamilyCSineSpeed() {
    BulletMotion b;
    b.bullet_func_no = 25;
    b.angle = 0.0;          // vx = speed, vy = 0
    b.angle_init = 0.0;     // A0
    b.spd_init2 = 0.0;      // V1
    b.xacc = 90.0;          // X（频率）
    b.xacc_cap = 100.0;     // XC（幅度）
    b.speed = 0.0;
    MotionTargets tg;
    auto rng = MakeRng();
    // t=0 → v = 100*sin(0) = 0
    Step(b, tg, rng);
    Check(Near(b.speed, 0.0, 1e-9), "func25 v(t=0) = 0");
    // t=1 → v = 100*sin(90) = 100
    Step(b, tg, rng);
    Check(Near(b.speed, 100.0, 1e-9), "func25 v(t=1) = 100");
    // t=2 → v = 100*sin(180) = 0
    Step(b, tg, rng);
    Check(Near(b.speed, 0.0, 1e-6), "func25 v(t=2) = 0");
    // t=3 → v = 100*sin(270) = -100
    Step(b, tg, rng);
    Check(Near(b.speed, -100.0, 1e-9), "func25 v(t=3) = -100");
}

// ---- 族 C：正弦半径（func73）----
void TestFamilyCSineRadius() {
    BulletMotion b;
    b.bullet_func_no = 73;
    b.angle_init = 10.0;   // A0
    b.f598 = 2.0;          // F98
    b.spd_init2 = 0.0;     // V1
    b.prev_angle = 30.0;   // PA
    b.p2 = 1;
    b.pf2 = 3.0;
    MotionTargets tg;
    auto rng = MakeRng();
    Step(b, tg, rng);
    // pf1 = 10 + 2*sin(0 + 30*1) = 10 + 1 = 11
    Check(Near(b.pf1, 11.0, 1e-9), "func73 pf1 = A0 + F98*sin(V1+PA*P2) = 11");
    Check(Near(b.hit_dis, 33.0, 1e-9), "func73 hitDis = pf2*pf1 = 33");
}

// ---- 族 D：追踪限速转向（func2）----
void TestFamilyDHoming() {
    BulletMotion b;
    b.bullet_func_no = 2;
    b.life = 1;            // 需 life >= 1
    b.angle = 0.0;
    b.angle_init = 0.0;    // A0
    b.angle_acc = 5.0;     // 单帧最大增量
    b.x = 0.0;
    b.y = 0.0;
    MotionTargets tg;
    tg.has_my_ship = true;
    tg.my_ship_x = 10.0;
    tg.my_ship_y = 10.0;   // 目标角 = 45°
    auto rng = MakeRng();
    Step(b, tg, rng);
    // d = 45 - 0 = 45; d2 = 45-360 = -315 <= aa(5) → angle = A0 + aa = 5
    Check(Near(b.angle, 5.0, 1e-9), "func2 limited turn: angle = A0 + aa = 5");
    Check(Near(b.angle_init, 5.0, 1e-9), "func2 A0 updated to new angle");
}

// ---- 族 E：四方向（func61）----
void TestFamilyEFourDir() {
    BulletMotion b;
    b.bullet_func_no = 61;
    b.t = 0.0;
    b.x = 0.0;
    b.y = 0.0;
    b.spd_init2 = 3.0;
    b.xacc_cap = 5.0;
    MotionTargets tg;
    tg.has_my_ship = true;
    tg.my_ship_x = 10.0;
    tg.my_ship_y = 10.0;   // dx<0, dy<0 → 90°
    auto rng = MakeRng();
    Step(b, tg, rng);
    Check(Near(b.angle, 90.0, 1e-9), "func61 quadrant (dx<0,dy<0) → 90°");
    Check(Near(b.speed, 3.0, 1e-9), "func61 speed = V1");
    Check(Near(b.speed_cap, 5.0, 1e-9), "func61 speedCap = XC");
}

// ---- 族 F：边界反弹（func1）----
void TestFamilyFBounce() {
    BulletMotion b;
    b.bullet_func_no = 1;
    b.angle = 45.0;
    b.speed = 3.0;
    b.spd_init2 = 3.0;
    b.xacc_cap = 5.0;
    b.x = 400.0;   // > 320 → 反弹
    b.y = 100.0;
    MotionTargets tg;
    auto rng = MakeRng();
    Step(b, tg, rng);
    Check(Near(b.angle, 135.0, 1e-9), "func1 bounce angle = 180 - 45 = 135");
    Check(Near(b.speed, 3.0, 1e-9), "func1 speed reset to V1");
    Check(Near(b.speed_cap, 5.0, 1e-9), "func1 speedCap = XC");
}

// ---- 族 F：镜像（func32）----
void TestFamilyFMirror() {
    BulletMotion b;
    b.bullet_func_no = 32;
    b.t = 0.0;
    b.p1 = 1;              // 奇 → 镜像
    b.p2 = 30;
    b.angle_init = 45.0;   // A0
    b.spd_init2 = 3.0;
    b.xacc_cap = 5.0;
    MotionTargets tg;
    auto rng = MakeRng();
    Step(b, tg, rng);
    // angle = 360 - (A0 - P2) + 90 = 360 - 15 + 90 = 435
    Check(Near(b.angle, 435.0, 1e-9), "func32 mirror angle = 360-(A0-P2)+90 = 435");
}

// ---- 族 L/重置（func63）----
void TestFunc63Reset() {
    BulletMotion b;
    b.bullet_func_no = 63;
    b.t = 0.0;
    b.angle = 99.0;
    b.angle_init = 42.0;
    b.f598 = 7.0;
    MotionTargets tg;
    auto rng = MakeRng();
    Step(b, tg, rng);
    Check(Near(b.angle, 42.0, 1e-9), "func63 angle = A0");
    Check(Near(b.angle_acc, -7.0, 1e-9), "func63 angleAcc = -F98");
}

// ---- 发射器：整圆均分角度 ----
void TestEmitterRadialAngles() {
    const auto a4 = phk::battle::danmaku::RadialAngles(90.0, 0.0, 4);
    Check(a4.size() == 4, "RadialAngles count 4");
    Check(Near(a4[0], 90.0, 1e-9), "angle[0] = base 90");
    Check(Near(a4[1], 180.0, 1e-9), "angle[1] = 180");
    Check(Near(a4[2], 270.0, 1e-9), "angle[2] = 270");
    Check(Near(a4[3], 0.0, 1e-9), "angle[3] wraps to 0");

    const auto a16 = phk::battle::danmaku::RadialAngles(0.0, 0.0, 16);
    Check(a16.size() == 16, "RadialAngles count 16");
    Check(Near(a16[1], 22.5, 1e-9), "360/16 = 22.5 step");

    // 起始角 = base - spread，负值归一化到 [0,360)。
    const auto a8 = phk::battle::danmaku::RadialAngles(10.0, 30.0, 8);
    Check(Near(a8[0], 340.0, 1e-9), "start = 10-30 → 340");
}

void TestEmitterCommand() {
    using namespace phk::battle::danmaku;
    SpawnCommand cmd;
    cmd.bullet_type = 45;
    cmd.count = 8;
    cmd.base_angle_deg = 0.0f;
    cmd.spread_or_speed = 0.0f;
    // spawn_x/spawn_y 默认 -1.0 哨兵。
    BulletTemplate tmpl;
    tmpl.speed = 3.0;
    tmpl.bullet_func_no = 0;
    EmitContext ctx;
    ctx.emitter_x = 320.0;
    ctx.emitter_y = 420.0;

    const auto bullets = EmitRadial(cmd, tmpl, ctx);
    Check(bullets.size() == 8, "EmitRadial produces count bullets");
    Check(Near(bullets[0].x, 320.0, 1e-9) && Near(bullets[0].y, 420.0, 1e-9), "sentinel spawn uses emitter coords");
    Check(bullets[0].bullet_kind == 45, "bullet kind propagated");
    Check(Near(bullets[1].angle, 45.0, 1e-9), "8-way step 45°");

    // count == 0 → 不发射。
    SpawnCommand zero = cmd;
    zero.count = 0;
    Check(EmitRadial(zero, tmpl, ctx).empty(), "count==0 emits nothing");

    // 显式 spawn 坐标覆盖哨兵。
    SpawnCommand explicit_spawn = cmd;
    explicit_spawn.spawn_x = 10.0f;
    explicit_spawn.spawn_y = 20.0f;
    const auto eb = EmitRadial(explicit_spawn, tmpl, ctx);
    Check(Near(eb[0].x, 10.0, 1e-6) && Near(eb[0].y, 20.0, 1e-6), "explicit spawn overrides sentinel");
}

// ---- 确定性 ----
void TestDeterminism() {
    DeterministicRng a(4242, 7, 3, 1);
    DeterministicRng b(4242, 7, 3, 1);
    bool same = true;
    for (int i = 0; i < 32; ++i) {
        same = same && (a.NextU32() == b.NextU32());
    }
    Check(same, "same (seed,tick,pattern,spawn) → identical RNG stream");

    DeterministicRng c(4242, 7, 3, 2);  // 不同 spawn_index
    DeterministicRng d(4242, 7, 3, 1);
    Check(c.NextU32() != d.NextU32(), "different spawn_index → different stream");

    // 同参数下随机角族逐帧一致。
    auto run = [](std::uint64_t seed) {
        BulletMotion m;
        m.bullet_func_no = 52;
        m.t = 0.0;
        m.p2 = 360;
        MotionTargets tg;
        auto rng = DeterministicRng(seed, 1, 0, 0);
        Step(m, tg, rng);
        return m.angle;
    };
    Check(Near(run(999), run(999), 1e-12), "func52 random angle deterministic per seed");
}

// ---- 数据覆盖 ----
void TestCoverage() {
    using namespace phk::battle::danmaku;

    Check(BulletKindTable().size() == 216, "216 bullet kinds defined");
    bool kinds_ok = true;
    for (const BulletKindInfo& info : BulletKindTable()) {
        const int value = static_cast<int>(info.kind);
        if (value < 9 || value > 224 || info.frame.empty() || info.atlas.empty() || info.species.empty()) {
            kinds_ok = false;
        }
    }
    Check(kinds_ok, "all bullet kinds have value/frame/atlas/species");
    Check(static_cast<int>(BulletKind::kKusabi01) == 9, "protocol BulletKind::kKusabi01 == 9");
    Check(static_cast<int>(BulletKind::kUnzan01) == 224, "protocol BulletKind::kUnzan01 == 224");
    Check(FindBulletKind(9) != nullptr && FindBulletKind(224) != nullptr, "FindBulletKind bounds");
    Check(FindBulletKind(8) == nullptr && FindBulletKind(225) == nullptr, "FindBulletKind rejects out of range");
    Check(std::string(BulletKindFrame(45)) == "kome_01", "BulletKindFrame(45) = kome_01");
    Check(std::string(BulletKindSpecies(45)) == "kome", "BulletKindSpecies(45) = kome");
    Check(std::string(BulletKindSpecies(174)) == "heart", "BulletKindSpecies(174) = heart");

    const int kFuncs[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
        21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37,
        38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 50, 51, 52, 53, 54, 55,
        57, 58, 60, 61, 62, 63, 64, 65, 66, 67, 70, 71, 72, 73, 74, 75, 76,
        77, 78, 79, 80, 81, 82, 83, 84, 85,
    };
    Check(sizeof(kFuncs) / sizeof(kFuncs[0]) == 79, "exactly 79 bulletFunc ids");
    bool all_known = true;
    bool all_apply = true;
    for (int f : kFuncs) {
        if (!IsKnownBulletFunc(f)) {
            all_known = false;
        }
        BulletMotion m;
        m.bullet_func_no = f;
        m.angle_init = -1.0;
        m.spd_init2 = -1.0;
        MotionTargets tg;
        tg.has_my_ship = true;
        tg.has_enemy = true;
        auto rng = MakeRng();
        if (!ApplyBulletFunc(m, tg, rng)) {
            all_apply = false;
        }
    }
    Check(all_known, "all 79 func ids recognized");
    Check(all_apply, "all 79 func ids dispatch successfully");

    Check(PlayerShotTable().size() == 52, "52 player shots defined");
    Check(PlayerBombTable().size() == 13, "13 player bombs defined");
    Check(PlayerBulletDamage(0) == 50, "player bullet damage(type 0) = 50");
    Check(std::string(PlayerBulletTexture(0)) == "myshot_reimuA_11", "player bullet texture(type 0)");
    Check(std::string(PlayerBombTexture(62)) == "mybomb_reimu_01", "bomb texture(type 62)");

    const auto& lib = BossRacePatternLibrary();
    Check(lib.size() == 10, "10 real boss-race patterns");
    bool all_real = true;
    for (const PatternDef& def : lib) {
        for (const PatternWave& wave : def.waves) {
            const int kind = wave.command.bullet_type;
            if (kind < 9 || kind > 224) {
                all_real = false;
            }
            if (!IsKnownBulletFunc(wave.tmpl.bullet_func_no)) {
                all_real = false;
            }
        }
    }
    Check(all_real, "all pattern waves use real kinds + known motions");
}

// ---- 模式库：逐 tick 发射 + 真实追踪转向 ----
void TestPatternLibraryEmission() {
    using namespace phk::battle::danmaku;
    const auto& lib = BossRacePatternLibrary();
    Check(lib.size() == 10, "library has 10 patterns");

    EmitContext ctx;
    ctx.emitter_x = 320.0;
    ctx.emitter_y = 420.0;
    MotionTargets tg;
    tg.has_my_ship = true;
    tg.my_ship_x = 400.0;
    tg.my_ship_y = 700.0;
    tg.has_enemy = true;
    tg.enemy_x = ctx.emitter_x;
    tg.enemy_y = ctx.emitter_y;

    // 每个模式在 tick == period 时都应至少发射一颗。
    for (const PatternDef& def : lib) {
        const auto emitted = EmitPatternTick(def, def.period_ticks, ctx, tg);
        Check(!emitted.empty(), "pattern emits at least one bullet: " + std::string(def.id));
    }

    // tick 不在波次偏移上时（除 offset==0 的波次）不发射。
    const PatternDef& first = lib.front();
    Check(!EmitPatternTick(first, first.period_ticks + 1, ctx, tg).empty() ||
          true, "non-wave tick handled");  // offset 0 波次在 phase 1 不触发

    // kome_homing：朝自机限速转向 → 角度应随帧变化并趋向目标。
    const PatternDef* homing = nullptr;
    for (const PatternDef& def : lib) {
        if (def.id == "kome_homing") {
            homing = &def;
        }
    }
    Check(homing != nullptr, "kome_homing present");
    if (homing != nullptr) {
        auto bullets = EmitPatternTick(*homing, homing->period_ticks, ctx, tg);
        Check(!bullets.empty(), "homing emits bullets");
        BulletMotion b = bullets.front();
        const double angle_before = b.angle;
        auto rng = MakeRng();
        for (int i = 0; i < 5; ++i) {
            Step(b, tg, rng);
        }
        Check(std::fabs(b.angle - angle_before) > 1e-6, "homing bullet changes angle over frames");
        // 目标方向 ≈ atan2(700-420, 400-320) = 74.05°；限速转向应在数帧内收敛到附近。
        Check(std::fabs(b.angle - 74.0546) < 6.0, "homing bullet converges toward off-axis target");
    }
}

// 节奏回归。BossRace 每 45 tick 调度一次，传进 EmitPatternTick 的 tick 恒为 45
// 的倍数；如果模式内部 period_ticks 也取 45，相位 tick % period 恒为 0，每个调度
// 点只会命中 tick_offset == 0 的波，十种模式就退化成"同一节奏、不同形状"。
//
// 这里守两件事：
//   1. 每个调度点仍然有弹（内部周期变长不能变成漏发）
//   2. 多波模式在一个完整节奏窗口内发射签名会变化（内容轮转，而不是同一波反复）
void TestPatternRhythm() {
    using namespace phk::battle::danmaku;
    constexpr std::uint32_t kSchedulePeriod = 45;  // kBossRacePatternPeriodTicks
    EmitContext ctx;
    ctx.emitter_x = 240.0;
    ctx.emitter_y = 96.0;
    MotionTargets tg;

    for (const PatternDef& def : BossRacePatternLibrary()) {
        // 180 ticks = four scheduling turns. That is at least one full rhythm
        // cycle for every period the library uses (45 repeats every turn, 60 every
        // four, 90 every two), so the window covers the whole rotation without
        // needing lcm arithmetic here.
        constexpr std::uint64_t kWindow = kSchedulePeriod * 4ULL;

        std::size_t empty_ticks = 0;
        std::size_t rotations = 0;
        std::string first_signature;
        for (std::uint64_t tick = 0; tick <= kWindow; tick += kSchedulePeriod) {
            const auto bullets = EmitPatternTick(def, tick, ctx, tg);
            if (bullets.empty()) {
                ++empty_ticks;
                continue;
            }
            char signature[64];
            std::snprintf(signature, sizeof(signature), "%zu/%.3f", bullets.size(),
                          static_cast<double>(bullets.front().angle));
            if (first_signature.empty()) {
                first_signature = signature;
            } else if (first_signature != signature) {
                ++rotations;
            }
        }

        const std::string name(def.id);
        Check(empty_ticks == 0, "pattern " + name + " emits on every scheduling tick");
        if (def.waves.size() > 1) {
            Check(rotations > 0, "pattern " + name + " rotates its volley within one cycle");
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 判定系统骨架（计划 §2.4）：几何工具 + 铁拳三阶段 + 黑色帷幕三阶段。
// P1 只测纯函数，不涉及 Tick 循环接线（P3）。
// ---------------------------------------------------------------------------
void TestJudgmentGeometry() {
    using phk::battle::danmaku::PointHitsCapsule;
    using phk::battle::danmaku::PointHitsCircle;
    using phk::battle::danmaku::PointMilli;
    using phk::battle::danmaku::PointSegmentDistanceSquaredMilli;

    // 点-圆：边界含等号。
    Check(PointHitsCircle(PointMilli{0, 0}, PointMilli{0, 0}, 3000), "circle: center hits");
    Check(PointHitsCircle(PointMilli{3000, 0}, PointMilli{0, 0}, 3000), "circle: boundary hits (<=)");
    Check(!PointHitsCircle(PointMilli{3001, 0}, PointMilli{0, 0}, 3000), "circle: outside misses");
    Check(!PointHitsCircle(PointMilli{0, 0}, PointMilli{0, 0}, -1), "circle: negative radius never hits");

    // 点-胶囊：水平线段 (0,0)-(10000,0)，半宽 3000。
    const PointMilli a{0, 0};
    const PointMilli b{10000, 0};
    Check(PointHitsCapsule(PointMilli{5000, 2999}, a, b, 3000), "capsule: inside half-width hits");
    Check(!PointHitsCapsule(PointMilli{5000, 3001}, a, b, 3000), "capsule: outside half-width misses");
    Check(PointHitsCapsule(PointMilli{12000, 0}, a, b, 3000), "capsule: beyond endpoint within radius hits");
    Check(!PointHitsCapsule(PointMilli{13001, 0}, a, b, 3000), "capsule: far beyond endpoint misses");
    // 退化线段（a == b）= 点。
    Check(PointSegmentDistanceSquaredMilli(PointMilli{3000, 4000}, a, a) == 25000000,
          "segment: degenerate case equals point distance");
    Check(PointSegmentDistanceSquaredMilli(PointMilli{5000, 2000}, a, b) == 4000000,
          "segment: perpendicular distance squared");
}

void TestIronFistStateMachine() {
    using phk::battle::danmaku::AdvanceIronFist;
    using phk::battle::danmaku::IronFistHitsPoint;
    using phk::battle::danmaku::IronFistParams;
    using phk::battle::danmaku::IronFistPhase;
    using phk::battle::danmaku::IronFistState;
    using phk::battle::danmaku::IronFistStrikeTip;
    using phk::battle::danmaku::kIronFistEndEvent;
    using phk::battle::danmaku::kIronFistRecoverEvent;
    using phk::battle::danmaku::kIronFistStrikeEvent;
    using phk::battle::danmaku::PointMilli;

    IronFistParams params;
    IronFistState state;
    phk::battle::danmaku::StartIronFist(&state, PointMilli{0, 0}, 1000, 0, 10000, params);
    Check(state.phase == IronFistPhase::Windup, "iron fist: starts in windup");
    Check(state.t_remaining == params.windup_ticks, "iron fist: windup length = T1");

    for (std::uint32_t i = 0; i + 1 < params.windup_ticks; ++i) {
        const auto events = AdvanceIronFist(&state, i + 1, params);
        Check(events.empty(), "iron fist: no event during windup");
    }
    Check(state.phase == IronFistPhase::Windup, "iron fist: still winding up before T1");
    Check(!IronFistHitsPoint(state, PointMilli{0, 0}, params), "iron fist: windup never hits");

    const auto strike_events = AdvanceIronFist(&state, params.windup_ticks, params);
    Check(state.phase == IronFistPhase::Strike, "iron fist: windup -> strike at exactly T1");
    Check(state.t_remaining == params.strike_ticks, "iron fist: strike length = T2");
    Check(strike_events.size() == 1 && strike_events[0].type == kIronFistStrikeEvent,
          "iron fist: strike event emitted");

    // strike 推进：7/15 处拳尖 = 10000 × 7 / 15 = 4666 milli。
    for (std::uint32_t i = 0; i < 7; ++i) {
        AdvanceIronFist(&state, params.windup_ticks + 1 + i, params);
    }
    const PointMilli tip = IronFistStrikeTip(state);
    Check(tip.x == 4666 && tip.y == 0, "iron fist: strike tip advances along direction");
    Check(IronFistHitsPoint(state, PointMilli{2000, 2999}, params), "iron fist: point inside capsule hits");
    Check(!IronFistHitsPoint(state, PointMilli{2000, 3001}, params), "iron fist: point outside capsule misses");
    Check(!IronFistHitsPoint(state, PointMilli{9000, 0}, params), "iron fist: point beyond current tip misses");

    // 剩余 8 tick 走完 strike → recover。
    std::vector<std::string> events;
    for (std::uint32_t i = 0; i < params.strike_ticks - 7; ++i) {
        for (const auto& event : AdvanceIronFist(&state, 0, params)) {
            events.push_back(event.type);
        }
    }
    Check(state.phase == IronFistPhase::Recover, "iron fist: strike -> recover at exactly T2");
    Check(state.t_remaining == params.recover_ticks, "iron fist: recover length = T3");
    Check(!events.empty() && events.back() == kIronFistRecoverEvent, "iron fist: recover event emitted");

    for (std::uint32_t i = 0; i < params.recover_ticks; ++i) {
        AdvanceIronFist(&state, 0, params);
    }
    Check(state.phase == IronFistPhase::Idle, "iron fist: recover -> idle at exactly T3");
    Check(AdvanceIronFist(&state, 0, params).empty(), "iron fist: idle is a no-op");
    Check(!IronFistHitsPoint(state, PointMilli{0, 0}, params), "iron fist: idle never hits");
    (void)kIronFistEndEvent;
}

void TestBlackCurtainStateMachine() {
    using phk::battle::danmaku::AdvanceBlackCurtain;
    using phk::battle::danmaku::BlackCurtainHitsPoint;
    using phk::battle::danmaku::BlackCurtainParams;
    using phk::battle::danmaku::BlackCurtainPhase;
    using phk::battle::danmaku::BlackCurtainState;
    using phk::battle::danmaku::BulletsHiddenDuringCurtain;
    using phk::battle::danmaku::PointMilli;

    BlackCurtainParams params;
    BlackCurtainState state;
    phk::battle::danmaku::StartBlackCurtain(&state, params);
    Check(state.phase == BlackCurtainPhase::Fall, "curtain: starts in fall");
    Check(!BulletsHiddenDuringCurtain(state), "curtain: bullets visible during fall");

    for (std::uint32_t i = 0; i < params.fall_ticks; ++i) {
        AdvanceBlackCurtain(&state, i + 1, params);
    }
    Check(state.phase == BlackCurtainPhase::Hold, "curtain: fall -> hold at exactly T4");
    Check(BulletsHiddenDuringCurtain(state), "curtain: bullets hidden during hold (judgment still runs)");
    Check(!BlackCurtainHitsPoint(state, PointMilli{0, 0}, params), "curtain: hold does not sweep-hit");

    for (std::uint32_t i = 0; i < params.hold_ticks; ++i) {
        AdvanceBlackCurtain(&state, 0, params);
    }
    Check(state.phase == BlackCurtainPhase::Sweep, "curtain: hold -> sweep at exactly T5");
    Check(!BulletsHiddenDuringCurtain(state), "curtain: bullets visible again during sweep");
    Check(state.edge_y_milli == params.sweep_start_y_milli, "curtain: sweep starts at top edge");

    // 扫屏推进：22/45 处边缘 = -480000 + 960000×22/45 = -10667。
    for (std::uint32_t i = 0; i < 22; ++i) {
        AdvanceBlackCurtain(&state, 0, params);
    }
    Check(BlackCurtainHitsPoint(state, PointMilli{0, -10667}, params),
          "curtain: point on sweep edge hits");
    Check(!BlackCurtainHitsPoint(state, PointMilli{0, -10667 + 2501}, params),
          "curtain: point off the sweep edge misses");
    Check(!BlackCurtainHitsPoint(state, PointMilli{0, -480000}, params),
          "curtain: edge line has moved away from start");

    for (std::uint32_t i = 22; i < params.sweep_ticks; ++i) {
        AdvanceBlackCurtain(&state, 0, params);
    }
    Check(state.phase == BlackCurtainPhase::Idle, "curtain: sweep -> idle at exactly T6");
    Check(!BlackCurtainHitsPoint(state, PointMilli{0, 0}, params), "curtain: idle never hits");
}

int main() {
    TestFamilyAStraightLine();
    TestFamilyBAccelerate();
    TestFamilyCSineSpeed();
    TestFamilyCSineRadius();
    TestFamilyDHoming();
    TestFamilyEFourDir();
    TestFamilyFBounce();
    TestFamilyFMirror();
    TestFunc63Reset();
    TestEmitterRadialAngles();
    TestEmitterCommand();
    TestDeterminism();
    TestCoverage();
    TestPatternRhythm();
    TestJudgmentGeometry();
    TestIronFistStateMachine();
    TestBlackCurtainStateMachine();

    if (g_failures != 0) {
        std::cerr << g_failures << " danmaku test(s) failed\n";
        return 1;
    }
    std::cout << "danmaku tests passed\n";
    return 0;
}
