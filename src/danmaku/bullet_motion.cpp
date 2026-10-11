// 千夜追忆 1.40 EnemyBullet 运动学实现（79 个 bulletFunc）。
//
// 移植自 rev/patterns/bullet_funcs_reimpl.hpp + bullet_funcs_disasm.md。
// 角度单位 = 度。每个函数注释标注原版地址、置信度（实测/推断）。
// 版权：东方同人逆向成果，仅用于自研玩法。
#include "phk/battle/danmaku/bullet_motion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace phk::battle::danmaku {
namespace {

// 原版 .rodata 常量（bullet_funcs_disasm.md §1）。
constexpr double kHalfW = 320.0;   // @0xac5188 屏幕半宽
constexpr double kHalfH = 480.0;   // @0xac518c 屏幕半高
constexpr double kTop = 40.0;      // @0xace180 上边界留白
constexpr double kDeg180 = 180.0;
constexpr double kDeg360 = 360.0;
constexpr double kNeg1 = -1.0;

double AngleToMyShip(const BulletMotion& b, const MotionTargets& tg) {
    if (!tg.has_my_ship) {
        return b.angle;
    }
    return AngleToPoint(b.x, b.y, tg.my_ship_x, tg.my_ship_y);
}

double AngleToEnemy(const BulletMotion& b, const MotionTargets& tg) {
    if (!tg.has_enemy) {
        return b.angle;
    }
    return AngleToPoint(b.x, b.y, tg.enemy_x, tg.enemy_y);
}

double DistSqToMyShip(const BulletMotion& b, const MotionTargets& tg) {
    if (!tg.has_my_ship) {
        return 1.0e18;
    }
    const double dx = b.x - tg.my_ship_x;
    const double dy = b.y - tg.my_ship_y;
    return dx * dx + dy * dy;
}

double DistSqToEnemy(const BulletMotion& b, const MotionTargets& tg) {
    if (!tg.has_enemy) {
        return 1.0e18;
    }
    const double dx = b.x - tg.enemy_x;
    const double dy = b.y - tg.enemy_y;
    return dx * dx + dy * dy;
}

// ===========================================================================
// A 初始化/直线
// ===========================================================================

// bulletFunc0 @0x50cdcc —— 基础初始化 + 直线。[实测]
void Func0(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (b.angle_init != kNeg1) {
        b.angle = b.angle_init;
    }
    if (b.speed_init != kNeg1) {
        b.angle_spd = b.speed_init;
        b.angle_acc = b.f598;
        b.f56c = b.prev_angle;
    }
    if (b.spd_init2 != kNeg1) {
        b.speed = b.spd_init2;
    }
    if (b.xacc != kNeg1) {
        b.accel = b.xacc;
    }
    if (b.xacc_cap != kNeg1) {
        b.speed_cap = b.xacc_cap;
    }
}

// bulletFunc4 @0x50d238 —— v = V1（速度重置）。[实测]
void Func4(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    b.speed = b.spd_init2;
}

// bulletFunc29 @0x511384 —— 分段重置 + 合成 vx/vy。[实测]
void Func29(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    const int t = static_cast<int>(b.t);
    if (t < b.life) {
        b.vx = b.prev_angle * CosDeg(b.angle);
        b.vy = b.prev_angle * SinDeg(b.angle);
    }
    if (t == b.life) {
        b.angle = b.angle_init;
        b.vx = 0.0;
    }
    if (t >= b.life) {
        b.angle = b.angle_init;
        b.speed = b.spd_init2;
        b.angle_acc = b.f598;
        b.speed_cap = b.xacc_cap;
    }
}

// bulletFunc47 @0x5114a8 —— 每帧重算 vx/vy（直线）。[实测]
void Func47(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (static_cast<int>(b.t) == 0) {
        b.vx = 0.0;
    }
}

// ===========================================================================
// B 加速/减速
// ===========================================================================

// bulletFunc5 @0x50d250 —— v += X，夹到 XC。[实测]
void Func5(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    const double a = b.xacc;
    if (a == 0.0) {
        return;
    }
    if (a > 0.0) {
        if (b.speed < b.xacc_cap) {
            b.speed += a;
            if (b.speed > b.xacc_cap) {
                b.speed = b.xacc_cap;
            }
        }
    } else {
        if (b.speed > b.xacc_cap) {
            b.speed += a;
            if (b.speed < b.xacc_cap) {
                b.speed = b.xacc_cap;
            }
        }
    }
}

// bulletFunc35 @0x50ec24 —— 接近自机 → 指数减速 + 朝自机。[实测/推断阈值]
void Func35(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (b.p2 != 1) {
        if (DistSqToMyShip(b, tg) <= static_cast<double>(b.life)) {
            b.p2 = 1;
        }
    }
    if (b.p2 == 1) {
        b.speed_cap = 0.0;
        b.accel = b.speed * (-0.1);
    }
    if (static_cast<int>(b.t) == b.p1) {
        if (b.angle_init > 1.0) {
            b.angle = AngleToMyShip(b, tg);
        }
        b.speed = b.spd_init2;
        b.speed_cap = b.xacc_cap;
    }
}

// bulletFunc39 @0x50ef00 —— 状态机：线性减速 va = -v/L。[实测]
void Func39(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (b.p2 == 0) {
        return;
    }
    if (b.p2 == 1) {
        b.speed_cap = 0.0;
        b.accel = -(b.speed / static_cast<double>(b.life));
        b.angle = b.angle_init;
    }
}

// bulletFunc43 @0x50f500 —— v = V1 + X·sin(XC·T)；hitDis = pf2·v。[实测]
void Func43(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (b.p2 >= b.life) {
        const double s = b.spd_init2 + b.xacc * SinDeg(b.xacc_cap * b.t);
        b.speed = s;
        b.hit_dis = b.pf2 * s;
    }
}

// ===========================================================================
// C 正弦调速 / 正弦角
// ===========================================================================

// bulletFunc25 @0x50e194 —— v = V1 + XC·sin(A0 + X·T)。[实测]
void Func25(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    const double arg = b.angle_init + b.xacc * b.t;
    b.speed = b.spd_init2 + b.xacc_cap * SinDeg(arg);
}

// bulletFunc40 @0x50f08c —— aa = A0 + F98·sin(V1 + PA·T)。[实测]
void Func40(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (static_cast<int>(b.t) < b.life) {
        const double arg = b.spd_init2 + b.prev_angle * b.t;
        b.angle_acc = b.angle_init + b.f598 * SinDeg(arg);
    }
}

// bulletFunc73 @0x510bbc —— pf1 = A0 + F98·sin(V1 + PA·P2)；hitDis = pf2·pf1。[实测]
void Func73(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    const double arg = b.spd_init2 + b.prev_angle * static_cast<double>(b.p2);
    b.pf1 = b.angle_init + b.f598 * SinDeg(arg);
    b.hit_dis = b.pf2 * b.pf1;
}

// bulletFunc81 @0x511d48 —— pf1 = A0 + F98·sin(PA·P2)；v = V1 + XC·s。[实测]
void Func81(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    const double s = SinDeg(b.prev_angle * static_cast<double>(b.p2));
    b.pf1 = b.angle_init + b.f598 * s;
    b.speed = b.spd_init2 + b.xacc_cap * s;
    b.hit_dis = b.pf2 * b.pf1;
}

// ===========================================================================
// D 追踪 / 限速转向
// ===========================================================================

// 限速转向共用逻辑：朝 target 转，单帧增量受 max_step 限制。
// [推断：原 disasm 的条件极性存疑；此处按「朝目标限速转向」的文档语义实现，
//  并对角差做最短路径归一化。]
void RateLimitedTurn(BulletMotion& b, double target, double max_step) {
    double d = target - b.angle_init;
    while (d > 180.0) {
        d -= 360.0;
    }
    while (d < -180.0) {
        d += 360.0;
    }
    if (d > max_step) {
        b.angle = b.angle_init + max_step;
    } else if (d < -max_step) {
        b.angle = b.angle_init - max_step;
    } else {
        b.angle = target;
    }
    b.angle_init = b.angle;
}

// bulletFunc2 @0x50d16c —— 朝自机限速转向，单帧增量 ≤ aa。[实测/推断条件]
void Func2(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (b.life < 1) {
        return;
    }
    RateLimitedTurn(b, AngleToMyShip(b, tg), b.angle_acc);
}

// bulletFunc17 @0x50db94 —— 限速追踪（单帧 ≤ A0）。[实测]
void Func17(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    const double target = AngleToMyShip(b, tg);
    if (static_cast<int>(b.t) == b.life) {
        b.prev_angle = target;
        return;
    }
    if (static_cast<int>(b.t) > b.life) {
        double t = target;
        const double d = t - b.prev_angle;
        if (d > b.angle_init) {
            t = b.prev_angle + b.angle_init;
        }
        if (d < -b.angle_init) {
            t = b.prev_angle - b.angle_init;
        }
        b.angle = t;
        b.prev_angle = b.angle;
    }
}

// bulletFunc18 @0x50dc4c —— 同 17，P2 作目标选择标记。[推断：目标选择/限速细节]
void Func18(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    const double target = (b.p2 == 2) ? AngleToEnemy(b, tg) : AngleToMyShip(b, tg);
    if (static_cast<int>(b.t) < b.life) {
        return;
    }
    RateLimitedTurn(b, target, (b.angle_init > 0.0) ? b.angle_init : b.angle_acc);
}

// bulletFunc48 @0x50f6c4 —— T==L 时瞄准。[实测]
void Func48(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (static_cast<int>(b.t) < b.life) {
        return;
    }
    if (static_cast<int>(b.t) == b.life) {
        if (b.p2 == 1) {
            b.angle = b.angle_init;
        } else {
            b.angle = AngleToMyShip(b, tg);
        }
        b.speed_cap = b.xacc_cap;
    }
}

// ===========================================================================
// E 周期瞄准 / 出屏转向
// ===========================================================================

// bulletFunc11 @0x50d6e4 —— 出屏后重新瞄准。[实测]
void Func11(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (b.x > kHalfW || b.x < 0.0 || b.y > kHalfH) {
        if (b.p1 == 2) {
            b.angle = AngleToEnemy(b, tg);
        } else if (b.p1 == 1) {
            b.angle = AngleToMyShip(b, tg);
        }
    }
}

// bulletFunc12 @0x50d7a8 —— 出屏（含 y<40）后重新瞄准。[实测]
void Func12(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (b.x > kHalfW || b.x < 0.0 || b.y > kHalfH || b.y < kTop) {
        if (b.p1 == 2) {
            b.angle = AngleToEnemy(b, tg);
        } else if (b.p1 == 1) {
            b.angle = AngleToMyShip(b, tg);
        }
    }
}

// bulletFunc13 @0x50d860 —— 每帧朝自机瞄准；P1←0x168。[推断：最近点搜索细节]
void Func13(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    b.angle = AngleToMyShip(b, tg);
    b.p1 = 0x168;
}

// bulletFunc14 @0x50d950 —— pf1 随 life 线性变化；hitDis = pf2·pf1。[实测]
void Func14(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    constexpr double kK = 0.1;
    if (static_cast<int>(b.t) >= 1 && b.p2 != 0) {
        const double d = static_cast<double>(b.life) * kK / static_cast<double>(b.p1);
        b.pf1 = (b.pf1 + d) * kK;
    }
    if (b.p2 != 0) {
        if (b.life < b.p2) {
            const double n = b.pf1 * kK;
            if (n < b.pf1) {
                b.pf1 = n;
            }
        }
        if (b.life > b.p2) {
            const double n = b.pf1 * kK;
            if (n > b.pf1) {
                b.pf1 = n;
            }
        }
    }
    b.hit_dis = b.pf2 * b.pf1;
}

// bulletFunc70 @0x51079c —— 出屏 → 瞄准 + 随机角 + 换皮。[实测/推断]
void Func70(BulletMotion& b, const MotionTargets& tg, DeterministicRng& rng) {
    if (b.x > kHalfW || b.y < 0.0 || b.y > kHalfH) {
        const double d = kDeg180 - b.angle;
        b.angle = d + static_cast<double>(rng.RandomInt(0, b.p2));
        if (b.p1 <= 4) {
            b.angle = AngleToMyShip(b, tg);
        }
        b.speed = b.spd_init2;
        b.speed_cap = b.xacc_cap;
    }
}

// bulletFunc76 @0x510d30 —— y>480 → 瞄准 + 换皮。[实测]
void Func76(BulletMotion& b, const MotionTargets&, DeterministicRng& rng) {
    if (b.y > kHalfH) {
        b.angle = kDeg360 - b.angle;
        b.angle += static_cast<double>(rng.RandomInt(0, b.p2));
        b.speed = b.spd_init2;
        b.speed_cap = b.xacc_cap;
    }
}

// ===========================================================================
// F 边界反弹 / 镜像
// ===========================================================================

// bulletFunc1 @0x50ce78 —— 三段边界反弹。[实测]
void Func1(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    bool bounce = false;
    if (b.x > kHalfW || b.x < 0.0 || b.y > kHalfH) {
        bounce = true;
    }
    if (bounce) {
        b.angle = kDeg180 - b.angle;
        if (b.angle < 0.0) {
            b.angle += kDeg360;
        }
    }
    b.speed = b.spd_init2;
    b.speed_cap = b.xacc_cap;
}

// bulletFunc7 @0x50d3b8 —— 同 1，P1>=1 叠加随机角。[实测]
void Func7(BulletMotion& b, const MotionTargets&, DeterministicRng& rng) {
    bool bounce = false;
    if (b.x > kHalfW || b.x < 0.0 || b.y > kHalfH || b.y < kTop) {
        bounce = true;
    }
    if (bounce) {
        b.angle = kDeg180 - b.angle;
        if (b.angle < 0.0) {
            b.angle += kDeg360;
        }
        if (b.p1 >= 1) {
            b.angle += static_cast<double>(rng.RandomInt(0, 360));
        }
    }
}

// bulletFunc20 @0x50dde0 —— 边界分段（80/240/340/320 带）。[推断]
void Func20(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (b.y < kTop) {
        b.life = 1;
        b.angle = kDeg180 - b.angle;
        if (b.angle < 0.0) {
            b.angle += kDeg360;
        }
    } else if (b.y > kHalfH) {
        b.life = 2;
        b.angle = kDeg180 - b.angle;
        if (b.angle < 0.0) {
            b.angle += kDeg360;
        }
    } else if (b.x > kHalfW) {
        b.life = 3;
        b.angle += 90.0;
    } else if (b.x < 0.0) {
        b.life = 4;
        b.angle -= 90.0;
    }
}

// bulletFunc32 @0x50ea08 —— 按 P1 奇偶镜像角。[实测]
void Func32(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (static_cast<int>(b.t) != 0) {
        return;
    }
    const double a0 = b.angle_init;
    b.speed = b.spd_init2;
    b.speed_cap = b.xacc_cap;
    if ((b.p1 & 1) != 0) {
        b.angle = kDeg360 - (a0 - b.p2) + 90.0;
    } else {
        b.angle = a0 - b.p2;
    }
}

// bulletFunc38 @0x50edcc —— 条件单次反弹；a = A0 + a。[推断]
void Func38(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    bool bounce = false;
    if (b.y > kHalfH || b.y < kTop || b.x > kHalfW || b.x < 0.0) {
        bounce = true;
    }
    if (bounce) {
        b.angle = kDeg180 - b.angle;
        if (b.angle < 0.0) {
            b.angle += kDeg360;
        }
    }
    b.angle = b.angle_init + b.angle;
}

// bulletFunc57 @0x50fd84 —— 在 A0±PA 之间三角波。[实测]
void Func57(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (tg.global_state == 0x17) {
        b.angle += b.f598;
        if (b.angle > b.angle_init + b.prev_angle) {
            b.angle = b.angle_init + b.prev_angle;
        }
    } else {
        b.angle -= b.f598;
        if (b.angle < b.angle_init - b.prev_angle) {
            b.angle = b.angle_init - b.prev_angle;
        }
    }
}

// bulletFunc58 @0x50fe00 —— 折返角。[实测]
void Func58(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (static_cast<int>(b.t) != 0) {
        return;
    }
    if ((b.p1 & 1) != 0) {
        b.angle += b.f598;
    } else {
        b.angle -= b.f598;
        b.angle_acc = b.angle_acc - b.f598;
    }
    b.angle = kDeg360 - b.angle + 90.0;
}

// bulletFunc62 @0x510088 —— 边界反弹 + 随机角。[推断]
void Func62(BulletMotion& b, const MotionTargets&, DeterministicRng& rng) {
    if (b.x > kHalfW || b.x < 0.0 || b.y > kHalfH || b.y < kTop) {
        b.angle = kDeg180 - b.angle;
        if (b.angle < 0.0) {
            b.angle += kDeg360;
        }
        b.angle += static_cast<double>(rng.RandomInt(-0x50, 0x50));
    }
}

// bulletFunc67 @0x510544 —— 边界反弹（320/480/40）+ 换皮。[推断：忽略特效]
void Func67(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (b.x > kHalfW || b.y > kHalfH || b.y < kTop) {
        b.angle = kDeg180 - b.angle;
        if (b.angle < 0.0) {
            b.angle += kDeg360;
        }
    }
    b.speed_cap = b.xacc_cap;
}

// bulletFunc77 @0x510dcc —— 状态 0x21/0x22 → 镜像角。[实测]
void Func77(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (tg.global_state == 0x21 || tg.global_state == 0x22) {
        b.angle = kDeg180 - b.angle;
        if (b.angle < 0.0) {
            b.angle += kDeg360;
        }
    }
}

// bulletFunc78 @0x510e88 —— y<40 → 镜像角。[实测]
void Func78(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (b.y < kTop) {
        b.angle = kDeg180 - b.angle;
        if (b.angle < 0.0) {
            b.angle += kDeg360;
        }
    }
}

// ===========================================================================
// G 接近触发
// ===========================================================================

// bulletFunc15 @0x50da6c —— 接近自机 → 加速。[实测/推断阈值]
void Func15(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (b.p2 != 0) {
        return;
    }
    if (DistSqToMyShip(b, tg) <= static_cast<double>(b.life)) {
        b.p2 = 1;
        b.speed = b.spd_init2;
        b.speed_cap = b.xacc_cap;
    }
}

// bulletFunc16 @0x50daf8 —— 接近敌机 → 触发。[实测/推断阈值]
void Func16(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (b.p2 != 0) {
        return;
    }
    if (static_cast<int>(b.t) <= b.bullet_life) {
        return;
    }
    if (DistSqToEnemy(b, tg) < static_cast<double>(b.life)) {
        b.p2 = 1;
    }
}

// bulletFunc24 @0x50e100 —— 接近自机 → 重置角/上限。[推断阈值]
void Func24(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (DistSqToMyShip(b, tg) < b.xacc && b.p2 == 0) {
        b.angle = b.angle_init;
        b.p2 = 1;
        b.speed_cap = b.xacc_cap;
    }
}

// bulletFunc34 @0x50eaa4 —— 全局状态==7 → 悬停。[实测]
void Func34(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (tg.global_state == 7) {
        b.p2 = 1;
        b.speed = 0.0;
        b.hit_dis = 4.0;
    }
    if (b.p2 == 1) {
        if (b.p1 < b.life) {
            b.speed = 0.0;
        }
        if (b.p1 == b.life) {
            b.angle = b.angle_init;
        }
    }
}

// bulletFunc41 @0x50f0f8 —— 接近自机 → 变身/发射。[实测]
void Func41(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (b.p2 != 0) {
        return;
    }
    if (DistSqToMyShip(b, tg) > static_cast<double>(b.life)) {
        return;
    }
    if (b.p1 > 6) {
        return;
    }
    b.p2 = 1;
    b.angle = AngleToMyShip(b, tg);
    b.speed = b.spd_init2;
    b.speed_cap = b.xacc_cap;
    b.hit_dis = 7.0;
    b.pf2 = 3.0;
    b.rotate_flg = 0;
}

// bulletFunc42 @0x50f3a0 —— 接近敌机 → 变身。[实测]
void Func42(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (b.p2 != 0) {
        return;
    }
    if (DistSqToEnemy(b, tg) >= static_cast<double>(b.life)) {
        return;
    }
    b.p2 = 1;
    if (b.p1 == 1) {
        b.hit_dis = 2.0;
        b.rotate_flg = 2;
    }
    b.angle = b.angle_init;
    b.speed_cap = b.xacc_cap;
}

// bulletFunc44 @0x50f580 —— L==1 且 dist≤P1 → 重置。[实测]
void Func44(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (b.p2 == 0 && b.life == 1 && DistSqToMyShip(b, tg) <= static_cast<double>(b.p1)) {
        b.angle = b.angle_init;
        b.speed_cap = b.xacc_cap;
        b.p2 = 1;
    }
}

// bulletFunc45 @0x50f600 —— 触发重置。[推断：调用语义未知]
void Func45(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (b.p2 == 0) {
        b.speed = b.spd_init2;
        b.p2 = 1;
    }
}

// bulletFunc53 @0x50fa34 —— 两阶段接近触发。[推断：常量存疑]
void Func53(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (b.p1 == 0 && DistSqToMyShip(b, tg) <= static_cast<double>(b.life)) {
        b.p1 = 1;
    }
    if (b.p1 == 1 && b.p2 == b.life) {
        b.angle = b.angle_init;
        b.speed_cap = b.xacc_cap;
    }
}

// bulletFunc60 @0x50fea8 —— 接近敌机 → 反向角加速。[实测]
void Func60(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (b.p2 == 0 && static_cast<int>(b.t) > b.bullet_life &&
        DistSqToEnemy(b, tg) <= static_cast<double>(b.life)) {
        b.angle = b.angle_init;
        b.angle_acc = -b.f598;
        b.f56c = -b.prev_angle;
        b.p2 = 1;
    }
}

// bulletFunc74 @0x510c3c —— LP 状态机。[推断]
void Func74(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (static_cast<int>(b.t) == 0xb) {
        b.bullet_loop = 1;
    }
    if (DistSqToMyShip(b, tg) <= static_cast<double>(b.life)) {
        b.bullet_loop = 1;
    }
}

// bulletFunc80 @0x510f28 —— 距离落在 [XC-V1, XC+V1] → P2=1。[推断：分量比较近似]
void Func80(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (b.p2 != 0 || static_cast<int>(b.t) != 0 || b.life > 3) {
        return;
    }
    const double lo = b.xacc_cap - b.spd_init2;
    const double hi = b.xacc_cap + b.spd_init2;
    if (b.x > lo && b.x < hi) {
        b.p2 = 1;
    }
}

// bulletFunc84 @0x510ff4 —— 换皮 / 触发。[实测]
void Func84(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (b.p2 == 1) {
        int t = b.p1;
        if (t > 255) {
            t = 255;
        }
        if (t >= kBulletKindMin && t <= kBulletKindMax) {
            b.bullet_kind = t;
        }
        if (DistSqToMyShip(b, tg) <= b.spd_init2) {
            b.p2 = 1;
        }
    }
}

// bulletFunc85 @0x5110a4 —— 接近自机 → 换 hitDis。[实测]
void Func85(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (DistSqToMyShip(b, tg) < static_cast<double>(b.life)) {
        if (b.bullet_kind == 0x25) {
            b.hit_dis = 20.0;
        } else if (b.bullet_kind == 0x2a) {
            b.hit_dis = 4.0;
        }
    }
}

// ===========================================================================
// H 分裂/召唤（Boss）—— 需宿主回调生成子子弹，这里只做状态标记。
// ===========================================================================

// bulletFunc31 @0x50e498 —— Boss 分裂/召唤。[推断：子子弹生成交宿主]
void Func31(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (static_cast<int>(b.t) == b.p1 && b.life <= 0x1e) {
        b.rotate_flg = 2;
        b.p2 = 1;  // 标记「本帧请求分裂」
    }
}

// bulletFunc50 @0x50f800 —— Boss 蓄力/阶段。[推断：忽略音效/特效]
void Func50(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (b.life == 0 && (b.x > 280.0 || b.y < kTop || b.y > 440.0 || b.x < 80.0)) {
        b.life = 1;
        b.speed = 0.0;
        b.speed_cap = 0.0;
    } else if (b.life == 1) {
        b.p1 += 1;
    } else if (b.life == 2 && b.p2 == 0x1e) {
        b.angle = b.angle_init;
        b.speed_cap = b.xacc_cap;
    }
}

// bulletFunc55 @0x50fca0 —— 接近敌机 → 蓄力特效。[推断：事件仅]
void Func55(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (DistSqToEnemy(b, tg) <= static_cast<double>(b.life)) {
        b.p2 = 1;  // 请求蓄力特效（宿主处理）
    }
}

// bulletFunc83 @0x511fcc —— Boss 摆线。[推断：常量 160/260/30/90/40]
void Func83(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    const double n = std::min(60.0, b.t);
    const double a = 160.0 + 260.0 * SinDeg(30.0 * n + 90.0) + 40.0;
    b.angle = a;
}

// ===========================================================================
// I 换皮 / 半径
// ===========================================================================

// bulletFunc30 @0x50e224 —— 按 bulletType 换贴图 + hitDis。[实测]
void Func30(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (static_cast<int>(b.t) != b.p2) {
        return;
    }
    b.hit_dis = 4.0;
}

// bulletFunc54 @0x50faf4 —— 按 life 阶段换皮。[实测：贴图交宿主]
void Func54(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (static_cast<int>(b.t) != 0) {
        return;
    }
    b.speed = b.spd_init2;
    b.speed_cap = b.xacc_cap;
}

// bulletFunc65 @0x510498 —— 按 life/P2 换皮。[实测：贴图交宿主]
void Func65(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (static_cast<int>(b.t) != 0) {
        return;
    }
    b.speed = b.spd_init2;
    b.speed_cap = b.xacc_cap;
}

// ===========================================================================
// J 参数方程 / 摆线（推断：多路 cos/sin 叠加的精确系数未逐指令验证）
// ===========================================================================

// bulletFunc26 @0x511524 —— vx/vy = speed·(cos/sin)(angle + XC·sin(XC + X·L))。[推断]
void Func26(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    const double a = b.angle + b.xacc_cap * SinDeg(b.xacc_cap + b.xacc * static_cast<double>(b.life));
    b.angle = a;
}

// bulletFunc33 @0x51160c —— 双正弦参数轨迹。[推断]
void Func33(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    const double a = b.angle + b.f598 * b.t;
    const double px = b.speed * b.t * CosDeg(a);
    const double py = b.speed * b.t * SinDeg(a);
    b.xacc = px;
    b.xacc_cap = py;
    b.angle = AngleToPoint(0.0, 0.0, px, py);
}

// bulletFunc66 @0x511ab4 —— 参数方程。[推断]
void Func66(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    const double c = CosDeg(b.angle_init * static_cast<double>(b.p2) + b.f598);
    const double s = SinDeg(b.xacc * static_cast<double>(b.p2) + b.speed_cap);
    b.angle = kDeg360 - (c + s) + 90.0;
}

// bulletFunc75 @0x511c08 —— 双正弦参数轨迹。[推断]
void Func75(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    const double c = CosDeg(b.angle_init + b.f598 * static_cast<double>(b.p2));
    const double s = SinDeg(b.spd_init2 + b.xacc * static_cast<double>(b.p2));
    b.angle = kDeg360 - (c + s) + 90.0;
}

// bulletFunc82 @0x511e48 —— 敌机/自机参数方程。[推断]
void Func82(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    const double base = (b.p1 == 0) ? AngleToMyShip(b, tg) : AngleToEnemy(b, tg);
    const double a = base + b.f598 * SinDeg(b.spd_init2 + b.xacc * b.t);
    b.angle = a;
}

// ===========================================================================
// K 状态机（全局状态）
// ===========================================================================

// bulletFunc19 @0x50dd24 —— 全局条件成立 → v *= 0.6。[推断：魔数 0x66666667]
void Func19(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (b.p2 == 0 && tg.global_state >= 0) {
        b.speed *= 0.6;
        b.p2 = 1;
    }
}

// bulletFunc21 @0x50dfb8 —— 状态==3 → v = L·0.03。[实测]
void Func21(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (tg.global_state == 3) {
        b.speed = static_cast<double>(b.life) * 0.03;
    } else {
        b.speed = b.spd_init2;
    }
}

// bulletFunc22 @0x50e018 —— 状态==4 → v = L·0.1。[实测]
void Func22(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (tg.global_state == 4) {
        b.speed = static_cast<double>(b.life) * 0.1;
    } else {
        b.speed = b.spd_init2;
    }
}

// bulletFunc23 @0x50e078 —— 状态==5 → v = L·0.1（+换皮）。[实测]
void Func23(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (tg.global_state == 5) {
        b.speed = static_cast<double>(b.life) * 0.1;
    } else {
        b.speed = b.spd_init2;
    }
}

// bulletFunc51 @0x511874 —— 状态==0x15 位移。[推断]
void Func51(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (tg.global_state != 0x15) {
        return;
    }
    const double sign = (b.p1 == 0) ? 1.0 : -1.0;
    b.x += sign * b.speed * CosDeg(b.angle);
    b.y += sign * b.speed * SinDeg(b.angle);
}

// bulletFunc71 @0x510ac8 —— 状态==0x1e 随机化初角/幅度。[实测]
void Func71(BulletMotion& b, const MotionTargets& tg, DeterministicRng& rng) {
    if (tg.global_state != 0x1e || static_cast<int>(b.t) != 0) {
        return;
    }
    b.angle_init = static_cast<double>(rng.RandomInt(0, b.p1));
    b.f598 = static_cast<double>(rng.RandomInt(0, b.p1) + rng.RandomInt(0, b.p1));
}

// bulletFunc72 @0x510b78 —— 状态==0x1c → 速度重置。[实测]
void Func72(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (tg.global_state == 0x1c) {
        b.speed = b.spd_init2;
        b.speed_cap = b.xacc_cap;
    }
}

// ===========================================================================
// L 辅助（无字段写入）
// ===========================================================================

// bulletFunc3/6/8/28/36/79 —— 边界/着色/取值辅助，无运动字段写入。[实测]
void FuncNoop(BulletMotion&, const MotionTargets&, DeterministicRng&) {}

// bulletFunc27 @0x50cd54 —— P1==1 → hitHantei=0；否则时间缩放。[推断]
void Func27(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (b.p1 == 1) {
        b.hit_hantei = false;
    }
}

// bulletFunc46 @0x50f654 —— 每 N 帧把角 ±90。[实测]
void Func46(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (static_cast<int>(b.t) == 0) {
        return;
    }
    if (static_cast<int>(b.t) == b.bullet_life) {
        b.angle += (b.life == 0) ? 90.0 : -90.0;
        b.bullet_life = static_cast<int>(b.t);
    }
}

// bulletFunc52 @0x50f9e4 —— 随机角。[实测]
void Func52(BulletMotion& b, const MotionTargets&, DeterministicRng& rng) {
    if (static_cast<int>(b.t) != 0) {
        return;
    }
    b.angle = static_cast<double>(rng.RandomInt(0, b.p2));
}

// bulletFunc61 @0x50ff78 —— 四方向（0/90/180/270）。[实测]
void Func61(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (static_cast<int>(b.t) != 0) {
        return;
    }
    const double dx = tg.has_my_ship ? (b.x - tg.my_ship_x) : 0.0;
    const double dy = tg.has_my_ship ? (b.y - tg.my_ship_y) : 0.0;
    if (dx < 0.0 && dy < 0.0) {
        b.angle = 90.0;
    } else if (dx < 0.0 && dy >= 0.0) {
        b.angle = 180.0;
    } else if (dx >= 0.0 && dy >= 0.0) {
        b.angle = 270.0;
    } else {
        b.angle = 0.0;
    }
    b.speed = b.spd_init2;
    b.speed_cap = b.xacc_cap;
}

// bulletFunc63 @0x510394 —— a=A0; aa=-F98。[实测]
void Func63(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (static_cast<int>(b.t) == 0) {
        b.angle = b.angle_init;
        b.angle_acc = -b.f598;
    }
}

// bulletFunc64 @0x5103d0 —— 分阶段。[实测]
void Func64(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (b.life == 0) {
        b.angle = b.angle_init;
        b.speed = b.spd_init2;
    } else if (b.life == 1) {
        b.angle = AngleToMyShip(b, tg);
        b.speed = b.spd_init2;
        b.speed_cap = b.xacc_cap;
    }
}

// bulletFunc37 @0x51172c —— 正弦角 a = 75·sin(L + X·T − 90) + 180。[推断]
void Func37(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    b.angle = 75.0 * SinDeg(static_cast<double>(b.life) + b.xacc * b.t - 90.0) + 180.0;
}

}  // namespace

