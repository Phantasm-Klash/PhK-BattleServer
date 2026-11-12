#include "phk/battle/danmaku/emitter.hpp"
#include "phk/battle/danmaku/bullet_size.hpp"  // 原版 per-kind hitDis/artScale（实测）

#include <cstddef>

namespace phk::battle::danmaku {

std::vector<double> RadialAngles(double base_angle_deg, double spread_deg, int count) {
    std::vector<double> angles;
    if (count <= 0) {
        return angles;
    }
    const double step = 360.0 / static_cast<double>(count);  // fdiv s12,s0,s3
    double start = base_angle_deg - spread_deg;              // fsub s10,s1,s2
    while (start < 0.0) {                                    // while (s10<0) s10+=360
        start += 360.0;
    }
    angles.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        angles.push_back(NormalizeDeg(start + step * static_cast<double>(i)));
    }
    return angles;
}

std::vector<BulletMotion> EmitRadial(
    const SpawnCommand& cmd,
    const BulletTemplate& tmpl,
    const EmitContext& ctx,
    const DifficultyScale& difficulty
) {
    std::vector<BulletMotion> bullets;
    const int count = ScaleCount(cmd.count, difficulty.density_mul);  // 密度缩放（恒等 = 原值）
    if (count <= 0) {  // cbz w8, return
        return bullets;
    }

    // -1.0 哨兵 → 发射者坐标。
    const double spawn_x = (cmd.spawn_x == kSpawnFollowEmitter) ? ctx.emitter_x : static_cast<double>(cmd.spawn_x);
    const double spawn_y = (cmd.spawn_y == kSpawnFollowEmitter) ? ctx.emitter_y : static_cast<double>(cmd.spawn_y);

    const double step = 360.0 / static_cast<double>(count);
    double start = static_cast<double>(cmd.base_angle_deg) - static_cast<double>(cmd.spread_or_speed);
    while (start < 0.0) {
        start += 360.0;
    }

    const int kind = (tmpl.bullet_kind != 0) ? tmpl.bullet_kind : cmd.bullet_type;
    // 速度缩放（恒等倍率下 x·1.0 == x，逐字节不变）。
    const double speed = tmpl.speed * difficulty.speed_mul;
    const double speed_cap = tmpl.speed_cap * difficulty.speed_mul;

    bullets.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        BulletMotion b;
        b.x = spawn_x;
        b.y = spawn_y;
        b.bullet_kind = kind;
        b.bullet_func_no = tmpl.bullet_func_no;
        b.angle = NormalizeDeg(start + step * static_cast<double>(i));

        // 构造快照（对应 EnemyBullet ctor 参数）。
        b.angle_init = tmpl.angle_init;
        b.speed_init = (tmpl.speed != 0.0) ? speed : 0.0;
        b.angle_spd = tmpl.angle_spd;
        b.angle_acc = tmpl.angle_acc;
        b.f56c = tmpl.f56c;
        b.speed = speed;
        b.accel = 0.0;
        b.speed_cap = speed_cap;
        b.f598 = tmpl.f598;
        b.prev_angle = tmpl.prev_angle;
        b.spd_init2 = speed;
        b.xacc = tmpl.xacc;
        b.xacc_cap = tmpl.xacc_cap;
        b.pf1 = 1.0;
        b.pf2 = tmpl.pf2;
        b.bullet_life = tmpl.bullet_life;
        b.life = tmpl.life;
        b.p1 = tmpl.p1;
        b.p2 = tmpl.p2;
        b.delay = tmpl.delay;
        b.rotate_flg = 0;
        b.hit_hantei = true;
        // 碰撞半径兜底：运动函数（func 0 等）不写 hit_dis 时用它。
        // ★ 2026-11-13：原版 `EnemyBullet::setBulletType` @0x512140 按弹种写
        //   对象 +0x54c 的 hitDis（216 个 kind 逐分支解出，见 bullet_size.hpp）：
        //     普通弹 2.4px / heart·knife 类 8.0px / 大弹 14.14·66·86px。
        //   旧实现用模板 pf2·pf1（chain_node 硬编码 pf2=4.0）让**所有弹统一
        //   4px**，既与原版不符，也让客户端按 radius 归一化时尺寸全错。
        //   优先用原版 per-kind 值；仅当该 kind 未收录（kind<=0 等）才回退
        //   pf2·pf1，保留 legacy 路径行为。
        if (b.hit_dis == 0.0) {
            if (kind > 0) {
                const float original = BulletHitDisForKind(kind);
                b.hit_dis = (original > 0.0f) ? static_cast<double>(original)
                                              : b.pf2 * b.pf1;
            } else if (b.pf2 > 0.0) {
                b.hit_dis = b.pf2 * b.pf1;
            }
        }

        bullets.push_back(b);
    }
    return bullets;
}

}  // namespace phk::battle::danmaku
