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

// 原版 MyMathUtil::sin 走的是**单精度**：sinf(deg * (π/180))。
// func36 的 alpha 要按 1/128 取整，double 与 float 的 1ULP 差异正好会跨过截断边界
// （基准里实测到 opacity 差 1），所以这里显式用 float 复刻原版。
inline float SinDegF(float deg) {
    constexpr float kPiF = 3.14159265358979323846f;
    return std::sin(deg * (kPiF / 180.0f));
}
inline float CosDegF(float deg) {
    constexpr float kPiF = 3.14159265358979323846f;
    return std::cos(deg * (kPiF / 180.0f));
}

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
    b.life -= 1;      // 0x50d244: 末尾无条件 life--
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
    if (a > 0.0) {
        if (b.speed < b.xacc_cap) {
            b.speed += a;
            if (b.speed > b.xacc_cap) {
                b.speed = b.xacc_cap;
            }
        }
    } else if (a < 0.0) {
        if (b.speed > b.xacc_cap) {
            b.speed += a;
            if (b.speed < b.xacc_cap) {
                b.speed = b.xacc_cap;
            }
        }
    }
    b.life -= 1;      // 0x50d2d8: 无条件（含 a==0 的早退路径）
}

// bulletFunc35 @0x50ec24 —— 接近自机 → 指数减速 + 朝自机。[实测/推断阈值]
void Func35(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    // 0x50ec24: p2==0 → 距离平方 <= life 则 p2=1（不加 1，直接置位）；
    //           p2==1 → vc=0、spdAcc = speed×(-0.1)、p2=2；
    //           p2==p1（且 >=2）→ v=V1、spdAcc=X、vc=XC，若 A0>1 再瞄准；p2++
    const int p2 = b.p2;
    if (p2 == 0) {
        if (DistSqToMyShip(b, tg) <= static_cast<double>(b.life)) {
            b.p2 = 1;
        }
        return;
    }
    if (p2 == 1) {
        b.speed_cap = 0.0;
        b.accel = b.speed * (-0.1);
        b.p2 = 2;
        return;
    }
    if (p2 == b.p1) {
        b.speed = b.spd_init2;
        b.accel = b.xacc;
        b.speed_cap = b.xacc_cap;
        if (b.angle_init > 1.0) {
            b.angle = AngleToMyShip(b, tg);
        }
    }
    b.p2 = p2 + 1;
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
    // 0x50f500: 若 p2>=life：v = V1 + X*sin(XC*(p2-life))；setScale(v)；hitDis = pf2*v
    //           末尾无条件 p2++
    if (b.p2 >= b.life) {
        const double v = b.spd_init2 + b.xacc *
            SinDeg(b.xacc_cap * static_cast<double>(b.p2 - b.life));
        b.scale = v;
        b.hit_dis = static_cast<float>(b.pf2 * v);
    }
    b.p2 += 1;
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
    b.scale = b.pf1;              // 0x510c00: 随后 setScale(pf1)
    b.hit_dis = b.pf2 * b.pf1;
}

// bulletFunc81 @0x511d48 —— pf1 = A0 + F98·sin(PA·P2)；v = V1 + XC·s。[实测]
void Func81(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    // 0x511d48: pf1 = A0 + F98·sin(PA·p2)；位置 (V1 + XC·p2·cos(pf1), X + XC·p2·sin(pf1))
    //   是**绝对值**，直接 setPosition（vtable+0x98）覆盖当前位置；不写 speed/hitDis。
    //   末尾 setRotation(270-pf1)、p2++。
    const double p2 = static_cast<double>(b.p2);
    b.pf1 = b.angle_init + b.f598 * SinDeg(b.prev_angle * p2);
    const double r = b.xacc_cap * p2;
    b.x = b.spd_init2 + r * CosDeg(b.pf1);
    b.y = b.xacc + r * SinDeg(b.pf1);
    b.p2 += 1;
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
    b.life -= 1;      // 0x50d234 之后的公共尾段
}

