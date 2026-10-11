#pragma once

#include <cmath>
#include <cstdint>

#include "phk/battle/danmaku/bullet_kind.hpp"

// 千夜追忆 1.40 EnemyBullet 运动学（libMyGame.so 逆向），仅用于自研玩法。
//
// 本模块是 rev/patterns/bullet_funcs_reimpl.hpp 的「项目风格移植」：
//   * 角度单位 = 度（degree）。原版 MyMathUtil::sin/cos 内部 ×2π/360 再调 libm。
//   * 每帧只做运动学更新（对应 EnemyBullet::update @0x50b0b8）；位置积分
//     pos += (vx,vy) 由宿主（boss_race）完成，与原版分工一致。
//   * 参数在 spawn 时由 SpawnCommand 确定；bulletFunc 只负责覆写与事件。
//   * 原版 bulletFunc 本身不含 RNG；只有「分裂/随机角」族经 my_random 引入随机，
//     这里用 (match_seed, tick, pattern_index, spawn_index) 哈希的确定性 RNG 取代，
//     禁用系统时间/帧率。
//
// 单位与坐标：本模块内部使用「像素」浮点（原版语义）；服务端权威坐标是整数
// milli（1/1000 px），由 boss_race 在边界处 llround 转换。
namespace phk::battle::danmaku {

constexpr double kPi = 3.14159265358979323846;

// 原版 MyMathUtil::sin/cos @0x5ab0c0/0x5ab0dc：入参为度。
[[nodiscard]] inline double SinDeg(double degrees) { return std::sin(degrees * (kPi / 180.0)); }
[[nodiscard]] inline double CosDeg(double degrees) { return std::cos(degrees * (kPi / 180.0)); }

// 目标查询钩子：替代原版 GameLayer::getMyShipPos / getEnemyPos / getAngleToMyShip
// / getAngleToEnemy / 全局状态 getter。服务端权威。
struct MotionTargets {
    double my_ship_x = 0.0;
    double my_ship_y = 0.0;
    bool has_my_ship = false;

    double enemy_x = 0.0;  // Boss / 发射者位置（BossRace 中 = 发射者）
    double enemy_y = 0.0;
    bool has_enemy = false;

    int global_state = -1;  // GameLayer 状态码；-1 = 未提供（状态机族退化为默认分支）
};

// 确定性 RNG：替代 MyMathUtil::random(int,int)。输入 (seed,tick,pattern,spawn)，
// 与原版 bulletFunc 的「无 RNG」主路径保持一致：只有显式调用时才消耗序列。
class DeterministicRng {
public:
    DeterministicRng(std::uint64_t seed, std::uint64_t tick, std::uint32_t pattern_index, std::uint32_t spawn_index);

    [[nodiscard]] std::uint32_t NextU32();
    // 返回 [a,b] 内整数（a>b 时自动交换），语义同 MyMathUtil::random。
    [[nodiscard]] int RandomInt(int a, int b);

private:
    std::uint64_t seed_;
    std::uint64_t tick_;
    std::uint32_t pattern_index_;
    std::uint32_t spawn_index_;
    std::uint32_t counter_ = 0;
};

// 子弹运动状态（对应 EnemyBullet 字段，见 bullet_funcs_disasm.md §2）。
struct BulletMotion {
    // ---- 核心运动状态 ----
    double x = 0.0;       // 位置（像素）
    double y = 0.0;
    double vx = 0.0;      // 每帧位移 X（+0x57c）
    double vy = 0.0;      // 每帧位移 Y（+0x580）
    double angle = 0.0;   // 当前角度（度）（+0x560）
    double speed = 0.0;   // 当前速度（+0x570）
    double accel = 0.0;   // 速度加速度 spdAcc（+0x574）
    double t = 0.0;       // 帧计时器 t1（+0x5dc），每帧自增
    int life = 0;         // 生命帧计数/阈值（+0x5b8）

    // ---- 构造参数 / 快照 ----
    double angle_spd = 0.0;    // +0x564 角度步长/容差
    double angle_acc = 0.0;    // +0x568 角加速度
    double f56c = 0.0;         // +0x56c 角度夹持参考
    double speed_cap = 0.0;    // +0x578 速度上限
    double angle_init = -1.0;  // +0x590 A0
    double speed_init = -1.0;  // +0x594 V0
    double f598 = 0.0;         // +0x598 正弦幅度
    double prev_angle = 0.0;   // +0x59c PA
    double spd_init2 = -1.0;   // +0x5a0 V1
    double xacc = 0.0;         // +0x5a4 X
    double xacc_cap = 0.0;     // +0x5a8 XC
    double pf1 = 1.0;          // +0x5c4
    double pf2 = 0.0;          // +0x5c8
    int bullet_kind = 0;       // +0x538 kEnemyBullet
    int rotate_flg = 0;        // +0x530
    int bullet_life = 0;       // +0x588 N（!=0 才做角积分）
    int bullet_loop = 0;       // +0x58c LP
    int delay = 0;             // +0x5ac
    int vanish_delay = 0;      // +0x5b0
    int p1 = 0;                // +0x5bc
    int p2 = 0;                // +0x5c0
    int bullet_func_no = 0;    // +0x5b4
    double hit_dis = 0.0;      // +0x54c 命中半径
    bool hit_hantei = true;    // +0x548 碰撞判定开关

    // 以 (angle,speed) 合成 vx/vy（update 默认尾段）。
    void IntegrateVelocity() {
        vx = speed * CosDeg(angle);
        vy = speed * SinDeg(angle);
    }
};

// 从子弹位置 (x,y) 指向 (tx,ty) 的角度（度，[0,360)）。
[[nodiscard]] double AngleToPoint(double x, double y, double tx, double ty);

// 归一化到 [0,360)。
[[nodiscard]] double NormalizeDeg(double degrees);

// 79 个 bulletFunc 的逐帧分派（对应 EnemyBullet::update 的间接跳转表）。
// 返回 true 表示该编号已实现，false 表示未实现（调用者应记录）。
bool ApplyBulletFunc(BulletMotion& b, const MotionTargets& targets, DeterministicRng& rng);

// 完整单帧更新：delay 倒计时 → 角积分 → bulletFunc 分派 → 合成 vx/vy。
// 不移动位置（宿主负责 pos += (vx,vy) 并递增 t）。
void UpdateBulletMotion(BulletMotion& b, const MotionTargets& targets, DeterministicRng& rng);

// 该编号是否在 79 个已知 bulletFunc 集合内。
[[nodiscard]] bool IsKnownBulletFunc(int func_no);

}  // namespace phk::battle::danmaku
