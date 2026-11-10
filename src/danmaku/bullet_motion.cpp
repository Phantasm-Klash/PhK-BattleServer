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
    // 原版 MyMathUtil::sin 是 sinf(deg * π / 180)：先乘 π 再除 180，两步都是 float。
    // 写成 deg*(π/180) 会差 1ULP，在 func36 的 ×128 截断处放大成 opacity 差 1。
    constexpr float kPiF = 3.14159265358979323846f;
    return std::sin(deg * kPiF / 180.0f);
}
inline float CosDegF(float deg) {
    constexpr float kPiF = 3.14159265358979323846f;
    return std::cos(deg * kPiF / 180.0f);
}

// 原版边界反弹的常见指令序列：
//   fsub s0, sC, sAngle; fadd s1, s0, s360; fcmp s0,#0; fcsel s0,s1,s0,mi
// 即 d = C - angle；d<0 时 d += 360。x 边界 C=180(0xace194)，y 边界 C=360(0xace178)。
inline double MirrorAngle(double angle, double c) {
    double d = c - angle;
    if (d < 0.0) {
        d += kDeg360;
    }
    return d;
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

// bulletFunc0 @0x50cdcc —— **不是逐帧路径**。
// funcNo=0 在 update 跳转表里 index=-1（越界），直接落共用出口 0x50c618，
// 全库没有 `bl bulletFunc0@plt`；独立符号是 spawn 时用的初始化函数。
// 整帧实测：非零参数下 angle/speed 保持前处理后的值，不被引擎改写。
void Func0(BulletMotion&, const MotionTargets&, DeterministicRng&) {
}

// bulletFunc4 @0x50bbf0（update 分派入口）—— 由 +0x541 标志门控。
// 整帧实测：入口只有 `ldrb w8,[x19,#1345]; cbz w8, 0x50c618`；
// 生产端不产生该标志（恒 0），所以**整段不执行**，speed/life 都不改。
// 独立符号 0x50d238 里的 `speed=V1; life--` 是给别处调用的，不是逐帧路径。
void Func4(BulletMotion&, const MotionTargets&, DeterministicRng&) {
}

// bulletFunc29 @0x511384 —— 分段重置 + 合成 vx/vy。[实测]
void Func29(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    // 独立符号 0x511384 逐指令：三段（不是旧实现的 t<life / t==life / t>=life）。
    const int t = static_cast<int>(b.t);
    if (t < b.life) {
        b.vx = b.prev_angle * CosDeg(b.angle);
        b.vy = b.prev_angle * SinDeg(b.angle);
        return;
    }
    const int reset_at = b.life + b.p1;
    if (t < reset_at) {
        // 5113f4：只有 t == life+1 才写 A0；这一段 vx/vy 恒为 0。
        if (t == b.life + 1) {
            b.angle = b.angle_init;
        }
        b.vx = 0.0;
        b.vy = 0.0;
        return;
    }
    if (t == reset_at) {
        // 51140c：整块复位（angle=A0、{speed,spdAcc}={V1,X}、angleAcc=F98、speedCap=XC）
        b.angle = b.angle_init;
        b.speed = b.spd_init2;
        b.accel = b.xacc;
        b.angle_acc = b.f598;
        b.speed_cap = b.xacc_cap;
    }
    b.vx = b.speed * CosDeg(b.angle);
    b.vy = b.speed * SinDeg(b.angle);
}

// bulletFunc47 @0x50c9f8（update 内联）—— 节拍门控制是否合成速度。
//   d = t1-life；d<0 或 (d%p1)!=0（p1==0 时退化为 d!=0）→ vx=vy=0；
//   命中才 vx=speed·cos(angle)、vy=speed·sin(angle)。随后 b 50cafc 跳过默认尾段。
void Func47(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    const int d = static_cast<int>(b.t) - b.life;
    const int rem = (b.p1 != 0) ? (d % b.p1) : d;   // ARM sdiv 0 → 0
    if (d < 0 || rem != 0) {
        b.vx = 0.0;
        b.vy = 0.0;
        return;
    }
    b.vx = b.speed * CosDeg(b.angle);
    b.vy = b.speed * SinDeg(b.angle);
}

// ===========================================================================
// B 加速/减速
// ===========================================================================

// bulletFunc5 @0x50b654（update 分派入口）—— 同样由 +0x541 标志门控，
// 基准/生产端该标志恒 0 → 整段 no-op。独立符号 0x50d250 的加速与 life--
// 不是 update 的逐帧路径（整帧实测 speed 恒 3、life 不变）。
void Func5(BulletMotion&, const MotionTargets&, DeterministicRng&) {
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

// bulletFunc39 @0x50ef00 —— p2 状态机。[unicorn 逐指令实测]
//   p2==0：检查 x>320 / x<0（C=180）/ y>480（C=360）→ 镜像角、p2=1，
//          然后 fallthrough 到减速段；若都不命中且 p2==life 则进复位段。
//   p2==1：speedCap=0、spdAcc = -(speed/life)；**不写 angle**（旧实现写错了）。
//   p2==life(>=2)：整块 16B 复位（angle=A0 位拷贝、speed=V1、spdAcc=X、
//          angleAcc=F98、f56c=PA、speedCap=XC），p1=2/3/4 时换 bulletType 42/37/30。
//   末尾 p2>=1 → p2++。
void Func39(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    bool decel = false;
    bool reset = false;
    if (b.p2 == 0) {
        double mirror = -1.0;
        if (b.x > kHalfW || b.x < 0.0) {
            mirror = kDeg180;
        } else if (b.y > kHalfH) {
            mirror = kDeg360;
        }
        if (mirror >= 0.0) {
            b.angle = MirrorAngle(b.angle, mirror);
            b.p2 = 1;
            decel = true;
        } else if (b.p2 == b.life) {
            reset = true;
        }
    } else if (b.p2 == 1) {
        decel = true;
    } else if (b.p2 == b.life) {
        reset = true;
    }

    if (decel) {
        b.speed_cap = 0.0;
        // 原版 50efc8 是 `fdiv s0, speed, float(life)`，life==0 得到 IEEE ±inf；
        // 共用出口 50c8f0 应用这个无穷加速度后会把 speed 夹到 speedCap(0)。
        // 加 `life!=0` 保护反而让 func39 的 speed 停在 -1，与整帧基准不符。
        b.accel = -(b.speed / static_cast<double>(b.life));
    } else if (reset) {
        b.angle = b.angle_init;
        b.angle_acc = b.f598;
        b.f56c = b.prev_angle;
        b.speed = b.spd_init2;
        b.accel = b.xacc;
        b.speed_cap = b.xacc_cap;
        if (b.p1 == 4) {
            b.bullet_kind = 30;
        } else if (b.p1 == 3) {
            b.bullet_kind = 37;
        } else if (b.p1 == 2) {
            b.bullet_kind = 42;
        }
    }
    if (b.p2 >= 1) {
        b.p2 += 1;
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
    // 0x510bcc 全程 s 寄存器（float），末尾 p2++。
    // 用 double 中间量会在 pf2*pf1 上放大出 ~0.2 的 hit_dis 误差。
    const float arg = static_cast<float>(b.spd_init2)
                      + static_cast<float>(b.prev_angle) * static_cast<float>(b.p2);
    const float s = SinDegF(arg);
    const float pf1 = static_cast<float>(b.angle_init)
                      + static_cast<float>(b.f598) * s;
    b.pf1 = pf1;
    b.scale = pf1;               // 0x510c00 setScale(pf1)
    b.hit_dis = static_cast<float>(b.pf2) * pf1;
    b.p2 += 1;                  // 0x510c20
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
        // 0x50dc20 `fcmp d,A0` 之后两个分支复用同一组 flags：
        //   b.le → 50dc30 的 `b.pl` 只在 d==A0 时成立（保持 target）；
        //   d > A0 → prev+A0；d < A0 → prev-A0。
        // 旧实现把第三支写成 d<0，导致 0<=d<A0 时错误地保持 target。
        const double d = target - b.prev_angle;
        double out = target;
        if (d > b.angle_init) {
            out = b.prev_angle + b.angle_init;
        } else if (d < b.angle_init) {
            out = b.prev_angle - b.angle_init;
        }
        b.angle = out;
        b.prev_angle = out;
    }
}

// bulletFunc18 @0x50dc4c —— 状态机：p2 保存上一次的瞄准角。[unicorn 逐指令实测]
//   t1 == life：p2 = (int)getAngleToEnemy(pos, angle, (int)angleInit)
//   t1 >  life：aim = getAngleToMyShip(pos)；先写 angle=aim，
//               d = aim - p2；d>p1 → angle=p1+p2；d<p1 → angle=p2-p1；
//               否则保持 aim；末尾 p2=(int)angle。
//   ★ 注意原版 else 分支的门是 d<p1（不是 d<-p1），与 func17 同类原版写法。
void Func18(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    const int t = static_cast<int>(b.t);
    if (t == b.life) {
        b.p2 = static_cast<int>(AngleToEnemy(b, tg));
    }
    if (t > b.life) {
        const double aim = AngleToMyShip(b, tg);
        b.angle = aim;
        const double lim = static_cast<double>(b.p1);
        const double d = aim - static_cast<double>(b.p2);
        if (d > lim) {
            b.angle = lim + static_cast<double>(b.p2);
        } else if (d < lim) {
            b.angle = static_cast<double>(b.p2) - lim;
        }
        b.p2 = static_cast<int>(b.angle);
    }
}

// bulletFunc48 @0x50f6c4 —— 蓄力 30 帧后一次性复位。[unicorn 逐指令实测]
//   t1<life → no-op；life<=t1<life+30 → speed=0（**不写 angle**）；
//   t1==life+30 → p2==0 瞄准自机 / p2==1 取 A0，随后整块 16B 复位
//   （angleAcc=F98、f56c=PA、v=V1、spdAcc=X、speedCap=XC），
//   p1==0→kind37、p1==1→kind206。
void Func48(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    const int t = static_cast<int>(b.t);
    if (t < b.life) {
        return;
    }
    if (t < b.life + 30) {
        b.speed = 0.0;
        return;
    }
    if (t != b.life + 30) {
        return;
    }
    if (b.p2 == 1) {
        b.angle = b.angle_init;
    } else if (b.p2 == 0) {
        b.angle = AngleToMyShip(b, tg);
    }
    b.angle_acc = b.f598;
    b.f56c = b.prev_angle;
    b.speed = b.spd_init2;
    b.accel = b.xacc;
    b.speed_cap = b.xacc_cap;
    if (b.p1 == 1) {
        b.bullet_kind = 206;
    } else if (b.p1 == 0) {
        b.bullet_kind = 37;
    }
}

// ===========================================================================
// E 周期瞄准 / 出屏转向
// ===========================================================================

// bulletFunc11 @0x50bc4c（update 入口内联调用 bulletFunc11Efff）。
//   ★ 独立符号的**返回值**被 50c2fc 写回 angle，生产此前完全漏了这一步。
//   屏内返回第三参数（常量 90）；出屏且 p1==1/2 才返回瞄准角并 life--。
//   入口还有 `life<1` 门。
void Func11(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (b.life < 1) {
        return;
    }
    const bool out = (b.x > kHalfW || b.x < 0.0 || b.y > kHalfH);
    if (out && b.p1 == 1) {
        b.angle = AngleToMyShip(b, tg);
        b.life -= 1;
        return;
    }
    if (out && b.p1 == 2) {
        b.angle = AngleToEnemy(b, tg);
        b.life -= 1;
        return;
    }
    b.angle = 90.0;      // 0xac5384，update 调用点传给第 3 个 float 参数的常量
}

// bulletFunc12 @0x50d7a8 —— 出屏（含 y<40）后重新瞄准。[实测]
void Func12(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    // update 分派入口 0x50bc68：`life < 1` 整段跳过。缺这个门会让
    // life-- 一直减到负数（p1/p2 档在 t4 之后明显发散）。
    if (b.life < 1) {
        return;
    }
    if (b.x > kHalfW || b.x < 0.0 || b.y > kHalfH || b.y < kTop) {
        // 0x50d840：出屏且 p1==1/2 时除了改角，还无条件 life--。
        if (b.p1 == 2) {
            b.angle = AngleToEnemy(b, tg);
            b.life -= 1;
        } else if (b.p1 == 1) {
            b.angle = AngleToMyShip(b, tg);
            b.life -= 1;
        }
    }
}

// bulletFunc13 @0x50d860 —— 近距限速追踪。[unicorn 逐指令实测]
//   0x50d870 t1==0 → p1=(int)angle；0x50d8d8 距离门 dist² >= life² 则**整段跳过**；
//   否则 getAngleToPoint(自机) 后按 p1%360 做 ±2° 限速：
//     d = aim - (p1%360)；angle = (float)(p1 + (d<2 ? -2 : 2))；p1 = (int)angle。
//   ★ 旧实现无条件 angle=自机角，且把 p1 写成 0x168(=360)，全错。
void Func13(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (static_cast<int>(b.t) == 0) {
        b.p1 = static_cast<int>(b.angle);
    }
    if (!tg.has_my_ship) {
        return;
    }
    const double dx = tg.my_ship_x - b.x;
    const double dy = tg.my_ship_y - b.y;
    const double dist2 = dx * dx + dy * dy;
    const double radius2 = static_cast<double>(b.life) * static_cast<double>(b.life);
    if (dist2 >= radius2) {
        return;
    }
    const double aim = AngleToMyShip(b, tg);
    const double d = aim - static_cast<double>(b.p1 % 360);
    b.angle = static_cast<float>(static_cast<double>(b.p1) + (d < 2.0 ? -2.0 : 2.0));
    b.p1 = static_cast<int>(b.angle);
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

// bulletFunc70 @0x51079c —— 三段独立出屏反射（x>320 / x<0 / y>480）。
//   [unicorn 逐指令实测] 每段：angle = C - angle + random(-p2,p2)，
//   前两段 C=180 且 d<0 时 +360 归一，第三段 C=360 **不归一**；
//   随后按 p1 的 5 档跳转表换皮（160/161/150/17/214，p1==2 额外 setScale(A0)，
//   p1==4 额外瞄准自机）；末尾 8B 拷贝 {V1,X}→{v,va}、speedCap=XC、life--。
void Func70(BulletMotion& b, const MotionTargets& tg, DeterministicRng& rng) {
    // update 分派入口 0x50ba8c：`life < 1` 整段跳过（life0=0 时原版为直线）。
    if (b.life < 1) {
        return;
    }
    const bool hit[3] = {b.x > kHalfW, b.x < 0.0, b.y > kHalfH};
    for (int seg = 0; seg < 3; ++seg) {
        if (!hit[seg]) {
            continue;
        }
        const double c = (seg == 2) ? kDeg360 : kDeg180;
        double a = c - b.angle + static_cast<double>(rng.RandomInt(-b.p2, b.p2));
        if (seg != 2 && a < 0.0) {
            a += kDeg360;
        }
        b.angle = a;
        switch (b.p1) {
            case 0: b.bullet_kind = 160; break;
            case 1: b.bullet_kind = 161; break;
            case 2: b.bullet_kind = 150; b.scale = b.angle_init; break;
            case 3: b.bullet_kind = 17; break;
            case 4: b.bullet_kind = 214; b.angle = AngleToMyShip(b, tg); break;
            default: break;
        }
        b.speed = b.spd_init2;
        b.accel = b.xacc;
        b.speed_cap = b.xacc_cap;
        b.life -= 1;
    }
}

// bulletFunc76 @0x510d30 —— y>480 → 瞄准 + 换皮。[实测]
void Func76(BulletMotion& b, const MotionTargets&, DeterministicRng& rng) {
    // 分派入口 0x50c0fc：`life < 1` 整段跳过。
    if (b.life < 1) {
        return;
    }
    if (b.y > kHalfH) {
        // 0x510d78 `s8 = 360-angle`，`s0 = float(random) + s8`，**不归一**。
        b.angle = kDeg360 - b.angle + static_cast<double>(rng.RandomInt(-b.p2, b.p2));
        // 0x510d80 8B 拷贝 {spdInit2,xacc}→{speed,spdAcc} + speedCap=XC。
        b.speed = b.spd_init2;
        b.accel = b.xacc;
        b.speed_cap = b.xacc_cap;
        if (b.p1 == 0) {
            b.bullet_kind = 17;      // 0x510da4
        }
        b.life -= 1;                 // 0x510db0
    }
}

// ===========================================================================
// F 边界反弹 / 镜像
// ===========================================================================

// bulletFunc1 @0x50ce78 —— 三段**独立**边界反弹。[unicorn 逐指令实测]
//   0x50cea4 x>320 / 0x50cf90 x<0 / 0x50d074 y>480；每命中一段各做一次
//   angle = (C-angle) 归一到 [0,360)（C = 180 或 360）、按 p1 换皮、life--。
//   ★ 原版**完全不写 speed / speedCap**；旧实现写了 spdInit2(-1) 导致子弹倒飞。
void Func1(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    // update 分派入口 0x50b514：`life < 1` 直接跳到共用出口（整段 no-op）。
    if (b.life < 1) {
        return;
    }
    int hits = 0;
    if (b.x > kHalfW) { b.angle = MirrorAngle(b.angle, kDeg180); ++hits; }
    if (b.x < 0.0)   { b.angle = MirrorAngle(b.angle, kDeg180); ++hits; }
    if (b.y > kHalfH) { b.angle = MirrorAngle(b.angle, kDeg360); ++hits; }
    b.life -= hits;
    // 0x50cee8 起每段命中后按 (p1-1) 走字节表 @0xb0a972 换 bulletType：
    // p1=1→30, 2→19, 3→109, 4→168；p1<=0 时 index 越界不换。
    if (hits > 0 && b.p1 >= 1 && b.p1 <= 4) {
        static constexpr int kKindByP1[4] = {30, 19, 109, 168};
        b.bullet_kind = kKindByP1[b.p1 - 1];
        // 表项 idx1(50cf60) 还会把 spdInit2 写进 speed；
        // idx3(50cf20) 是 8B 拷贝 {spdInit2,xacc}→{speed,spdAcc} 并 speedCap=XC。
        if (b.p1 == 2 || b.p1 == 4) {
            b.speed = b.spd_init2;
        }
        if (b.p1 == 4) {
            b.accel = b.xacc;
            b.speed_cap = b.xacc_cap;
        }
    }
}

// bulletFunc7 @0x50d3b8 —— 四段独立边界反弹，每段 life--，p1>=1 时叠加
//   MyMathUtil::random(-p1, p1)。[unicorn 逐指令实测]
//   x>320/x<0 用 C=180，y>480/y<40 用 C=360。
void Func7(BulletMotion& b, const MotionTargets&, DeterministicRng& rng) {
    // update 分派入口 0x50bc14：`life < 1` 整段跳过。
    if (b.life < 1) {
        return;
    }
    // 原版四段各自 `bl MyMathUtil::random(-p1,p1)`（0x50d430 等），
    // 不是四段命中后统一加一次；调用次数不同会让整条 RNG 流错位。
    int hits = 0;
    const auto bounce = [&](bool hit, double c) {
        if (!hit) {
            return;
        }
        b.angle = MirrorAngle(b.angle, c);
        if (b.p1 >= 1) {
            b.angle += static_cast<double>(rng.RandomInt(-b.p1, b.p1));
        }
        ++hits;
    };
    bounce(b.x > kHalfW, kDeg180);
    bounce(b.x < 0.0, kDeg180);
    bounce(b.y > kHalfH, kDeg360);
    bounce(b.y < kTop, kDeg360);
    b.life -= hits;
}

// bulletFunc20 @0x50dde0 —— 签名带 Vec2& 位置入/出参，会**直接改写位置**。
//   [unicorn 逐指令实测] 常量：80(0xacd9b4) / 240(0xace1a0) / 340(0xace1c0) /
//   180(0xace194) / 320 / 420 / 100。
//   life==0：x>240→x=320,life=1,+180；x<80→x=0,life=2,+180；
//            y>340→y=420,life=3,+180；y<180→y=100,life=4,+180。
//   life>0：只在 x∈(80,240) 且 y∈(180,340) 内才按 (life-1) 做四档折返链。
//   ★ 旧实现按 y<40/y>480/x>320/x<0 无条件改角，语义完全不同。
void Func20(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (b.life < 0) {
        return;
    }
    if (b.life == 0) {
        if (b.x > 240.0) {
            b.x = 320.0;
            b.life = 1;
            b.angle += kDeg180;
        } else if (b.x < 80.0) {
            b.x = 0.0;
            b.life = 2;
            b.angle += kDeg180;
        }
        if (b.y > 340.0) {
            b.y = 420.0;
            b.life = 3;
            b.angle += kDeg180;
        } else if (b.y < 180.0) {
            b.y = 100.0;
            b.life = 4;
            b.angle += kDeg180;
        }
        return;
    }
    if (b.x <= 80.0 || b.x >= 240.0 || b.y <= 180.0 || b.y >= 340.0) {
        return;
    }
    const int idx = b.life - 1;
    if (idx > 3) {
        return;
    }
    // 0x50de60 起的四档跳转表是**级联 fallthrough**（每档追加一次折返）。
    switch (idx) {
        case 0:
            b.x = 320.0; b.life = 2; b.angle += kDeg180;
            [[fallthrough]];
        case 1:
            b.x = 0.0; b.life = 3; b.angle += kDeg180;
            [[fallthrough]];
        case 2:
            b.y = 420.0; b.life = 4; b.angle += kDeg180;
            [[fallthrough]];
        case 3:
            b.y = 100.0; b.life = 5; b.angle += kDeg180;
            break;
        default:
            break;
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

// bulletFunc38 @0x50edcc —— 节拍折返。[unicorn 逐指令实测]
//   0x50edec 门：p2<1 直接返回；dt=t1-life>0 且 dt%life==0 才走主体；
//   主体：p2--、angle = angleInit + angle；随后若 p2-1==1 则换皮并 p1++。
//   dt<=0 且 p2==1 且 p1==1 时也换皮并 p1++。
//   ★ 旧实现无条件 angle = angleInit + angle（初值 -1 → 每帧偏 1°）。
void Func38(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    const int p2 = b.p2;
    if (p2 < 1) {
        return;
    }
    const int dt = static_cast<int>(b.t) - b.life;
    const bool on_beat = (dt > 0 && b.life != 0 && (dt % b.life) == 0);
    if (!on_beat) {
        if (p2 == 1 && b.p1 == 1) {
            b.p1 += 1;    // 0x50ee8c: 换贴图并 p1++
        }
        return;
    }
    b.p2 = p2 - 1;
    b.angle = b.angle_init + b.angle;
    if (p2 - 1 == 1) {
        b.p1 += 1;
    }
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

// bulletFunc58 @0x50bfa8（update 内联；独立符号 0x50fe00 不是逐帧路径）。
//   门：t1>=0 且 t1%life==0（life==0 时 ARM sdiv 0 → rem = t1）。
//   p1 偶：angle = 90 + A0；p1 奇：angle = 90 - A0（s2 固定为 0xac5384 = 90）。
//   随后 setRotation(360-angle+90) 只改贴图朝向；末尾 p1++。
void Func58(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    const int t = static_cast<int>(b.t);
    if (t < 0) {
        return;
    }
    const int rem = (b.life != 0) ? (t % b.life) : t;   // ARM sdiv 0 → 0
    if (rem != 0) {
        return;
    }
    b.angle = ((b.p1 & 1) == 0) ? (90.0 + b.angle_init) : (90.0 - b.angle_init);
    b.p1 += 1;
}

// bulletFunc59 @0x50c768（update 共用出口的内联分支）—— t1<life 时每帧 angle+1。
//   t1>=life 走 50c7ec：prevAngle 与 f598 符号相反时
//   f598 += prevAngle；angle += f598（基准下两者均为 0，无变化）。
//   独立符号表里没有 bulletFunc59，生产此前直接 no-op。
void Func59(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (static_cast<int>(b.t) < b.life) {
        b.angle += 1.0;
        return;
    }
    if ((b.prev_angle < 0.0 && b.f598 > 0.0) ||
        (b.prev_angle > 0.0 && b.f598 < 0.0)) {
        b.f598 += b.prev_angle;
        b.angle += b.f598;
    }
}

// bulletFunc62 @0x510088 —— 签名 (float x, float y)，经 x8 出参**直接改写位置**。
//   [unicorn 逐指令实测] cw/ch = getContentSize() = 2*halfW/2*halfH。
//   life>=1：四段边界折返，命中段把位置钉到 -cw / halfW+cw / 40-ch / halfH+ch，
//            每段 life--、kind=47；末尾写出 (s8,s9)。
//   life<1 ：同样四段，但**不改位置**，改的是 angle = 180/0/270/90 + random(-80,80)
//            （四次调用按序消耗 RNG）、speed = xacc 的位拷贝、kind=45、life--。
//   ★ 旧实现把它当成普通边界反弹，且 x/y 边界常数写错。
void Func62(BulletMotion& b, const MotionTargets&, DeterministicRng& rng) {
    // update 分派入口 0x50bff8：`tbnz life,#31` → life<0 时整段跳过。
    // 缺这个门会让 life 每帧继续递减，逐弹状态迅速发散。
    if (b.life < 0) {
        return;
    }
    const double cw = 2.0 * b.half_w;
    const double ch = 2.0 * b.half_h;
    if (b.life >= 1) {
        double nx = b.x;
        double ny = b.y;
        if (b.half_w + cw < b.x) {
            nx = -cw; b.life -= 1; b.bullet_kind = 47;
        } else if (nx < -cw) {
            nx = b.half_w + cw; b.life -= 1; b.bullet_kind = 47;
        }
        if (b.half_h + ch < b.y) {
            ny = kTop - ch; b.life -= 1; b.bullet_kind = 47;
        } else if (ny < kTop - ch) {
            ny = b.half_h + ch; b.life -= 1; b.bullet_kind = 47;
        }
        b.x = nx;
        b.y = ny;
        return;
    }
    if (b.half_w + cw < b.x) {
        b.bullet_kind = 45;
        b.angle = kDeg180 + static_cast<double>(rng.RandomInt(-80, 80));
        b.speed = b.xacc; b.life -= 1;
    }
    if (-cw > b.x) {
        b.bullet_kind = 45;
        b.angle = static_cast<double>(rng.RandomInt(-80, 80));
        b.speed = b.xacc; b.life -= 1;
    }
    if (b.half_h + ch < b.y) {
        b.bullet_kind = 45;
        b.angle = 270.0 + static_cast<double>(rng.RandomInt(-80, 80));
        b.speed = b.xacc; b.life -= 1;
    }
    if (kTop - ch > b.y) {
        b.bullet_kind = 45;
        b.angle = 90.0 + static_cast<double>(rng.RandomInt(-80, 80));
        b.speed = b.xacc; b.life -= 1;
    }
}

// bulletFunc67 @0x510544 —— 四段独立反弹。[unicorn 逐指令实测]
//   0x510584 门：life-1 < 0 → 整段 no-op（life0=0 时原版完全不动）。
//   每段命中：angle = C-angle（x 段 C=180、y 段 C=360）、p1++、life--。
void Func67(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    if (b.life >= 1) {
        int hits = 0;
        if (b.x > kHalfW) { b.angle = MirrorAngle(b.angle, kDeg180); ++hits; }
        if (b.x < 0.0)   { b.angle = MirrorAngle(b.angle, kDeg180); ++hits; }
        if (b.y > kHalfH) { b.angle = MirrorAngle(b.angle, kDeg360); ++hits; }
        if (b.y < kTop)  { b.angle = MirrorAngle(b.angle, kDeg360); ++hits; }
        b.life -= hits;
        b.p1 += hits;
    }
    // 0x5106a4 尾段：p1==1 时 p2 每帧 +1；p2==10 换皮 198 + 整块 16B 复位。
    if (b.p1 == 1) {
        if (b.p2 == 10) {
            b.bullet_kind = 198;
            b.angle_acc = b.f598;
            b.f56c = b.prev_angle;
            b.speed = b.spd_init2;
            b.accel = b.xacc;
            b.speed_cap = b.xacc_cap;
        }
        b.p2 += 1;
    }
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
    // 分派入口 0x50c180：`life < 1` 整段跳过（life 减到 0 后不再翻转）。
    // 0x510ea4 逐指令：y < 40 时 angle = 360-angle（不是 180-angle），并 life--。
    if (b.life < 1) {
        return;
    }
    if (b.y < kTop) {
        b.angle = MirrorAngle(b.angle, kDeg360);
        b.life -= 1;
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
    // 0x5112b4 `str s8,[x19,#1356]` 无条件写 hitDis；三档初值实测均为 4.0，
    // 只有 bulletType==0x25 且距离门命中时才取 20.0。旧实现漏了默认写回。
    b.hit_dis = 4.0;
    if (DistSqToMyShip(b, tg) < static_cast<double>(b.life) && b.bullet_kind == 0x25) {
        b.hit_dis = 20.0;
    }
}

// ===========================================================================
// H 分裂/召唤（Boss）—— 需宿主回调生成子子弹，这里只做状态标记。
// ===========================================================================

// bulletFunc31 @0x50e498 —— t1==p1 时按 (life-1) 走 31 档跳转表换皮 + 设半径。
//   [unicorn 逐指令实测：life=1..31 逐档探测 hitDis / bulletType]
//   2.4 / 3 / 3 / 43 / 2.4 / 2.4 来自 0x4019999A / 0x40400000 / 0x422C0000，
//   第 7 档起只换 bulletType（不改 hitDis）。
void Func31(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    const int t = static_cast<int>(b.t);
    // 本项目扩展：链式分裂（1→8→64）的宿主标记沿用旧条件 life<=0x1e，
    // 与原版逐弹状态的 life>=1 门**解耦**。否则 life=0 的宿主不再分裂，
    // phk_danmaku_chain_tests 会退化成「只有 1 颗弹」。
    if (t == b.p1 && b.life <= 0x1e) {
        b.rotate_flg = 2;
        b.p2 = 1;
    }
    if (t != b.p1) {
        return;
    }
    const int idx = b.life - 1;
    if (idx < 0 || idx > 30) {
        return;
    }
    static constexpr float kHitDis[31] = {
        2.4f, 3.0f, 3.0f, 43.0f, 2.4f, 2.4f,
        4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 4.0f,
        4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 4.0f,
        4.0f, 4.0f, 4.0f, 4.0f, 4.0f,
    };
    static constexpr int kKind[31] = {
        45, 45, 45, 45, 45, 45,
        207, 210, 135, 206, 10, 186, 91, 184, 187, 189,
        184, 189, 189, 175, 150, 169, 217, 218, 199, 211,
        213, 215, 64, 69, 70,
    };
    b.hit_dis = kHitDis[idx];
    b.bullet_kind = kKind[idx];
}

// bulletFunc50 @0x50f800 —— 三阶段蓄力状态机。[unicorn 逐指令实测]
//   life==0：x>280 / x<40 / y>440 / y<80 → life=1、8B 清零 speed+spdAcc、speedCap=0
//            （旧实现的 40/80 写反了）。
//   life==1：p1 每帧 +1；p1<=12 且为偶数时蓄力特效，p1==12 时 life=2。
//   life==2：p2 每帧 +1；p2==30 时整块 16B 复位。
void Func50(BulletMotion& b, const MotionTargets&, DeterministicRng& rng) {
    if (b.life == 0) {
        if (b.x > 280.0 || b.x < 40.0 || b.y > 440.0 || b.y < 80.0) {
            b.life = 1;
            b.speed = 0.0;
            b.accel = 0.0;
            b.speed_cap = 0.0;
        }
        return;
    }
    if (b.life == 1) {
        if (b.p1 <= 12 && (b.p1 & 1) == 0) {
            if (b.p1 == 12) {
                b.life = 2;
            } else {
                // 原版两次 MyMathUtil::random(-5,5) 只用于 BossChargeEffect 位置。
                rng.RandomInt(-5, 5);
                rng.RandomInt(-5, 5);
            }
        }
        b.p1 += 1;
        return;
    }
    if (b.life == 2) {
        if (b.p2 == 30) {
            b.angle = b.angle_init;
            b.angle_acc = b.f598;
            b.f56c = b.prev_angle;
            b.speed = b.spd_init2;
            b.accel = b.xacc;
            b.speed_cap = b.xacc_cap;
        }
        b.p2 += 1;
    }
}

// bulletFunc55 @0x50fca0 —— 接近敌机 → 蓄力特效。[推断：事件仅]
void Func55(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    if (DistSqToEnemy(b, tg) <= static_cast<double>(b.life)) {
        b.p2 = 1;  // 请求蓄力特效（宿主处理）
    }
}

// bulletFunc83 @0x511fcc —— 双谐波参数方程，**绝对定位**。[unicorn 逐指令实测]
//   d = t1-p2 < 0 → 整段跳过；r = min(float(0.2*d), 60)；A = 30*sin(d)。
//   x = r*cos(d) + 160 + cos(A)*life - sin(A)*p1
//   y = r*sin(d) + 260 + sin(A)*life + cos(A)*p1
//   末尾 setRotation(360-d+90) 只改贴图朝向。★ 原版不写 angle/speed。
void Func83(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    const double d = b.t - static_cast<double>(b.p2);
    if (d < 0.0) {
        return;
    }
    const double r = std::min(static_cast<double>(static_cast<float>(0.2 * d)), 60.0);
    const double a = 30.0 * SinDeg(d);
    const double life = static_cast<double>(b.life);
    const double p1 = static_cast<double>(b.p1);
    b.x = r * CosDeg(d) + 160.0 + CosDeg(a) * life - SinDeg(a) * p1;
    b.y = r * SinDeg(d) + 260.0 + SinDeg(a) * life + CosDeg(a) * p1;
}

// ===========================================================================
// I 换皮 / 半径
// ===========================================================================

// bulletFunc30 @0x50e224 —— 按 bulletType 换贴图 + hitDis。[实测]
void Func30(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    // 0x50e248 逐指令：d = t1-life；d<0 → 只做 setRotation（不改状态）。
    //   rem = d % p1（p1==0 时 ARM sdiv 0 → rem = d）；
    //   rem==0 → 换贴图并把 hitDis 写 20.0（0x41a00000）；
    //   rem==p2-1 → 换贴图并把 hitDis 写 4.0（0x40800000）；
    //   其余且 rem<p2 → 只做 setRotation(360 - rotateAngle + 90)。
    // 旧实现把门写成 `d == p2`，只在 p0 哨兵档碰巧等价。
    const int d = static_cast<int>(b.t) - b.life;
    if (d < 0) {
        return;
    }
    const int rem = (b.p1 != 0) ? (d % b.p1) : d;
    if (rem == 0) {
        b.hit_dis = 20.0;
    } else if (rem == b.p2 - 1) {
        b.hit_dis = 4.0;
    }
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
        // 字节表 @0xb0a9c7 = [0,26,28,30,32,34,36] → 目标地址
        // 50fbd0 / 50fc38 / 50fc40 / 50fc48 / 50fc50 / 50fc58 / 50fc60，
        // 对应 kind 163,157,158,159,160,161,162（旧实现写成了逆序）。
        static constexpr int kTypeByP2Mod7[7] = {0xa3, 0x9d, 0x9e, 0x9f, 0xa0, 0xa1, 0xa2};
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
    // 0x511524: 参数方程（绝对坐标，走 setPosition）：
    //   r = p2 * sin(X*t1 + XC)；a = angle + speed*t1
    //   x = life + r*cos(a)；y = p1 + r*sin(a)
    const double t = b.t;
    const double r = static_cast<double>(b.p2) * SinDeg(b.xacc * t + b.xacc_cap);
    const double a = b.angle + b.speed * t;
    b.x = static_cast<double>(b.life) + r * CosDeg(a);
    b.y = static_cast<double>(b.p1) + r * SinDeg(a);
}

// bulletFunc33 @0x51160c —— 双正弦参数轨迹。[推断]
void Func33(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    // 独立符号 0x51160c 逐指令：只把两个「目标坐标」写进 xacc(1444) /
    // xaccCap(1448)，尾部 atan2f 只用于 setRotation。
    // ★ 原版**完全不写 angle**（旧实现写 AngleToPoint 会让 p1/p2 档发散）。
    const double t = b.t;
    const double x = b.angle_init + b.speed * t * CosDeg(b.angle)
                     + b.spd_init2 * CosDeg(b.f598 * t);
    const double y = b.prev_angle + b.speed * t * SinDeg(b.angle)
                     + b.spd_init2 * SinDeg(b.f598 * t);
    b.xacc = x;
    b.xacc_cap = y;
}

// bulletFunc66 @0x511ab4 —— 参数方程。[推断]
void Func66(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    // 0x511ab4: 绝对坐标（setPosition）
    //   u = A0 * p2；a = X*p2 + vc
    //   x = PA  + u*cos(F98) + V1*cos(a)
    //   y = XC  + u*sin(F98) + V1*sin(a)
    const double u = b.angle_init * static_cast<double>(b.p2);
    const double a = b.xacc * static_cast<double>(b.p2) + b.speed_cap;
    b.x = b.prev_angle + u * CosDeg(b.f598) + b.spd_init2 * CosDeg(a);
    b.y = b.xacc_cap + u * SinDeg(b.f598) + b.spd_init2 * SinDeg(a);
    b.p2 += 1;          // 0x511bf0 附近：末尾 p2++
}

// bulletFunc75 @0x511c08 —— 双正弦参数轨迹。[推断]
void Func75(BulletMotion& b, const MotionTargets&, DeterministicRng&) {
    // 0x511c08: 极坐标（绝对定位）
    //   r = V1 + X*p2；a = A0 + F98*p2
    //   x = PA + r*cos(a)；y = XC + r*sin(a)；末尾 p2++
    const double p2 = static_cast<double>(b.p2);
    const double r = b.spd_init2 + b.xacc * p2;
    const double a = b.angle_init + b.f598 * p2;
    b.x = b.prev_angle + r * CosDeg(a);
    b.y = b.xacc_cap + r * SinDeg(a);
    b.p2 += 1;
}

// bulletFunc82 @0x511e48 —— 以**自机坐标**为基点的极坐标绝对定位。
//   [unicorn 逐指令实测] d = t1-p2；ang = A0 + F98*t1；r = V1 + (d>=0 ? X*d : 0)，
//   再取 r = max(r, XC)；x = ship.x + r*cos(ang)；y = ship.y + r*sin(ang)。
//   getEnemyPos 只用于尾段擦弹标志，不进逐弹状态；原版不写 angle/speed。
void Func82(BulletMotion& b, const MotionTargets& tg, DeterministicRng&) {
    const double d = b.t - static_cast<double>(b.p2);
    double r = b.spd_init2;
    if (d >= 0.0) {
        r += b.xacc * d;
    }
    r = std::max(r, b.xacc_cap);
    const double ang = b.angle_init + b.f598 * b.t;
    const double ox = tg.has_my_ship ? tg.my_ship_x : 320.0;
    const double oy = tg.has_my_ship ? tg.my_ship_y : 600.0;
    b.x = ox + r * CosDeg(ang);
    b.y = oy + r * SinDeg(ang);
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

// bulletFunc27 —— update 的分派落在共用出口 0x50c618，比较链里没有 27，
// 全库也没有 `bl bulletFunc27@plt`：整帧实测 hitDis 恒为初值 4。
// 独立符号 0x50cd54 的 hitDis *= life*0.01 不是逐帧路径。
void Func27(BulletMotion&, const MotionTargets&, DeterministicRng&) {
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
    // 表 @0xacee48 = [+90, -90]，update  inline 版是 `s2(90) + 表值`
    // 赋给 angle（不是累加）：life!=0 → 180°，life==0 → 0°。
    b.angle = 90.0 + ((b.life == 0) ? -90.0 : 90.0);
    b.bullet_loop += 1;
    b.bullet_life = b.p2 * b.bullet_loop + (b.p1 + t);
}

// bulletFunc52 @0x50f9e4 —— 随机角。[实测]
void Func52(BulletMotion& b, const MotionTargets&, DeterministicRng& rng) {
    // 0x50f9f0 逐指令：t1>=0 且 t1%life==0（life==0 时退化为 t1==0）；
    //   `neg w0,w1` → random(-p2,p2)，结果再乘 p1 写进 angle。
    const int t = static_cast<int>(b.t);
    if (t < 0) {
        return;
    }
    const int rem = (b.life != 0) ? (t % b.life) : t;   // ARM sdiv 0 → 0
    if (rem != 0) {
        return;
    }
    b.angle = static_cast<double>(b.p1) * rng.RandomInt(-b.p2, b.p2);
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
    // 0x510034 是 8B 拷贝 {spdInit2,xacc}→{speed,spdAcc}，
    // 漏写 spdAcc 会让同帧的共用出口少加一次 xacc。
    b.speed = b.spd_init2;
    b.accel = b.xacc;
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
    // 0x51172c: 第一段不是角度而是 **setOpacity**：alpha = (int)(75*sin(X*t1+life-90) + 180)
    //   （常量 s8 在 0xac5380 = -90.0；adrp 是 ac5000 不是 ace000）
    //   随后 x = A0 + XC*sin(X*t1+life)、y = F98 + V1*t1（setPosition 绝对坐标）
    //   末尾用同一个 sin 的符号写 hitHantei(0x548) 与 bombNoVanish(0x550)。
    const double t = b.t;
    const double arg = b.xacc * t + static_cast<double>(b.life);
    const double s = SinDeg(arg - 90.0);
    b.opacity = static_cast<int>(75.0 * s + 180.0);
    b.x = b.angle_init + b.xacc_cap * SinDeg(arg);
    b.y = b.f598 + b.spd_init2 * t;
    b.hit_hantei = (s >= 0.0);
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
        case 52: case 53: case 54: case 55: case 57: case 58: case 59: case 60: case 61:
        case 62: case 63: case 64: case 65: case 66: case 67: case 70: case 71:
        case 72: case 73: case 74: case 75: case 76: case 77: case 78: case 79:
        case 80: case 81: case 82: case 83: case 84: case 85:
            return true;
        default:
            return false;
    }
}

namespace {

// 原版 EnemyBullet::update 的三段调用时机（整帧执行实测）：
//   0 early = 分派入口 / 共用出口内联（0x50b514..0x50c618 之前）
//   1 mid   = func29，在 0x50c9ec（共用出口 accel 应用**之后**、
//             50ca58 速度合成**之前**），随后 b 0x50cafc
//   2 late  = 0x50cafc 二次分派里 `bl` 的 8 个独立符号（速度合成之后）
int BulletFuncStage(int func_no) {
    switch (func_no) {
        case 29:
            return 1;
        case 33: case 37: case 51: case 66:
        case 75: case 81: case 82: case 83:
            return 2;
        default:
            return 0;
    }
}

bool ApplyBulletFuncStage(BulletMotion& b, const MotionTargets& targets,
                          DeterministicRng& rng, int stage) {
    if (BulletFuncStage(b.bullet_func_no) != stage) {
        return true;   // 本阶段不执行，但编号是已知的
    }
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
        case 59: Func59(b, targets, rng); break;
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

}  // namespace

// 兼容外部调用（danmaku_tests 的确定性覆盖测试）：一次跑完三个阶段。
bool ApplyBulletFunc(BulletMotion& b, const MotionTargets& targets, DeterministicRng& rng) {
    const bool known = ApplyBulletFuncStage(b, targets, rng, 0);
    ApplyBulletFuncStage(b, targets, rng, 1);
    ApplyBulletFuncStage(b, targets, rng, 2);
    return known;
}

// 原版 EnemyBullet::update 的尾段真值（unicorn 整帧执行实测，见
// tools/tna_coverage/findings-20261109.md）：
//   50c978 起默认用 speed·cos/sin(angle) 合成 vx/vy；但 func3 的内联代码、
//   func29 的 `bl` 分支、func47 的节拍分支写完后都直接 `b 50cafc`，
//   **跳过速度合成**。
bool SkipsVelocitySynthesis(int func_no) {
    return func_no == 3 || func_no == 29 || func_no == 47;
}

// 50cafc 的二次分派表 @0xb0a938：下列编号不走 50cc5c 的默认位置积分
// （26 走参数方程 setPosition；其余 8 个 `bl` 到独立 func 后直接 b 50cc84）。
bool SkipsPositionIntegration(int func_no) {
    switch (func_no) {
        case 26: case 33: case 37: case 51: case 66:
        case 75: case 81: case 82: case 83:
            return true;
        default:
            return false;
    }
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
    // 3) early 阶段：分派入口 / 共用出口内联。
    ApplyBulletFuncStage(b, targets, rng, 0);
    // 4) 原版共用出口 0x50c8f0：spdAcc != 0 时 speed += accel 并夹到 speedCap。
    //    旧实现完全漏了这段，导致写 accel 的 func（39/53 等）速度不变化。
    if (b.accel > 0.0 && b.speed < b.speed_cap) {
        b.speed = std::min(b.speed + b.accel, b.speed_cap);
    } else if (b.accel < 0.0 && b.speed > b.speed_cap) {
        b.speed = std::max(b.speed + b.accel, b.speed_cap);
    }
    // 4b) mid 阶段：func29（0x50c9ec）。它必须在 accel 应用**之后**，
    //     否则它写入的 spdAcc 会在同帧被应用，speed 多出一个 xacc。
    ApplyBulletFuncStage(b, targets, rng, 1);
    // 5) 原版尾段：默认合成 vx/vy；少数 func 自己合成，跳过默认段。
    const int fn = b.bullet_func_no;
    if (!SkipsVelocitySynthesis(fn)) {
        b.IntegrateVelocity();
    }
    // 5b) late 阶段：0x50cafc 二次分派的 8 个 func（速度合成之后）。
    ApplyBulletFuncStage(b, targets, rng, 2);
    // 6) 宿主（BossRaceSimulation / phk_tna_replica）紧接着固定做
    //    pos += (vx,vy)。原版对二次分派里的这 9 个编号**跳过**该积分，
    //    这里预减位移，使积分后正好回到 func 用 setPosition 钉下的位置。
    if (SkipsPositionIntegration(fn)) {
        b.x -= b.vx;
        b.y -= b.vy;
    }
    // 7) 原版 0x50cc84：帧末 bulletLife--（>0 时）。
    //    它决定**下一帧开头**是否做 angle 累加/夹持；漏掉会让生产端
    //    每帧都夹持，而原版只在 bulletLife 归零前夹持（p1 档大批差异的根因）。
    if (b.bullet_life > 0) {
        b.bullet_life -= 1;
    }
}

}  // namespace phk::battle::danmaku
