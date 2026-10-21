#include "phk/battle/danmaku/chain_split.hpp"

#include <algorithm>

namespace phk::battle::danmaku {

std::vector<ChainSplitChild> ComputeChainSplitChildren(
    const BulletMotion& host,
    std::uint64_t seed,
    std::uint64_t tick,
    std::uint32_t pattern_index,
    std::uint32_t host_spawn_index,
    int max_children
) {
    std::vector<ChainSplitChild> children;
    // 请求判定（ChainSplitRequested）是调用方职责：本函数只对「已判定
    // 请求分裂」的宿主计算参数。这里只保留预算钳制。
    if (max_children <= 0) {
        return children;
    }

    const int count = std::min(kChainSplitChildrenPerHost, max_children);
    children.reserve(static_cast<std::size_t>(count));

    // 与宿主自身每帧运动 RNG 相互独立：宿主运动 RNG 在 AdvanceBullets 里
    // 每帧新建（counter 从 0），这里也新建一个（counter 从 0），两者输入
    // 相同但消耗互不影响——分裂不改变宿主运动，宿主运动也不消耗分裂序列。
    DeterministicRng rng(seed, tick, pattern_index, host_spawn_index);
    for (int i = 0; i < count; ++i) {
        // 固定消耗序列：每颗 2 次 RandomInt(0,9999)，先角度后速度。
        const double u_angle = static_cast<double>(rng.RandomInt(0, 9999)) / 10000.0;
        const double u_speed = static_cast<double>(rng.RandomInt(0, 9999)) / 10000.0;

        ChainSplitChild child;
        child.angle_deg = NormalizeDeg(
            host.angle + static_cast<double>(i) * kChainSplitAngleStepDeg +
            (u_angle - 0.5) * kChainSplitAngleJitterDeg
        );
        child.speed = host.speed * (
            kChainSplitChildSpeedLo +
            (kChainSplitChildSpeedHi - kChainSplitChildSpeedLo) * u_speed
        );
        child.spawn_index = host_spawn_index + kChainSplitSpawnIndexBase +
                            static_cast<std::uint32_t>(i);
        children.push_back(child);
    }
    return children;
}

BulletMotion MakeChainSplitChildMotion(const BulletMotion& host, const ChainSplitChild& child) {
    BulletMotion b;
    // 位置 = 宿主当前位置（分裂发生在宿主本帧 update 之后）。
    b.x = host.x;
    b.y = host.y;
    // 弹种/运动函数/触发帧继承宿主：子子弹 func_no 仍为 31，于是它也会在
    // 自己的 t==p1 帧再次请求分裂——这就是「链式」。代数钳制在宿主侧
    // （generation < kChainSplitMaxGeneration），子子弹到叶子代自然停止。
    b.bullet_kind = host.bullet_kind;
    b.bullet_func_no = host.bullet_func_no;
    b.p1 = host.p1;
    // 运动学：速度由种子推导；速度上限/加速度不继承（直线飞出）。
    b.angle = child.angle_deg;
    b.speed = child.speed;
    b.speed_init = child.speed;
    b.spd_init2 = child.speed;
    b.angle_init = -1.0;
    b.speed_cap = 0.0;
    b.accel = 0.0;
    b.angle_acc = 0.0;
    b.angle_spd = 0.0;
    b.f56c = 0.0;
    b.f598 = 0.0;
    b.prev_angle = 0.0;
    b.xacc = 0.0;
    b.xacc_cap = 0.0;
    b.pf1 = 1.0;
    b.pf2 = 0.0;
    b.bullet_life = 0;
    b.life = 0;
    b.p2 = 0;
    b.delay = 0;
    b.vanish_delay = 0;
    b.rotate_flg = 0;
    // 命中半径：宿主已带则继承，否则兜底默认值。
    b.hit_dis = (host.hit_dis > 0.0) ? host.hit_dis : kChainSplitChildHitDis;
    b.hit_hantei = true;
    b.IntegrateVelocity();
    return b;
}

}  // namespace phk::battle::danmaku
