// bullet_funcs_reimpl.hpp
// ============================================================================
// EnemyBullet 弹幕运动逻辑的 C++17 重实现（零依赖，仅标准库）
// 逆向自 libMyGame_arm64.so（AArch64）；每个函数注释标注原始地址与实测常量。
//
// 关键约定（实测）：
//   * 角度单位 = 度（degree）。MyMathUtil::sin/cos 内部 ×2π/360 再调 libm。
//   * 字段偏移 ↔ 成员对应见下方 BulletMotion 注释。
//   * update() 负责 angle/speed 积分与 vx/vy 合成；bulletFuncN 负责覆写与事件。
//   * 目标查询（自机/敌机位置、目标角）由宿主通过钩子提供。
//
// 编译检查： g++ -std=c++17 -fsyntax-only bullet_funcs_reimpl.hpp
// ============================================================================
#ifndef BULLET_FUNCS_REIMPL_HPP
#define BULLET_FUNCS_REIMPL_HPP

#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace touhou {

// ---------------------------------------------------------------------------
// 数学工具（角度制）
// ---------------------------------------------------------------------------
constexpr float PI = 3.14159265358979323846f;
inline float deg2rad(float d) { return d * (PI / 180.0f); }
inline float my_sin_deg(float d) { return std::sin(deg2rad(d)); }  // MyMathUtil::sin @0x5ab0c0
inline float my_cos_deg(float d) { return std::cos(deg2rad(d)); }  // MyMathUtil::cos @0x5ab0dc
// MyMathUtil::random(a,b) @0x5ab0f8：返回 [a,b] 内随机整数
inline int my_random(int a, int b) {
    if (b < a) { int t = a; a = b; b = t; }
    return a + (int)(std::rand() % (unsigned)(b - a + 1));
}
inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ---------------------------------------------------------------------------
// 宿主钩子：目标查询（移植时替换为真实实现）
//   返回值单位：度
// ---------------------------------------------------------------------------
struct HostHooks {
    // GameLayer::getMyShipPos() @0x2f8990
    void (*get_my_ship_pos)(float& x, float& y) = nullptr;
    // GameLayer::getEnemyPos(int) @0x2eedf0
    void (*get_enemy_pos)(int idx, float& x, float& y) = nullptr;
    // GameLayer::getAngleToMyShip(float,float) @0x2fb7d0
    float (*get_angle_to_my_ship)(float x, float y) = nullptr;
    // GameLayer::getAngleToEnemy(float,float,int,float) @0x2ea940
    float (*get_angle_to_enemy)(float x, float y, int idx, float f) = nullptr;
    // GameLayer::getAngleToPoint(float,float,float,float) @0x2ebb60
    float (*get_angle_to_point)(float x, float y, float px, float py) = nullptr;
    // 全局状态码（GameLayer 某 getter；用于状态机族）
    int   (*get_global_state)() = nullptr;
};
inline HostHooks& hooks() { static HostHooks h; return h; }

// ---------------------------------------------------------------------------
// BulletMotion
//   前 9 个成员为任务指定的“核心运动状态”：
//     x,y,vx,vy,angle,speed,accel,life,t
//   其余成员为各 bulletFunc 所需的构造参数 / 快照（对应 EnemyBullet 字段）。
// ---------------------------------------------------------------------------
struct BulletMotion {
    // ---- 核心状态（任务指定） ----
    float x = 0.0f;      // 位置
    float y = 0.0f;
    float vx = 0.0f;     // 每帧位移 X   (字段 +0x57c)
    float vy = 0.0f;     // 每帧位移 Y   (字段 +0x580)
    float angle = 0.0f;  // 角度(度)     (+0x560)
    float speed = 0.0f;  // 速度         (+0x570)
    float accel = 0.0f;  // 速度加速度   (+0x574 spdAcc)
    int   life = 0;      // 生命帧计数   (+0x5b8)
    float t = 0.0f;      // 帧计时器     (+0x5dc t1)

    // ---- 构造参数 / 快照（EnemyBullet 字段） ----
    float angleSpd  = 0.0f;   // +0x564
    float angleAcc  = 0.0f;   // +0x568
    float f56c      = 0.0f;   // +0x56c
    float speedCap  = 0.0f;   // +0x578
    float angleInit = -1.0f;  // +0x590 A0
    float speedInit = -1.0f;  // +0x594 V0
    float f598      = 0.0f;   // +0x598 F98（正弦幅度）
    float prevAngle = 0.0f;   // +0x59c PA
    float spdInit2  = -1.0f;  // +0x5a0 V1
    float xacc      = 0.0f;   // +0x5a4 X
    float xaccCap   = 0.0f;   // +0x5a8 XC
    float pf1       = 1.0f;   // +0x5c4
    float pf2       = 0.0f;   // +0x5c8
    int   hitDisFlag= 0;      // 占位（命中半径由 pf2*pf1 计算）
    int   bulletType= 0;      // +0x538
    int   rotateFlg = 0;      // +0x530
    int   bulletLife= 0;      // +0x588 N
    int   bulletLoop= 0;      // +0x58c LP
    int   delay     = 0;      // +0x5ac
    int   vanishDelay = 0;    // +0x5b0
    int   p1        = 0;      // +0x5bc
    int   p2        = 0;      // +0x5c0
    int   bulletFuncNo = 0;   // +0x5b4
    float hitDis    = 0.0f;   // +0x54c
    bool  hitHantei = true;   // +0x548
    float halfW     = 0.0f;   // +0x554
    float halfH     = 0.0f;   // +0x558
    int   opacity   = 255;    // 原版走 cocos2d::Node::setOpacity（vtable+0x490，已在 .rela 解出）
    float scale     = 1.0f;   // 原版走 cocos2d::Sprite::setScale（vtable+0x80）

    // 便捷：以 (angle,speed) 合成 vx/vy（update 默认尾段）
    void integrate_velocity() {
        vx = speed * my_cos_deg(angle);
        vy = speed * my_sin_deg(angle);
    }
};