// ---------------------------------------------------------------------------
// 公共 API
// ---------------------------------------------------------------------------

DeterministicRng::DeterministicRng(
    std::uint64_t seed,
    std::uint64_t tick,
    std::uint32_t pattern_index,
    std::uint32_t spawn_index
) : seed_(seed), tick_(tick), pattern_index_(pattern_index), spawn_index_(spawn_index) {}

std::uint32_t DeterministicRng::NextU32() {
    // FNV-1a 风格混合 + Mix32 雪崩；输入含自增 counter_ 以区分同子弹的多次调用。
    std::uint32_t h = 2166136261u;
    auto mix_in = [&h](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= static_cast<std::uint32_t>(v & 0xffu);
            h *= 16777619u;
            v >>= 8;
        }
    };
    mix_in(seed_);
    mix_in(tick_);
    mix_in(pattern_index_);
    mix_in(spawn_index_);
    mix_in(counter_++);
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

int DeterministicRng::RandomInt(int a, int b) {
    if (b < a) {
        const int t = a;
        a = b;
        b = t;
    }
    const std::uint32_t span = static_cast<std::uint32_t>(b - a) + 1u;
    return a + static_cast<int>(NextU32() % span);
}

double AngleToPoint(double x, double y, double tx, double ty) {
    const double radians = std::atan2(ty - y, tx - x);
    return NormalizeDeg(radians * (180.0 / kPi));
}