// bulletFunc17 @0x50db94 —— 限速追踪（单帧 ≤ A0）。[实测]
void Func17(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    const double target = AngleToMyShip(b, tg);
    if (static_cast<int>(b.t) == b.life) {
        b.prev_angle = target;
        return;
    }
    if (static_cast<int>(b.t) > b.life) {
        // 0x50dc10: d = aim - prevAngle；d > lim → prev+lim；否则 **d < 0** → prev-lim
        // （第二个分支的门是 d<0，不是 d<-lim）
        double t = target;
        const double d = t - b.prev_angle;
        if (d > b.angle_init) {
            t = b.prev_angle + b.angle_init;
        } else if (d < 0.0) {
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
    // 0x50d950: t1==0 → pf1 = p1*0.1；否则（p2!=0）pf1 += (p2-life)*0.1/p1（double 链）
    //           随后把 pf1 双向夹到 p2*0.1；setScale(pf1)；hitDis = pf2*pf1
    const int t = static_cast<int>(b.t);
    if (t == 0) {
        b.pf1 = static_cast<float>(static_cast<double>(b.life) * 0.1);   // 0x50d9b0 用 life 不是 p1
    } else if (t >= 1 && b.p2 != 0) {
        b.pf1 = static_cast<float>(static_cast<double>(b.pf1) +
               (static_cast<double>(b.p2 - b.life) * 0.1) / static_cast<double>(b.p1));
    }
    if (b.p2 != 0) {
        const double target = static_cast<double>(b.p2) * 0.1;
        if (b.life < b.p2 && target < b.pf1) {
            b.pf1 = static_cast<float>(target);
        }
        if (b.life > b.p2 && target > b.pf1) {
            b.pf1 = static_cast<float>(target);
        }
    }
    b.scale = b.pf1;                                  // 0x50da48: setScale(pf1)
    b.hit_dis = static_cast<float>(b.pf2 * b.pf1);
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
    // 0x50ea08: 节拍门 t1 % life == 0；v=V1, spdAcc=X, vc=XC；
    //           angle = (p1&1) ? A0+p2 : p2-A0；随后 setRotation(270-angle)；p1++
    const int t = static_cast<int>(b.t);
    if (t < 0) {
        return;
    }
    const int rem = (b.life != 0) ? (t % b.life) : t;
    if (rem != 0) {
        return;
    }
    const double s = static_cast<double>(b.p2);
    const double a0 = b.angle_init;
    b.speed = b.spd_init2;
    b.accel = b.xacc;
    b.speed_cap = b.xacc_cap;
    b.angle = (b.p1 & 1) ? (a0 + s) : (s - a0);
    b.p1 += 1;
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
    // 0x50e100: 十字带触发 —— |dx|² < life² 或 |dy|² < life²（不是距离平方）
    //           p2==0 时 a=A0、{angleAcc,f56c}={f598,prevAngle}、vc=XC、p2=1
    const double lim = static_cast<double>(b.life) * static_cast<double>(b.life);
    const double dx = b.x - tg.my_ship_x;
    const double dy = b.y - tg.my_ship_y;
    const bool hit = (dx * dx < lim) || (dy * dy < lim);
    if (!hit || b.p2 != 0) {
        return;
    }
    b.angle = b.angle_init;
    b.angle_acc = b.f598;
    b.f56c = b.prev_angle;
    b.speed = b.spd_init2;      // 0x50e160 段：同时把 V1 写回 speed
    b.speed_cap = b.xacc_cap;
    b.p2 = 1;
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
void Func44(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (b.p2 != 0) {
        return;
    }
    if (b.life != 1) {
        return;
    }
    // 原版比较的是 getPosition().y < p1（不是与自机的距离）
    if (!(b.y < static_cast<double>(b.p1))) {
        return;
    }
    b.angle = b.angle_init;
    // 一条 16B 拷贝：{+0x598 f598, +0x59c prevAngle, +0x5a0 spdInit2, +0x5a4 xacc}
    //              → {+0x568 angleAcc, +0x56c f56c, +0x570 speed, +0x574 spdAcc}
    b.angle_acc = b.f598;
    b.f56c = b.prev_angle;
    b.speed = b.spd_init2;
    b.accel = b.xacc;
    b.speed_cap = b.xacc_cap;
    b.p2 += 1;
}

// bulletFunc45 @0x50f600 —— 首帧 setOpacity(life)+setScale(V1)。[实测]
void Func45(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (b.p2 != 0) {
        return;
    }
    // setOpacity(life) + Sprite::setScale(spdInit2)（vtable+0x490 / +0x80）
    b.opacity = b.life;
    b.scale = b.spd_init2;
    b.p2 = 1;
}

// bulletFunc53 @0x50fa34 —— 两阶段接近触发。[推断：常量存疑]
void Func53(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    // 0x50fa34: p1==0 → 若 pos.y < ship.y 则 p1=1 并**直接返回**（不碰 p2）；
    //           p1==1 后：p2==0 → 换皮 210 / spdAcc=-0.1 / vc=0；
    //                     p2==life → a=A0，16B 拷贝 {f598,prevAngle,V1,X} → {angleAcc,f56c,speed,spdAcc}，vc=XC；
    //           最后**无论走哪支都要 p2++**（0x50fae0 是公共尾）。
    if (b.p1 == 0) {
        if (!(b.y < tg.my_ship_y)) {
            return;
        }
        b.p1 = 1;
        return;
    }
    if (b.p1 != 1) {
        return;
    }
    if (b.p2 == 0) {
        b.bullet_kind = 210;          // 常量 @0xb09fc0: {210, 1}
        b.accel = -0.1;               // 常量 @0xb09fc8: {-0.1f, 0}
        b.speed_cap = 0.0;
    } else if (b.p2 == b.life) {
        b.angle = b.angle_init;
        b.angle_acc = b.f598;
        b.f56c = b.prev_angle;
        b.speed = b.spd_init2;
        b.accel = b.xacc;
        b.speed_cap = b.xacc_cap;
    }
    b.p2 += 1;
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
    // 0x510c64: LP==0 && t1==11 → setOpacity(life)
    if (b.bullet_loop == 0 && static_cast<int>(b.t) == 11) {
        b.opacity = b.life;
    }
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
void Func84(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    // 0x510ff4: p2==0 → setOpacity(0)，且与 (spdInit2, xacc) 的平方距离 > life 时 p2=1；
    //           p2==1 → setOpacity(min(p1<<4, 255)); p1++
    if (b.p2 == 0) {
        b.opacity = 0;
        const double dx = b.x - b.spd_init2;
        const double dy = b.y - b.xacc;
        if (dx * dx + dy * dy > static_cast<double>(b.life)) {
            b.p2 = 1;
        }
    } else if (b.p2 == 1) {
        int a = b.p1 << 4;
        if (a >= 255) {
            a = 255;
        }
        b.opacity = a;
        b.p1 += 1;
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
    // 0x50faf4: 节拍门 t1 % p1；然后按 life 分档换贴图：
    //   life==0 → p2:0→0x94, 1→0xcc(+setColor)，其它不变；
    //   life==1 → 类型取 p2%7 的字节表 @0xb0a9c7（index0 → 0xa3）；
    //   life==2 → p2:0→0x4a, 1→0x26, 2→0x95，且这三支才写 v=V1、vc=XC；
    //   末尾统一 p2++。
    const int t = static_cast<int>(b.t);
    const int rem = (b.p1 != 0) ? (t % b.p1) : t;
    if (rem != 0) {
        return;
    }
    if (b.life == 0) {
        if (b.p2 == 0) {
            b.bullet_kind = 0x94;
        } else if (b.p2 == 1) {
            b.bullet_kind = 0xcc;
        }
    } else if (b.life == 1) {
        static constexpr int kTypeByP2Mod7[7] = {0xa3, 0xa2, 0xa1, 0xa0, 0x9f, 0x9e, 0x9d};
        b.bullet_kind = kTypeByP2Mod7[((b.p2 % 7) + 7) % 7];
    } else if (b.life == 2) {
        if (b.p2 == 0) {
            b.bullet_kind = 0x4a;
        } else if (b.p2 == 1) {
            b.bullet_kind = 0x26;
        } else if (b.p2 == 2) {
            b.bullet_kind = 0x95;
        } else {
            b.p2 += 1;
            return;
        }
        b.speed = b.spd_init2;
        b.speed_cap = b.xacc_cap;
    }
    b.p2 += 1;
}

// bulletFunc65 @0x510498 —— 按 life/P2 换皮。[实测：贴图交宿主]
void Func65(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    // 0x510498: 节拍门 t1 % p1 == 0；然后按 life(0/1) 换贴图并重置速度，末尾 life--
    const int t = static_cast<int>(b.t);
    if (t < 0) {
        return;
    }
    const int rem = (b.p1 != 0) ? (t % b.p1) : t;
    if (rem != 0) {
        return;
    }
    if (b.life == 0) {
        if (b.p2 == 0) {
            b.bullet_kind = 0x37;
        } else if (b.p2 == 1) {
            b.bullet_kind = 0x3c;
        }
        b.speed = b.xacc;
    } else if (b.life == 1) {
        if (b.p2 == 0) {
            b.bullet_kind = 0x2e;
        } else if (b.p2 == 1) {
            b.bullet_kind = 0x33;
        }
        b.speed = b.spd_init2;
    }
    b.life -= 1;
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
// L 辅助（无 angle/speed 写入，但会改位置 / 透明度）
// ===========================================================================

// bulletFunc3 @0x51130c —— vx/vy += 0.0163333·(cos life, sin life)，角度制。[实测]
// ★ 分派真值：update 的跳转表 @0xb0a88e（项 = uint16×4 + 基址 0x50b514）里
//   funcNo=3 走共用出口 0x50c618，且全库无 `bl bulletFunc3` 调用点 ⇒ 该函数
//   不参与逐帧更新（其符号是独立导出的死代码）。生产侧保持 no-op 是对的。
void FuncNoop(BulletMotion&, const MotionTargets&, DeterministicRng&) {}

// bulletFunc3 @0x51130c —— vx/vy 叠加 0.0163333·(cos life, sin life)，角度制。[实测]
// ★ 内联在 update 的比较链里：0x50c9b8 装 double 常量 0x3F90B9AF72015D87，
//   0x50c9c4 写回 +0x57c/+0x580，故它逐帧执行（不是死代码）。
//   注意：本工程 update 末尾统一 integrate_velocity() 会用 angle/speed 覆盖 vx/vy，
//   因此该叠加对可观测轨迹无贡献——与既有 52 个函数的处理口径一致。
void Func3(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    constexpr double kDrift = 0x1.0B9AF72015D87p-6;   // 0.01633334…
    const double c = static_cast<double>(CosDegF(static_cast<float>(b.life)));
    const double s = static_cast<double>(SinDegF(static_cast<float>(b.life)));
    b.vx += static_cast<float>(kDrift * c);
    b.vy += static_cast<float>(kDrift * s);
}

// bulletFunc6 @0x50d2e8 —— X 轴环绕：越界换边并 life--。[实测]
//   原版用 getContentSize().width 做边界，这里约定 content = 2·half_w。
void Func6(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (b.life < 1) {
        return;
    }
    const double w = b.half_w * 2.0;
    if (b.x > b.half_w + w) {
        b.x = -w;
        --b.life;
    }
    if (b.x < -w) {
        b.x = b.half_w + w;
        --b.life;
    }
}

// bulletFunc8 @0x50d574 —— X/Y 双轴环绕（Y 下界 40-h）。[实测]
void Func8(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (b.life < 1) {
        return;
    }
    const double w = b.half_w * 2.0;
    const double h = b.half_h * 2.0;
    if (b.x > b.half_w + w) {
        b.x = -w;
        --b.life;
    }
    if (b.x < -w) {
        b.x = b.half_w + w;
        --b.life;
    }
    if (b.y > b.half_h + h) {
        b.y = kTop - h;
        --b.life;
    }
    if (b.y < kTop - h) {
        b.y = b.half_h + h;
        --b.life;
    }
}

// bulletFunc28 @0x50e1e8 —— alpha = max(255 - p2·(t1-p1), life)，淡出。[实测]
void Func28(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    const int d = static_cast<int>(b.t) - b.p1;
    int a = 0;
    if (d >= 0) {
        a = 255 - b.p2 * d;
        if (a < b.life) {
            a = b.life;
        }
    }
    b.opacity = a;
}

// bulletFunc79 @0x510ef0 —— alpha = min(p2·(t1-p1), life)，淡入。[实测]
void Func79(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    const int d = static_cast<int>(b.t) - b.p1;
    int a = 0;
    if (d >= 0) {
        a = b.p2 * d;
        if (a > b.life) {
            a = b.life;
        }
    }
    b.opacity = a;
}

// bulletFunc36 @0x50ed50 —— alpha = 128 + 128·sin(p1·(t1-life)+90)，[实测]
//   并把 hitHantei(0x548) 设为 alpha>127（暗到看不见时关掉碰撞）。
void Func36(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    const int d = static_cast<int>(b.t) - b.life;
    if (d < 0) {
        b.opacity = 255;
        return;
    }
    const int s = static_cast<int>(SinDegF(static_cast<float>(b.p1 * d + 90)) * 128.0f);
    int a = s + 128;
    if (a >= 255) {
        a = 255;
    }
    const int c = (a < 0) ? 0 : a;
    b.hit_hantei = (c > 127);
    b.opacity = (c < 128) ? c : a;
}

// bulletFunc27 @0x50cd54 —— P1==1 → hitHantei=0；否则 hitDis *= life*0.01。[实测]
void Func27(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (b.p1 == 1) {
        b.hit_hantei = false;
        return;
    }
    // getHitDis()->setHitDis(life*0.01*hitDis)，中间量走 double（fcvt/fmul 序列）
    b.hit_dis = static_cast<float>(static_cast<double>(b.life) * 0.01 * b.hit_dis);
}

// bulletFunc46 @0x50f654 —— 每 N 帧把角 ±90。[实测]
void Func46(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    // 0x50f654: t1==0 时先把 bulletLife 置为 p1 再比；命中则
    //           angle += (life==0 ? +90 : -90)；LP++；bulletLife = p2*LP + p1 + t1
    const int t = static_cast<int>(b.t);
    if (t == 0) {
        b.bullet_life = b.p1;
        if (t != b.p1) {
            return;
        }
    } else if (t != b.bullet_life) {
        return;
    }
    b.angle += (b.life == 0) ? -90.0 : 90.0;   // 表 @0xacee48: [0]=+90（life!=0）,[1]=-90
    b.bullet_loop += 1;
    b.bullet_life = b.p2 * b.bullet_loop + (b.p1 + t);
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
    // 0x50ff78: 门 = (t1-life) >= 0 且 (t1-life)%p1 == 0；
    //   p2 偶 → X 轴：x<ship.x ? 0° : 180°；p2 奇 → Y 轴：y<ship.y ? 90° : 270°；
    //   随后 v=V1、vc=XC、p2++（setRotation 只影响贴图朝向，不参与运动）
    const int d = static_cast<int>(b.t) - b.life;
    if (d < 0) {
        return;
    }
    const int rem = (b.p1 != 0) ? (d % b.p1) : d;
    if (rem != 0) {
        return;
    }
    if ((b.p2 & 1) == 0) {
        b.angle = (b.x < tg.my_ship_x) ? 0.0 : 180.0;
    } else {
        b.angle = (b.y < tg.my_ship_y) ? 90.0 : 270.0;
    }
    b.speed = b.spd_init2;
    b.speed_cap = b.xacc_cap;
    b.p2 += 1;
}

// bulletFunc63 @0x510394 —— a=A0; aa=-F98。[实测]
void Func63(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    // 0x510394: if (t1-life < 0) return; if ((t1-life) % p1 != 0) return;
    //            angle=A0; angleAcc=-f598; f56c=-prevAngle（16B 取反拷贝）
    const int d = static_cast<int>(b.t) - b.life;
    if (d < 0) {
        return;
    }
    const int rem = (b.p1 != 0) ? (d % b.p1) : d;   // ARM sdiv 0 → 0
    if (rem != 0) {
        return;
    }
    b.angle = b.angle_init;
    b.angle_acc = -b.f598;
    b.f56c = -b.prev_angle;
}

// bulletFunc64 @0x5103d0 —— 分阶段。[实测]
void Func64(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    // 0x5103d0: 先过 t1 % p1 == 0 的节拍门；再按 life(0/1) 分支；末尾统一 life--
    const int t = static_cast<int>(b.t);
    const int rem = (b.p1 != 0) ? (t % b.p1) : t;
    if (rem != 0) {
        return;
    }
    if (b.life == 0) {
        b.angle = b.angle_init;
        b.speed = b.spd_init2;
        b.accel = 0.0;
        b.speed_cap = 0.0;
    } else if (b.life == 1) {
        b.angle = AngleToMyShip(b, tg);
        b.speed = b.spd_init2;
        b.accel = b.xacc;
        b.speed_cap = b.xacc_cap;
    }
    b.life -= 1;
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
        case 3:  Func3(b, targets, rng); break;
        case 4:  Func4(b, targets, rng); break;
        case 5:  Func5(b, targets, rng); break;
        case 6:  Func6(b, targets, rng); break;
        case 7:  Func7(b, targets, rng); break;
        case 8:  Func8(b, targets, rng); break;
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
        case 28: Func28(b, targets, rng); break;
        case 29: Func29(b, targets, rng); break;
        case 30: Func30(b, targets, rng); break;
        case 31: Func31(b, targets, rng); break;
        case 32: Func32(b, targets, rng); break;
        case 33: Func33(b, targets, rng); break;
        case 34: Func34(b, targets, rng); break;
        case 35: Func35(b, targets, rng); break;
        case 36: Func36(b, targets, rng); break;
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
        case 79: Func79(b, targets, rng); break;
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