// --- bulletFuncN 前置声明（供 update_common 调用） ---
inline void bulletFunc0(BulletMotion&, float);
inline void bulletFunc1(BulletMotion&, float);
inline void bulletFunc2(BulletMotion&, float);
inline void bulletFunc3(BulletMotion&, float);
inline void bulletFunc4(BulletMotion&, float);
inline void bulletFunc5(BulletMotion&, float);
inline void bulletFunc6(BulletMotion&, float);
inline void bulletFunc7(BulletMotion&, float);
inline void bulletFunc8(BulletMotion&, float);
inline void bulletFunc27(BulletMotion&, float);
inline void bulletFunc28(BulletMotion&, float);
inline void bulletFunc44(BulletMotion&, float);
inline void bulletFunc45(BulletMotion&, float);
inline void bulletFunc36(BulletMotion&, float);
inline void bulletFunc79(BulletMotion&, float);
inline void bulletFunc11(BulletMotion&, float);
inline void bulletFunc12(BulletMotion&, float);
inline void bulletFunc14(BulletMotion&, float);
inline void bulletFunc15(BulletMotion&, float);
inline void bulletFunc16(BulletMotion&, float);
inline void bulletFunc17(BulletMotion&, float);
inline void bulletFunc21(BulletMotion&, float);
inline void bulletFunc22(BulletMotion&, float);
inline void bulletFunc23(BulletMotion&, float);
inline void bulletFunc24(BulletMotion&, float);
inline void bulletFunc25(BulletMotion&, float);
inline void bulletFunc29(BulletMotion&, float);
inline void bulletFunc30(BulletMotion&, float);
inline void bulletFunc32(BulletMotion&, float);
inline void bulletFunc35(BulletMotion&, float);
inline void bulletFunc39(BulletMotion&, float);
inline void bulletFunc40(BulletMotion&, float);
inline void bulletFunc41(BulletMotion&, float);
inline void bulletFunc42(BulletMotion&, float);
inline void bulletFunc43(BulletMotion&, float);
inline void bulletFunc46(BulletMotion&, float);
inline void bulletFunc47(BulletMotion&, float);
inline void bulletFunc48(BulletMotion&, float);
inline void bulletFunc52(BulletMotion&, float);
inline void bulletFunc54(BulletMotion&, float);
inline void bulletFunc57(BulletMotion&, float);
inline void bulletFunc58(BulletMotion&, float);
inline void bulletFunc61(BulletMotion&, float);
inline void bulletFunc63(BulletMotion&, float);
inline void bulletFunc65(BulletMotion&, float);
inline void bulletFunc70(BulletMotion&, float);
inline void bulletFunc71(BulletMotion&, float);
inline void bulletFunc72(BulletMotion&, float);
inline void bulletFunc73(BulletMotion&, float);
inline void bulletFunc76(BulletMotion&, float);
inline void bulletFunc77(BulletMotion&, float);
inline void bulletFunc78(BulletMotion&, float);
inline void bulletFunc80(BulletMotion&, float);
inline void bulletFunc81(BulletMotion&, float);
inline void bulletFunc84(BulletMotion&, float);
inline void bulletFunc85(BulletMotion&, float);

// ===========================================================================
// update() 逐帧流程（对应 0x50b0b8）
//   1) delay 倒计时
//   2) 若 bulletLife != 0：angle += angleAcc，并按 f56c 夹持
//   3) 依 bulletFuncNo 调用 bulletFuncN
//   4) 默认尾段：vx = speed*cos(angle); vy = speed*sin(angle)
// 说明：位置 pos += (vx,vy) 由宿主在调用后完成。
// ===========================================================================
inline void update_common(BulletMotion& b, float dt) {
    (void)dt;
    if (b.delay > 0) { b.delay--; return; }
    if (b.bulletLife != 0) {
        b.angle += b.angleAcc;                                   // 0x50b494
        if (b.f56c > 0.0f) {                                     // 0x50b4a4
            float lim = b.f56c + b.angleSpd;
            if (b.angle > lim) b.angle = lim;
        }
        if (b.f56c < 0.0f) {                                     // 0x50b4c8
            float lim = b.f56c + b.angleSpd;
            if (b.angle < lim) b.angle = lim;
        }
    }
    switch (b.bulletFuncNo) {
        case  0: bulletFunc0 (b,dt); break;
        case  1: bulletFunc1 (b,dt); break;
        case  2: bulletFunc2 (b,dt); break;
        case  3: bulletFunc3 (b,dt); break;
        case  4: bulletFunc4 (b,dt); break;
        case  5: bulletFunc5 (b,dt); break;
        case  6: bulletFunc6 (b,dt); break;
        case  7: bulletFunc7 (b,dt); break;
        case  8: bulletFunc8 (b,dt); break;
        case 27: bulletFunc27(b,dt); break;
        case 28: bulletFunc28(b,dt); break;
        case 44: bulletFunc44(b,dt); break;
        case 45: bulletFunc45(b,dt); break;
        case 36: bulletFunc36(b,dt); break;
        case 79: bulletFunc79(b,dt); break;
        case 11: bulletFunc11(b,dt); break;
        case 12: bulletFunc12(b,dt); break;
        case 14: bulletFunc14(b,dt); break;
        case 15: bulletFunc15(b,dt); break;
        case 16: bulletFunc16(b,dt); break;
        case 17: bulletFunc17(b,dt); break;
        case 21: bulletFunc21(b,dt); break;
        case 22: bulletFunc22(b,dt); break;
        case 23: bulletFunc23(b,dt); break;
        case 24: bulletFunc24(b,dt); break;
        case 25: bulletFunc25(b,dt); break;
        case 29: bulletFunc29(b,dt); break;
        case 30: bulletFunc30(b,dt); break;
        case 32: bulletFunc32(b,dt); break;
        case 35: bulletFunc35(b,dt); break;
        case 39: bulletFunc39(b,dt); break;
        case 40: bulletFunc40(b,dt); break;
        case 41: bulletFunc41(b,dt); break;
        case 42: bulletFunc42(b,dt); break;
        case 43: bulletFunc43(b,dt); break;
        case 46: bulletFunc46(b,dt); break;
        case 47: bulletFunc47(b,dt); break;
        case 48: bulletFunc48(b,dt); break;
        case 52: bulletFunc52(b,dt); break;
        case 54: bulletFunc54(b,dt); break;
        case 57: bulletFunc57(b,dt); break;
        case 58: bulletFunc58(b,dt); break;
        case 61: bulletFunc61(b,dt); break;
        case 63: bulletFunc63(b,dt); break;
        case 65: bulletFunc65(b,dt); break;
        case 70: bulletFunc70(b,dt); break;
        case 71: bulletFunc71(b,dt); break;
        case 72: bulletFunc72(b,dt); break;
        case 73: bulletFunc73(b,dt); break;
        case 76: bulletFunc76(b,dt); break;
        case 77: bulletFunc77(b,dt); break;
        case 78: bulletFunc78(b,dt); break;
        case 80: bulletFunc80(b,dt); break;
        case 81: bulletFunc81(b,dt); break;
        case 84: bulletFunc84(b,dt); break;
        case 85: bulletFunc85(b,dt); break;
        default: break;   // 未实现者见 bulletFunc_unknown
    }
    b.integrate_velocity();
}