double NormalizeDeg(double degrees) {
    degrees = std::fmod(degrees, 360.0);
    if (degrees < 0.0) {
        degrees += 360.0;
    }
    return degrees;
}

bool IsKnownBulletFunc(int func_no) {
    switch (func_no) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8:
        case 11: case 12: case 13: case 14: case 15: case 16: case 17: case 18:
        case 19: case 20: case 21: case 22: case 23: case 24: case 25: case 26:
        case 27: case 28: case 29: case 30: case 31: case 32: case 33: case 34:
        case 35: case 36: case 37: case 38: case 39: case 40: case 41: case 42:
        case 43: case 44: case 45: case 46: case 47: case 48: case 50: case 51:
        case 52: case 53: case 54: case 55: case 57: case 58: case 60: case 61:
        case 62: case 63: case 64: case 65: case 66: case 67: case 70: case 71:
        case 72: case 73: case 74: case 75: case 76: case 77: case 78: case 79:
        case 80: case 81: case 82: case 83: case 84: case 85:
            return true;
        default:
            return false;
    }
}

bool ApplyBulletFunc(BulletMotion& b, const MotionTargets& targets, DeterministicRng& rng) {
    switch (b.bullet_func_no) {
        case 0:  Func0(b, targets, rng); break;
        case 1:  Func1(b, targets, rng); break;
        case 2:  Func2(b, targets, rng); break;
        case 3:  FuncNoop(b, targets, rng); break;
        case 4:  Func4(b, targets, rng); break;
        case 5:  Func5(b, targets, rng); break;
        case 6:  FuncNoop(b, targets, rng); break;
        case 7:  Func7(b, targets, rng); break;
        case 8:  FuncNoop(b, targets, rng); break;
        case 11: Func11(b, targets, rng); break;
        case 12: Func12(b, targets, rng); break;
        case 13: Func13(b, targets, rng); break;
        case 14: Func14(b, targets, rng); break;
        case 15: Func15(b, targets, rng); break;
        case 16: Func16(b, targets, rng); break;
        case 17: Func17(b, targets, rng); break;
        case 18: Func18(b, targets, rng); break;
        case 19: Func19(b, targets, rng); break;
        case 20: Func20(b, targets, rng); break;
        case 21: Func21(b, targets, rng); break;
        case 22: Func22(b, targets, rng); break;
        case 23: Func23(b, targets, rng); break;
        case 24: Func24(b, targets, rng); break;
        case 25: Func25(b, targets, rng); break;
        case 26: Func26(b, targets, rng); break;
        case 27: Func27(b, targets, rng); break;
        case 28: FuncNoop(b, targets, rng); break;
        case 29: Func29(b, targets, rng); break;
        case 30: Func30(b, targets, rng); break;
        case 31: Func31(b, targets, rng); break;
        case 32: Func32(b, targets, rng); break;
        case 33: Func33(b, targets, rng); break;
        case 34: Func34(b, targets, rng); break;
        case 35: Func35(b, targets, rng); break;
        case 36: FuncNoop(b, targets, rng); break;
        case 37: Func37(b, targets, rng); break;
        case 38: Func38(b, targets, rng); break;
        case 39: Func39(b, targets, rng); break;
        case 40: Func40(b, targets, rng); break;
        case 41: Func41(b, targets, rng); break;
        case 42: Func42(b, targets, rng); break;
        case 43: Func43(b, targets, rng); break;
        case 44: Func44(b, targets, rng); break;
        case 45: Func45(b, targets, rng); break;
        case 46: Func46(b, targets, rng); break;
        case 47: Func47(b, targets, rng); break;
        case 48: Func48(b, targets, rng); break;
        case 50: Func50(b, targets, rng); break;
        case 51: Func51(b, targets, rng); break;
        case 52: Func52(b, targets, rng); break;
        case 53: Func53(b, targets, rng); break;
        case 54: Func54(b, targets, rng); break;
        case 55: Func55(b, targets, rng); break;
        case 57: Func57(b, targets, rng); break;
        case 58: Func58(b, targets, rng); break;
        case 60: Func60(b, targets, rng); break;
        case 61: Func61(b, targets, rng); break;
        case 62: Func62(b, targets, rng); break;
        case 63: Func63(b, targets, rng); break;
        case 64: Func64(b, targets, rng); break;
        case 65: Func65(b, targets, rng); break;
        case 66: Func66(b, targets, rng); break;
        case 67: Func67(b, targets, rng); break;
        case 70: Func70(b, targets, rng); break;
        case 71: Func71(b, targets, rng); break;
        case 72: Func72(b, targets, rng); break;
        case 73: Func73(b, targets, rng); break;
        case 74: Func74(b, targets, rng); break;
        case 75: Func75(b, targets, rng); break;
        case 76: Func76(b, targets, rng); break;
        case 77: Func77(b, targets, rng); break;
        case 78: Func78(b, targets, rng); break;
        case 79: FuncNoop(b, targets, rng); break;
        case 80: Func80(b, targets, rng); break;
        case 81: Func81(b, targets, rng); break;
        case 82: Func82(b, targets, rng); break;
        case 83: Func83(b, targets, rng); break;
        case 84: Func84(b, targets, rng); break;
        case 85: Func85(b, targets, rng); break;
        default: return false;
    }
    return true;
}

void UpdateBulletMotion(BulletMotion& b, const MotionTargets& targets, DeterministicRng& rng) {
    // 1) delay 倒计时（>0 则本帧不运动）。
    if (b.delay > 0) {
        b.delay--;
        return;
    }
    // 2) 若 bulletLife != 0：angle += angleAcc，并按 f56c 夹持。
    if (b.bullet_life != 0) {
        b.angle += b.angle_acc;
        if (b.f56c > 0.0) {
            const double lim = b.f56c + b.angle_spd;
            if (b.angle > lim) {
                b.angle = lim;
            }
        }
        if (b.f56c < 0.0) {
            const double lim = b.f56c + b.angle_spd;
            if (b.angle < lim) {
                b.angle = lim;
            }
        }
    }
    // 3) 依 bulletFuncNo 分派。
    ApplyBulletFunc(b, targets, rng);
    // 4) 默认尾段：vx = speed·cos(angle); vy = speed·sin(angle)。
    b.IntegrateVelocity();
}

}  // namespace phk::battle::danmaku
