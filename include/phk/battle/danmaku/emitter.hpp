#pragma once

#include <vector>

#include "phk/battle/danmaku/bullet_motion.hpp"
#include "phk/battle/danmaku/difficulty_table.hpp"  // DifficultyScale（P4 难度缩放）
#include "phk/battle/danmaku/spawn_command.hpp"

// 整圆/扇形径向发射器：复刻 Enemy::createBullets1 @0x37bacc 的已解出语义。
//
// 已解出（实测）：
//   * count == 0 → 不发射。
//   * spawn_x/spawn_y == -1.0f（哨兵）→ 用发射者坐标。
//   * angleStep = 360.0f / count；起始角 = baseAngle - spread，归一化到 [0,360)。
// 未完全解出（推断）：createBullets1 把其余 20 个参数透传给 spriteWithFile，
//   但 spriteWithFile → EnemyBullet 字段的逐参数映射尚未解出。因此这里把
//   「运动学参数」显式放在 BulletTemplate 里，与角度分布解耦。
namespace phk::battle::danmaku {

// 发射者坐标（用于 -1.0 哨兵）。
struct EmitContext {
    double emitter_x = 0.0;
    double emitter_y = 0.0;
};

// 运动学模板：spawn 时写入子弹的快照字段（A0/V0/F98/PA/V1/X/XC）。
struct BulletTemplate {
    int bullet_kind = 0;        // 覆盖 cmd.bullet_type（0 = 用 cmd 的）
    int bullet_func_no = 0;     // 运动函数编号（1..85 语义，0 = 直线初始化）
    double speed = 0.0;         // 初始速度
    double angle_init = -1.0;   // A0（-1 = 使用计算出的角度）
    double angle_spd = 0.0;     // +0x564
    double angle_acc = 0.0;     // +0x568
    double f56c = 0.0;          // +0x56c
    double speed_cap = 0.0;     // +0x578
    double f598 = 0.0;          // +0x598
    double prev_angle = 0.0;    // +0x59c
    double xacc = 0.0;          // +0x5a4
    double xacc_cap = 0.0;      // +0x5a8
    double pf2 = 0.0;           // +0x5c8
    int bullet_life = 0;        // +0x588 N
    int life = 0;               // +0x5b8
    int p1 = 0;                 // +0x5bc
    int p2 = 0;                 // +0x5c0
    int delay = 0;              // +0x5ac
};

// 均匀铺满一圈、以 base_angle_deg 为中心对称分布的角序列（度，[0,360)）。
[[nodiscard]] std::vector<double> RadialAngles(double base_angle_deg, double spread_deg, int count);

// 执行一次齐射，返回生成的子弹（已应用运动学模板，未做首帧 update）。
// difficulty：P4 难度缩放（speed/density 两项在此生效；interval 由调用方的
// 调度周期负责）。默认恒等 —— 难度 1 / 默认 ruleset 逐字节不变。
[[nodiscard]] std::vector<BulletMotion> EmitRadial(
    const SpawnCommand& cmd,
    const BulletTemplate& tmpl,
    const EmitContext& ctx,
    const DifficultyScale& difficulty = DifficultyScale{}
);

}  // namespace phk::battle::danmaku