// ===========================================================================
// 逐 bulletFunc 实现
// ===========================================================================

// bulletFunc0 @0x50cdcc (172B) —— 基础初始化 + 直线
//   应用构造快照：A0/V0/F98/PA/V1/X/XC，然后合成 vx/vy。
inline void bulletFunc0(BulletMotion& b, float dt) {
    (void)dt;
    const float NEG1 = -1.0f;                                    // fmov s0,#-1.0
    if (b.angleInit != NEG1) b.angle = b.angleInit;              // 0x50cdf0
    if (b.speedInit != NEG1) {                                   // 0x50ce04
        b.angleSpd = b.speedInit;                                // 0x50ce04
        b.angleAcc = b.f598;                                     // str x8,[0x568] (拷贝 0x598→0x568)
        b.f56c     = b.prevAngle;                                // 0x59c→0x56c
    }
    if (b.spdInit2 != NEG1) b.speed = b.spdInit2;                // 0x50ce18
    if (b.xacc     != NEG1) b.accel = b.xacc;                    // 0x50ce28
    if (b.xaccCap  != NEG1) b.speedCap = b.xaccCap;              // 0x50ce38
    b.integrate_velocity();                                      // 0x50ce44..0x50ce64
}

// bulletFunc4 @0x50d238 (24B) —— 速度重置为 spdInit2
inline void bulletFunc4(BulletMotion& b, float dt) {
    (void)dt;
    b.speed = b.spdInit2;                                        // 0x50d240
}

// bulletFunc5 @0x50d250 (152B) —— 加速到上限（v += X，夹 XC）
inline void bulletFunc5(BulletMotion& b, float dt) {
    (void)dt;
    float a = b.xacc;                                            // 0x5a4
    if (a == 0.0f) return;
    if (a > 0.0f) {
        if (b.speed < b.xaccCap) {                               // 0x5a8
            b.speed += a;
            if (b.speed > b.xaccCap) b.speed = b.xaccCap;        // 0x50d280
        }
    } else {
        if (b.speed > b.xaccCap) {
            b.speed += a;
            if (b.speed < b.xaccCap) b.speed = b.xaccCap;
        }
    }
}

// bulletFunc2 @0x50d16c (204B) —— 朝自机限速转向
//   每帧把角度朝目标转，单帧增量受 angleAcc(0x568) 限制；A0 更新为当前角。
inline void bulletFunc2(BulletMotion& b, float dt) {
    (void)dt;
    if (b.life < 1) return;                                      // 0x50d174
    float target = hooks().get_angle_to_my_ship
        ? hooks().get_angle_to_my_ship(b.x, b.y) : b.angle;      // @0x2fb7d0
    float d  = target - b.angleInit;                             // s5
    float d2 = d - 360.0f;                                       // 常量 360 @0xace32c(-360)
    b.angle = target;                                            // 0x50d1f4
    if (d2 <= b.angleAcc)      b.angle = b.angleInit + b.angleAcc;
    if (d2 >= -b.angleAcc)     b.angle = b.angleInit - b.angleAcc; // fneg
    b.angleInit = b.angle;                                       // 0x50d21c
}

// bulletFunc1 @0x50ce78 (756B) —— 屏幕边界反弹 + 按 P1 换皮
//   x>320 / x<0 / y>480 → angle = 180 - angle（+360 归一）；v=V1; vc=XC。
inline void bulletFunc1(BulletMotion& b, float dt) {
    (void)dt;
    const float HALF_W = 320.0f;   // @0xac5188
    const float HALF_H = 480.0f;   // @0xac518c
    const float DEG180 = 180.0f;   // @0xace194
    const float DEG360 = 360.0f;   // @0xace178
    auto bounce = [&]() {
        b.angle = DEG180 - b.angle;                              // fsub s0,s1,s0
        if (b.angle < 0.0f) b.angle += DEG360;                   // fcmp #0 ; fadd
    };
    if (b.x > HALF_W) { bounce(); }
    else if (b.x < 0.0f) { bounce(); }
    else if (b.y > HALF_H) { bounce(); }
    b.speed    = b.spdInit2;                                     // str x8,[0x570]
    b.speedCap = b.xaccCap;                                      // str w9,[0x578]
    // 依 P1(0..3) 切换外观： setBulletType(0x1e/0x6d/0xa8/0x13) —— 由宿主执行
}

// bulletFunc7 @0x50d3b8 (444B) —— 同 1，且 P1>=1 时叠加随机角
inline void bulletFunc7(BulletMotion& b, float dt) {
    (void)dt;
    const float HALF_W = 320.0f, HALF_H = 480.0f, DEG180 = 180.0f, DEG360 = 360.0f;
    auto bounce = [&]() {
        b.angle = DEG180 - b.angle;
        if (b.angle < 0.0f) b.angle += DEG360;
        if (b.p1 >= 1) b.angle += (float)my_random(0, 360);       // MyMathUtil::random
    };
    if (b.x > HALF_W) bounce();
    else if (b.x < 0.0f) bounce();
    else if (b.y > HALF_H) bounce();
    else if (b.y < 40.0f) bounce();                               // @0xace180 = 40
}

// bulletFunc11 @0x50d6e4 (196B) —— 出屏后重新瞄准
inline void bulletFunc11(BulletMotion& b, float dt) {
    (void)dt;
    const float HALF_W = 320.0f, HALF_H = 480.0f;
    if (b.x > HALF_W || b.x < 0.0f || b.y > HALF_H) {            // 参数为 (x,y) 传入
        if (b.p1 == 2 && hooks().get_angle_to_enemy)
            b.angle = hooks().get_angle_to_enemy(b.x, b.y, b.p2, b.angle);  // @0x2ea940
        else if (b.p1 == 1 && hooks().get_angle_to_my_ship)
            b.angle = hooks().get_angle_to_my_ship(b.x, b.y);                // @0x2fb7d0
    }
}

// bulletFunc12 @0x50d7a8 (184B) —— 出屏(含 y<40) 后重新瞄准
inline void bulletFunc12(BulletMotion& b, float dt) {
    (void)dt;
    const float HALF_W = 320.0f, HALF_H = 480.0f, TOP = 40.0f;
    if (b.x > HALF_W || b.x < 0.0f || b.y > HALF_H || b.y < TOP) {
        if (b.p1 == 2 && hooks().get_angle_to_enemy)
            b.angle = hooks().get_angle_to_enemy(b.x, b.y, b.p2, b.angle);
        else if (b.p1 == 1 && hooks().get_angle_to_my_ship)
            b.angle = hooks().get_angle_to_my_ship(b.x, b.y);
    }
}

// bulletFunc17 @0x50db94 (184B) —— 限速追踪（单帧 ≤ A0）
inline void bulletFunc17(BulletMotion& b, float dt) {
    (void)dt;
    float target = hooks().get_angle_to_my_ship
        ? hooks().get_angle_to_my_ship(b.x, b.y) : b.angle;
    if ((int)b.t == b.life) { b.prevAngle = target; return; }    // 0x50dbe0
    if ((int)b.t > b.life) {
        float d = target - b.prevAngle;
        if (d >  b.angleInit) target = b.prevAngle + b.angleInit;
        if (d < -b.angleInit) target = b.prevAngle - b.angleInit;
        b.angle = target;
        b.prevAngle = b.angle;
    }
}

// bulletFunc15 @0x50da6c (140B) —— 接近自机 → 加速
inline void bulletFunc15(BulletMotion& b, float dt) {
    (void)dt;
    if (b.p2 != 0) return;
    float sx = 0, sy = 0;
    if (hooks().get_my_ship_pos) hooks().get_my_ship_pos(sx, sy);
    float dx = b.x - sx, dy = b.y - sy;
    float d2 = dx*dx + dy*dy;
    if (d2 <= (float)b.life) {                                   // 阈值：life 位模式比较（实测）
        b.p2 = 1; b.speed = b.spdInit2; b.speedCap = b.xaccCap;
    }
}

// bulletFunc16 @0x50daf8 (156B) —— 接近敌机 → 触发
inline void bulletFunc16(BulletMotion& b, float dt) {
    (void)dt;
    if (b.p2 != 0) return;
    if ((int)b.t <= b.bulletLife) return;                        // T <= N → 不触发
    float ex = 0, ey = 0;
    if (hooks().get_enemy_pos) hooks().get_enemy_pos(b.p1, ex, ey);
    float dx = b.x - ex, dy = b.y - ey;
    if (dx*dx + dy*dy < (float)b.life) b.p2 = 1;
}

// bulletFunc24 @0x50e100 (148B) —— 接近自机 → 重置角/上限
inline void bulletFunc24(BulletMotion& b, float dt) {
    (void)dt;
    float sx = 0, sy = 0;
    if (hooks().get_my_ship_pos) hooks().get_my_ship_pos(sx, sy);
    float dx = b.x - sx, dy = b.y - sy;
    float d2 = dx*dx + dy*dy;
    if (d2 < b.xacc /*占位阈值*/ && b.p2 == 0) {
        b.angle = b.angleInit;
        b.p2 = 1;
        b.speedCap = b.xaccCap;
    }
}

// bulletFunc21 @0x50dfb8 (96B) —— 全局状态==3 → v = life*0.03
inline void bulletFunc21(BulletMotion& b, float dt) {
    (void)dt;
    int st = hooks().get_global_state ? hooks().get_global_state() : -1;
    if (st == 3) b.speed = (float)b.life * 0.03f;                // double 0.03 @0xacee20
    else         b.speed = b.spdInit2;
}

// bulletFunc22 @0x50e018 (96B) —— 全局状态==4 → v = life*0.1
inline void bulletFunc22(BulletMotion& b, float dt) {
    (void)dt;
    int st = hooks().get_global_state ? hooks().get_global_state() : -1;
    if (st == 4) b.speed = (float)b.life * 0.1f;                 // double 0.1 @0xac73d8
    else         b.speed = b.spdInit2;
}

// bulletFunc23 @0x50e078 (136B) —— 全局状态==5 → v = life*0.1（+换皮）
inline void bulletFunc23(BulletMotion& b, float dt) {
    (void)dt;
    int st = hooks().get_global_state ? hooks().get_global_state() : -1;
    if (st == 5) b.speed = (float)b.life * 0.1f;
    else         b.speed = b.spdInit2;
}

// bulletFunc25 @0x50e194 (84B) —— v = V1 + XC*sin(A0 + X*t)
inline void bulletFunc25(BulletMotion& b, float dt) {
    (void)dt;
    float arg = b.angleInit + b.xacc * b.t;                      // A0 + X*t
    b.speed = b.spdInit2 + b.xaccCap * my_sin_deg(arg);          // V1 + XC*sin(arg)
}

// bulletFunc43 @0x50f500 (128B) —— v = V1 + X*sin(XC*t)；hitDis = pf2*v
inline void bulletFunc43(BulletMotion& b, float dt) {
    (void)dt;
    if ((int)b.p2 >= b.life) {
        float s = b.spdInit2 + b.xacc * my_sin_deg(b.xaccCap * b.t);
        b.speed  = s;
        b.hitDis = b.pf2 * s;
    }
}

// bulletFunc40 @0x50f08c (108B) —— aa = A0 + F98*sin(V1 + PA*t)
inline void bulletFunc40(BulletMotion& b, float dt) {
    (void)dt;
    if ((int)b.t < b.life) {
        float arg = b.spdInit2 + b.prevAngle * b.t;              // V1 + PA*t
        b.angleAcc = b.angleInit + b.f598 * my_sin_deg(arg);     // A0 + F98*sin(arg)
    }
}

// bulletFunc14 @0x50d950 (284B) —— pf1 随 life 线性变化；hitDis = pf2*pf1
//   注：0.1 为 double 常量（@0xac73d8）。
inline void bulletFunc14(BulletMotion& b, float dt) {
    (void)dt;
    const float K = 0.1f;
    if ((int)b.t >= 1 && b.p2 != 0) {
        float d = (float)b.life * K / (float)b.p1;               // life*0.1/p1
        b.pf1 = (b.pf1 + d) * K;
    }
    if (b.p2 != 0) {
        if (b.life < b.p2) { float n = b.pf1 * K; if (n < b.pf1) b.pf1 = n; }
        if (b.life > b.p2) { float n = b.pf1 * K; if (n > b.pf1) b.pf1 = n; }
    }
    b.hitDis = b.pf2 * b.pf1;
}

// bulletFunc35 @0x50ec24 (300B) —— 接近自机 → 指数减速 + 朝自机
inline void bulletFunc35(BulletMotion& b, float dt) {
    (void)dt;
    if (b.p2 != 1) {
        float sx = 0, sy = 0;
        if (hooks().get_my_ship_pos) hooks().get_my_ship_pos(sx, sy);
        float dx = b.x - sx, dy = b.y - sy;
        if (dx*dx + dy*dy <= (float)b.life) b.p2 = 1;            // 阈值
    }
    if (b.p2 == 1) {
        b.speedCap = 0.0f;                                       // 0x50eca8
        b.accel    = b.speed * (-0.1f);                          // double -0.1 @0xace3a0
    }
    if ((int)b.t == b.p1) {
        if (b.angleInit > 1.0f && hooks().get_angle_to_my_ship)
            b.angle = hooks().get_angle_to_my_ship(b.x, b.y);
        b.speed = b.spdInit2; b.speedCap = b.xaccCap;
    }
}

// bulletFunc39 @0x50ef00 (396B) —— 状态机：线性减速 va = -v/life
inline void bulletFunc39(BulletMotion& b, float dt) {
    (void)dt;
    if (b.p2 == 0) return;
    if (b.p2 == 1) {
        b.speedCap = 0.0f;                                       // 0x50efd0
        b.accel    = -(b.speed / (float)b.life);                 // fdiv+fneg 0x50efd8
        b.angle    = b.angleInit;
    }
    if (b.p1 == 2) { /* setBulletType(0x2a) */ }
    if (b.p1 == 3) { /* setBulletType(0x1e) */ }
    if (b.p1 == 4) { /* setBulletType(0x25) */ }
}

// bulletFunc32 @0x50ea08 (156B) —— 按 P1 奇偶镜像角
inline void bulletFunc32(BulletMotion& b, float dt) {
    (void)dt;
    if ((int)b.t < 0 || (int)b.t != 0) return;
    float a0 = b.angleInit;
    b.speed = b.spdInit2; b.speedCap = b.xaccCap;
    if (b.p1 & 1) b.angle = 360.0f - (a0 - b.p2) + 90.0f;        // 360 - a + 90（镜像）
    else          b.angle = a0 - b.p2;
}

// bulletFunc46 @0x50f654 (112B) —— 每 N 帧把角 ±90
inline void bulletFunc46(BulletMotion& b, float dt) {
    (void)dt;
    if ((int)b.t == 0) return;
    if ((int)b.t == b.bulletLife) {
        b.angle += (b.life == 0) ? 90.0f : -90.0f;               // 表 @0xacee48 [90,-90]
        b.bulletLoop = b.bulletLoop;
        b.bulletLife = (int)b.t;
    }
}

// bulletFunc57 @0x50fd84 (124B) —— 在 A0±PA 之间三角波
inline void bulletFunc57(BulletMotion& b, float dt) {
    (void)dt;
    int st = hooks().get_global_state ? hooks().get_global_state() : -1;
    if (st == 0x17) {
        b.angle += b.f598;
        if (b.angle > b.angleInit + b.prevAngle) b.angle = b.angleInit + b.prevAngle;
    } else {
        b.angle -= b.f598;
        if (b.angle < b.angleInit - b.prevAngle) b.angle = b.angleInit - b.prevAngle;
    }
}

// bulletFunc58 @0x50fe00 (168B) —— 折返角
inline void bulletFunc58(BulletMotion& b, float dt) {
    (void)dt;
    if ((int)b.t != 0) return;
    if (b.p1 & 1) { b.angle += b.f598; }
    else          { b.angle -= b.f598; b.angleAcc = b.angleAcc - b.f598; }
    b.angle = 360.0f - b.angle + 90.0f;
}

// bulletFunc63 @0x510394 (60B) —— a=A0; aa=-F98
inline void bulletFunc63(BulletMotion& b, float dt) {
    (void)dt;
    if ((int)b.t == 0) { b.angle = b.angleInit; b.angleAcc = -b.f598; }
}

// bulletFunc77 @0x510dcc (188B) —— 状态 0x21/0x22 → 镜像角
inline void bulletFunc77(BulletMotion& b, float dt) {
    (void)dt;
    int st = hooks().get_global_state ? hooks().get_global_state() : -1;
    if (st == 0x21 || st == 0x22) {
        b.angle = 180.0f - b.angle;
        if (b.angle < 0.0f) b.angle += 360.0f;
    }
}

// bulletFunc78 @0x510e88 (104B) —— y<40 → 镜像角
inline void bulletFunc78(BulletMotion& b, float dt) {
    (void)dt;
    if (b.y < 40.0f) {                                           // @0xace180
        b.angle = 180.0f - b.angle;
        if (b.angle < 0.0f) b.angle += 360.0f;
    }
}

// bulletFunc61 @0x50ff78 (272B) —— 四方向（0/90/180/270）
inline void bulletFunc61(BulletMotion& b, float dt) {
    (void)dt;
    if ((int)b.t != 0) return;
    float sx = 0, sy = 0;
    if (hooks().get_my_ship_pos) hooks().get_my_ship_pos(sx, sy);
    float dx = b.x - sx, dy = b.y - sy;
    if (dx < 0.0f && dy < 0.0f)      b.angle = 90.0f;            // 0x42b40000
    else if (dx < 0.0f && dy >= 0.0f) b.angle = 180.0f;          // 0x43340000
    else if (dx >= 0.0f && dy >= 0.0f) b.angle = 270.0f;         // 0x43870000
    else                              b.angle = 0.0f;
    b.speed = b.spdInit2; b.speedCap = b.xaccCap;
}

// bulletFunc52 @0x50f9e4 (80B) —— 随机角
inline void bulletFunc52(BulletMotion& b, float dt) {
    (void)dt;
    if ((int)b.t != 0) return;
    b.angle = (float)my_random(0, b.p2);
}

// bulletFunc71 @0x510ac8 (176B) —— 状态==0x1e → 随机化初角/幅度
inline void bulletFunc71(BulletMotion& b, float dt) {
    (void)dt;
    int st = hooks().get_global_state ? hooks().get_global_state() : -1;
    if (st != 0x1e || (int)b.t != 0) return;
    b.angleInit = (float)my_random(0, b.p1);
    b.f598      = (float)my_random(0, b.p1) + (float)my_random(0, b.p1);
}

// bulletFunc72 @0x510b78 (68B) —— 状态==0x1c → 速度重置
inline void bulletFunc72(BulletMotion& b, float dt) {
    (void)dt;
    int st = hooks().get_global_state ? hooks().get_global_state() : -1;
    if (st == 0x1c) { b.speed = b.spdInit2; b.speedCap = b.xaccCap; }
}

// bulletFunc73 @0x510bbc (128B) —— pf1 = A0 + F98*sin(V1 + PA*P2); hitDis = pf2*pf1
inline void bulletFunc73(BulletMotion& b, float dt) {
    (void)dt;
    float arg = b.spdInit2 + b.prevAngle * (float)b.p2;
    b.pf1 = b.angleInit + b.f598 * my_sin_deg(arg);
    b.hitDis = b.pf2 * b.pf1;
}

// bulletFunc81 @0x511d48 (256B) —— pf1 = A0 + F98*sin(PA*P2)；正弦半径
inline void bulletFunc81(BulletMotion& b, float dt) {
    (void)dt;
    float s = my_sin_deg(b.prevAngle * (float)b.p2);
    b.pf1 = b.angleInit + b.f598 * s;
    b.speed = b.spdInit2 + b.xaccCap * s;
    b.hitDis = b.pf2 * b.pf1;
}

// bulletFunc29 @0x511384 (292B) —— 分段重置 + 合成 vx/vy
inline void bulletFunc29(BulletMotion& b, float dt) {
    (void)dt;
    if ((int)b.t < b.life) {
        b.vx = b.prevAngle * my_cos_deg(b.angle);
        b.vy = b.prevAngle * my_sin_deg(b.angle);
    }
    if ((int)b.t == b.life) { b.angle = b.angleInit; b.vx = 0.0f; }
    if ((int)b.t >= b.life) {
        b.angle = b.angleInit; b.speed = b.spdInit2;
        b.angleAcc = b.f598;   b.speedCap = b.xaccCap;
    }
    b.integrate_velocity();
}

// bulletFunc47 @0x5114a8 (124B) —— 每帧重算 vx/vy（直线）
inline void bulletFunc47(BulletMotion& b, float dt) {
    (void)dt;
    if ((int)b.t == 0) b.vx = 0.0f;
    b.integrate_velocity();
}

// bulletFunc30 @0x50e224 (628B) —— 按 bulletType 换贴图 + hitDis
inline void bulletFunc30(BulletMotion& b, float dt) {
    (void)dt;
    if ((int)b.t != b.p2) return;
    if (b.bulletType == 0x2a) { /* getSpriteFrameByName("...png") */ }
    else if (b.bulletType == 0x25) { /* getSpriteFrameByName("...png") */ }
    b.hitDis = 4.0f;                                             // 0x40800000
}

// bulletFunc41 @0x50f0f8 (680B) —— 接近自机 → 变身/发射
inline void bulletFunc41(BulletMotion& b, float dt) {
    (void)dt;
    if (b.p2 != 0) return;
    float sx = 0, sy = 0;
    if (hooks().get_my_ship_pos) hooks().get_my_ship_pos(sx, sy);
    float dx = b.x - sx, dy = b.y - sy;
    if (dx*dx + dy*dy > (float)b.life) return;                   // 阈值
    if (b.p1 > 6) return;
    b.p2 = 1;
    if (hooks().get_angle_to_my_ship) b.angle = hooks().get_angle_to_my_ship(b.x, b.y);
    b.speed = b.spdInit2; b.speedCap = b.xaccCap;
    b.hitDis = 7.0f;    // 0x40e00000
    b.pf2    = 3.0f;    // 0x40400000
    b.rotateFlg = 0;
}

// bulletFunc42 @0x50f3a0 (352B) —— 接近敌机 → 变身
inline void bulletFunc42(BulletMotion& b, float dt) {
    (void)dt;
    if (b.p2 != 0) return;
    float ex = 0, ey = 0;
    if (hooks().get_enemy_pos) hooks().get_enemy_pos(b.p1, ex, ey);
    float dx = b.x - ex, dy = b.y - ey;
    if (dx*dx + dy*dy >= (float)b.life) return;
    b.p2 = 1;
    if (b.p1 == 1) { b.hitDis = 2.0f; b.rotateFlg = 2; }         // 0x40000000
    b.angle = b.angleInit;
    b.speedCap = b.xaccCap;
}

// bulletFunc48 @0x50f6c4 (316B) —— 接近瞄准 + 换皮
inline void bulletFunc48(BulletMotion& b, float dt) {
    (void)dt;
    if ((int)b.t < b.life) return;
    if ((int)b.t == b.life) {
        if (b.p2 == 1) b.angle = b.angleInit;
        else if (hooks().get_angle_to_my_ship)
            b.angle = hooks().get_angle_to_my_ship(b.x, b.y);
        b.speedCap = b.xaccCap;
        if (b.p1 == 1) { /* setBulletType(0xce) */ }
        if (b.p1 == 0) { /* setBulletType(0x25) */ }
    }
}

// bulletFunc54 @0x50faf4 (428B) —— 按 life 阶段换皮
inline void bulletFunc54(BulletMotion& b, float dt) {
    (void)dt;
    if ((int)b.t != 0) return;
    if (b.life == 0) { /* setBulletType 组 A */ }
    if (b.life == 1) { /* setBulletType 组 B */ }
    if (b.life == 2) { /* setBulletType 组 C */ }
    b.speed = b.spdInit2; b.speedCap = b.xaccCap;
}

// bulletFunc65 @0x510498 (172B) —— 按 life/P2 换皮
inline void bulletFunc65(BulletMotion& b, float dt) {
    (void)dt;
    if ((int)b.t != 0) return;
    if (b.life == 0 || b.life == 1) {
        if (b.p2 == 0) { /* setBulletType(0x2e/0x37) */ }
        else           { /* setBulletType(0x33/0x3c) */ }
    }
}

// bulletFunc70 @0x51079c (812B) —— 出屏 → 瞄准 + 换皮
inline void bulletFunc70(BulletMotion& b, float dt) {
    (void)dt;
    const float HALF_W = 320.0f, HALF_H = 480.0f;
    if (b.x > HALF_W || b.y < 0.0f || b.y > HALF_H) {
        float d = 180.0f - b.angle;                              // fsub s10,s1,s0
        b.angle = d + (float)my_random(0, b.p2);
        if (b.p1 <= 4 && hooks().get_angle_to_my_ship)
            b.angle = hooks().get_angle_to_my_ship(b.x, b.y);
        b.speed = b.spdInit2; b.speedCap = b.xaccCap;
    }
}

// bulletFunc76 @0x510d30 (156B) —— y>480 → 瞄准 + 换皮
inline void bulletFunc76(BulletMotion& b, float dt) {
    (void)dt;
    if (b.y > 480.0f) {
        b.angle = 360.0f - b.angle;
        b.angle += (float)my_random(0, b.p2);
        b.speed = b.spdInit2; b.speedCap = b.xaccCap;
        if (b.p1 == 0) { /* setBulletType(0x11) */ }
    }
}

// bulletFunc80 @0x510f28 (204B) —— 距离落在 [XC-V1, XC+V1] → P2=1
inline void bulletFunc80(BulletMotion& b, float dt) {
    (void)dt;
    if (b.p2 != 0 || (int)b.t != 0 || b.life > 3) return;
    float lo = b.xaccCap - b.spdInit2;
    float hi = b.xaccCap + b.spdInit2;
    if (b.x > lo && b.x < hi) b.p2 = 1;                          // 近似（实测为分量比较）
}

// bulletFunc84 @0x510ff4 (176B) —— 接近自机 → 换皮/触发
inline void bulletFunc84(BulletMotion& b, float dt) {
    (void)dt;
    if (b.p2 == 1) {
        int t = b.p1; if (t > 255) t = 255;                      // csel clamp
        (void)t; /* setBulletType(t) */
    }
    if (b.p2 == 1) {
        float sx = 0, sy = 0;
        if (hooks().get_my_ship_pos) hooks().get_my_ship_pos(sx, sy);
        float dx = b.x - sx, dy = b.y - sy;
        if (dx*dx + dy*dy <= b.spdInit2) b.p2 = 1;
    }
}

// bulletFunc85 @0x5110a4 (616B) —— 接近自机 → 换 hitDis
inline void bulletFunc85(BulletMotion& b, float dt) {
    (void)dt;
    float sx = 0, sy = 0;
    if (hooks().get_my_ship_pos) hooks().get_my_ship_pos(sx, sy);
    float dx = b.x - sx, dy = b.y - sy;
    if (dx*dx + dy*dy < (float)b.life) {
        if (b.bulletType == 0x25) b.hitDis = 20.0f;              // fmov #20.0
        else if (b.bulletType == 0x2a) b.hitDis = 4.0f;          // fmov #4.0
    }
}

// ---------------------------------------------------------------------------
// func27 @0x50cd54 —— p1==1 时关掉碰撞；否则 hitDis *= life*0.01（double 中间量）
//   getHitDis/setHitDis = vtable+0x6d0/+0x6d8；常量 0xac52f0 = 0.01
// ---------------------------------------------------------------------------
inline void bulletFunc27(BulletMotion& b, float) {
    if (b.p1 == 1) { b.hitHantei = false; return; }
    b.hitDis = (float)((double)b.life * 0.01 * (double)b.hitDis);
}

// ---------------------------------------------------------------------------
// func44 @0x50f580 —— p2==0 && life==1 && pos.y < p1 时整体重置：
//   angle=A0；{angleAcc,f56c,speed,spdAcc} ← {f598,prevAngle,spdInit2,xacc}（一条 16B 拷贝）；
//   speedCap = xaccCap；p2++
// ---------------------------------------------------------------------------
inline void bulletFunc44(BulletMotion& b, float) {
    if (b.p2 != 0) return;
    if (b.life != 1) return;
    if (!(b.y < (float)b.p1)) return;          // getPosition().y < p1
    b.angle    = b.angleInit;
    b.angleAcc = b.f598;
    b.f56c     = b.prevAngle;
    b.speed    = b.spdInit2;
    b.accel    = b.xacc;
    b.speedCap = b.xaccCap;
    b.p2       = b.p2 + 1;
}

// ---------------------------------------------------------------------------
// func45 @0x50f600 —— p2==0 时：setOpacity(life)、setScale(spdInit2)、p2=1
// ---------------------------------------------------------------------------
inline void bulletFunc45(BulletMotion& b, float) {
    if (b.p2 != 0) return;
    b.opacity = b.life;
    b.scale   = b.spdInit2;
    b.p2      = 1;
}

// bulletFunc3 @0x51130c —— vx/vy 叠加 0.0163333·(cos life, sin life)，角度制。
// 判别依据：update 体内 0x50c9b8..0x50c9c8 装了同一个 double 常量 0x3F90B9AF72015D87
// 并写回 +0x57c(vx)/+0x580(vy) ⇒ 该函数被**内联**进 update 的比较链（cmp #0x3 @0x50c98c），
// 逐帧执行，不是死代码。乘加在原版里走 double（fcvtl→fmul→fcvtn），故此处也先转 double。
inline void bulletFunc3(BulletMotion& b, float) {
    constexpr double kDrift = 0x1.0B9AF72015D87p-6;   // 0.01633334…
    const double c = (double)my_cos_deg((float)b.life);
    const double s = (double)my_sin_deg((float)b.life);
    b.vx += (float)(kDrift * c);
    b.vy += (float)(kDrift * s);
}

// ===========================================================================
// 分派表命中、但作用在「位置 / 透明度」而非 angle·speed 的 5 个函数
// （表 @0xb0a88e，项 = uint16 × 4 + 基址 0x50b514，索引 funcNo-1）
//   bulletFunc6  @0x50d2e8 : X 轴环绕，越界换边并 life--
//   bulletFunc8  @0x50d574 : X/Y 双轴环绕（Y 下界 40-h）
//   bulletFunc28 @0x50e1e8 : alpha = max(255 - p2·(t1-p1), life)
//   bulletFunc36 @0x50ed50 : alpha = 128+128·sin(p1·(t1-life)+90)，hitHantei = alpha>127
//   bulletFunc79 @0x510ef0 : alpha = min(p2·(t1-p1), life)
// 三处 alpha 最终都走 vtable+0x490 = cocos2d::Node::setOpacity(uint8)（.rela 已解出）。
// 注：halfW/halfH 是贴图半径，原版用 getContentSize()（= 2·halfW/2·halfH）做环绕边界。
// ===========================================================================
inline void bulletFunc6(BulletMotion& b, float) {
    if (b.life < 1) return;                        // 0x50b6a8: cmp life,#1; b.lt tail
    const float w = b.halfW * 2.0f;                // getContentSize().width
    if (b.x > b.halfW + w) { b.x = -w;           b.life -= 1; }
    if (b.x < -w)          { b.x = b.halfW + w;  b.life -= 1; }
}

inline void bulletFunc8(BulletMotion& b, float) {
    if (b.life < 1) return;
    const float w = b.halfW * 2.0f;
    const float h = b.halfH * 2.0f;
    if (b.x > b.halfW + w)  { b.x = -w;            b.life -= 1; }
    if (b.x < -w)           { b.x = b.halfW + w;   b.life -= 1; }
    if (b.y > b.halfH + h)  { b.y = 40.0f - h;     b.life -= 1; }   // 0xace180 = 40.0
    if (b.y < 40.0f - h)    { b.y = b.halfH + h;   b.life -= 1; }
}

inline void bulletFunc28(BulletMotion& b, float) {
    const int d = (int)b.t - b.p1;
    int a = 0;
    if (d >= 0) {
        a = 255 - b.p2 * d;                        // msub w8, p2, d, #0xff
        if (a < b.life) a = b.life;                // csel → max(…, life)
    }
    b.opacity = a;
}

inline void bulletFunc79(BulletMotion& b, float) {
    const int d = (int)b.t - b.p1;
    int a = 0;
    if (d >= 0) {
        a = b.p2 * d;
        if (a > b.life) a = b.life;                // csel → min(…, life)
    }
    b.opacity = a;
}

inline void bulletFunc36(BulletMotion& b, float) {
    const int d = (int)b.t - b.life;
    if (d < 0) { b.opacity = 255; return; }        // 0x50b908: orr w1,wzr,#0xff
    const int s = (int)(my_sin_deg((float)(b.p1 * d + 90)) * 128.0f);  // fcvtzs #7
    int a = s + 128;                               // add #0x80
    if (a >= 255) a = 255;                         // csel lt
    const int c = (a < 0) ? 0 : a;                 // bic w9,w8,w8,asr#31
    b.hitHantei = (c > 127);                       // 0x548 = alpha > 127
    b.opacity   = (c < 128) ? c : a;
}

// ===========================================================================
// 以下为「未能确定」或高度复杂者，提供占位实现（真实公式见 disasm 文档 §5）
//   bulletFunc27 @0x50cd54 : P1==1 → hitHantei=0；否则返回 L*0.01*t
//   bulletFunc20 @0x50dde0 : 边界分段（80/240/340/320 带）
//   bulletFunc26/33/66/75/82/83 : 参数方程/摆线（cos/sin 多路叠加）
//   bulletFunc13 : 最近点瞄准；bulletFunc19 : v*=0.6(魔数)；bulletFunc53 : 常量存疑
// ===========================================================================
inline void bulletFunc_unknown(BulletMotion& b, float dt) { (void)b; (void)dt; }

} // namespace touhou
#endif // BULLET_FUNCS_REIMPL_HPP
