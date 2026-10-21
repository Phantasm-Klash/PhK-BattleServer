#pragma once

#include <cstdint>
#include <vector>

#include "phk/battle/danmaku/bullet_motion.hpp"

// 链式分裂（H 族 bulletFunc31「Boss 分裂/召唤」的宿主侧落地）。
//
// 背景：bullet_motion.cpp 的 Func31 只做状态标记（t==p1 && life<=0x1e 时置
// p2=1 / rotate_flg=2），原版语义即「子子弹生成交宿主」（见 Func31 注释
// [推断：子子弹生成交宿主]）——在链式分裂落地前，一颗子弹永远不会产生
// 第二颗。本模块把那件事补上，且保持三项硬约束：
//   1. 服务端权威：分裂只发生在 BossRaceSimulation::AdvanceBullets（权威
//      推进路径），客户端要么只插帧服务端快照，要么用本文件的同算法镜像
//      本地重推（.dem 重放路径）——两端算法逐字节一致。
//   2. 可从种子推导：子子弹的角度/速度/spawn_index 全部由
//      danmaku::DeterministicRng(match_seed, tick, pattern_index,
//      host_spawn_index) 推导，无系统时间/帧率/无序容器参与。
//   3. 密度可控：代数、每宿主子数、单 tick 每玩家分裂宿主预算三个上限，
//      加上宿主已有的 kBossRaceMaxBulletsPerPlayer(384) 每玩家总量钳制。
//      常量取值理由见各常量注释。
namespace phk::battle::danmaku {

// 代数上限：0 = pattern 库直生（宿主），1 = 第一代子子弹，2 = 第二代（叶子）。
// 取 2 的理由：单宿主最坏产出 = 8^1 + 8^2 = 72 颗子子弹；若放到 3 代则
// 最坏 8+64+512 = 584 颗/宿主，必然撞穿 384/玩家总量钳制并把该玩家的主
// 发射波次挤光（预算竞争下截断顺序虽仍确定，但视觉上主模式会消失）。
// 2 代下「一波 4 颗 func31 宿主」最坏 4×73 = 292 < 384，主发射不受饿死。
inline constexpr std::uint32_t kChainSplitMaxGeneration = 2;

// 每个宿主每次分裂的子子弹数（整环 45° 间隔）。取 8 的理由：45° 间隔是
// 原版召唤类 bulletFunc 的常见密度（8/16 向），且 8 颗/tick × 8 宿主预算
// = 64 颗新弹/玩家/tick，在 60Hz 下渲染与序列化均可承受。
inline constexpr int kChainSplitChildrenPerHost = 8;

// 单 tick 内每玩家允许分裂的宿主数上限（双人局全局 = 2×8 = 16 个宿主）。
// 取 8 的理由：与「一波齐射的弹数」同量级，即使某 tick 内同代宿主全部
// 到点（最坏 8×8 = 64 颗新弹/玩家/tick），也不会把单 tick 的子弹增量
// 放大到需要再设全局上限；同时防止 func31 宿主密集注入时一帧爆量。
inline constexpr std::uint32_t kChainSplitHostsPerPlayerPerTick = 8;

// 子子弹 spawn_index = host_spawn_index + base + ordinal。
// 取 0x10000 的理由：pattern 波次的 spawn_index 实际值远小于 65536
// （单波 count ≤ 几十），不会与直生子弹冲突；子子弹自身再分裂时
// （0x10000 + 0x10000）仍在 u32 空间内，链式不溢出。
inline constexpr std::uint32_t kChainSplitSpawnIndexBase = 0x10000u;

// 子子弹角度 = NormalizeDeg(host.angle + ordinal×45° + (u-0.5)×jitter)。
// 抖动 ±4°：足够打破完美重合（视觉分层），又不破坏环的均匀可读性。
inline constexpr double kChainSplitAngleStepDeg = 45.0;   // 360 / kChainSplitChildrenPerHost
inline constexpr double kChainSplitAngleJitterDeg = 8.0;  // ±(jitter/2)

// 子子弹速度 = host.speed × (lo + (hi-lo)×u)。取 [0.6,1.0] 的理由：
// 子弹不比宿主快（不会追过宿主造成穿越），也不小于 0.6（不会糊在原地）。
inline constexpr double kChainSplitChildSpeedLo = 0.6;
inline constexpr double kChainSplitChildSpeedHi = 1.0;

// 子子弹命中半径（像素）。宿主 hit_dis 为 0（未初始化路径）时用它兜底；
// 宿主已带命中半径时继承宿主值。
inline constexpr double kChainSplitChildHitDis = 4.0;

// 子子弹 inherit 的字段：bullet_kind / bullet_func_no(31) / p1 / life 阈值
// 语义由宿主直传（见 MakeChainSplitChildMotion），这里只声明判定入口。

// 分裂请求判定：Func31 的「本帧请求分裂」标记。
// 只认 func_no==31：Func55（蓄力特效）与 Func42（变身）同样会写 p2=1
// （Func42 甚至也写 rotate_flg=2），但它们不是分裂族，不能误触发。
// 代数钳制不在此函数内——由调用方按 generation < kChainSplitMaxGeneration
// 另行判断（本函数只回答「子弹本帧是否请求了分裂」）。
[[nodiscard]] inline bool ChainSplitRequested(const BulletMotion& b) {
    return b.bullet_func_no == 31 && b.p2 != 0;
}

// 单个宿主本帧分裂出的子子弹参数（纯数据，尚未组装成 BulletMotion）。
struct ChainSplitChild {
    double angle_deg = 0.0;    // 绝对角度（度，[0,360)）
    double speed = 0.0;        // 像素/帧
    std::uint32_t spawn_index = 0;  // 派生 spawn_index（子子弹终身携带）
};

// 计算一个宿主的分裂子子弹参数。
//
// 确定性契约（客户端镜像必须逐项一致）：
//   * RNG = DeterministicRng(seed, tick, pattern_index, host_spawn_index)，
//     每颗粒子按序消耗 2 次 RandomInt(0, 9999)：先角度抖动 u_a，再速度
//     因子 u_s；unit = value / 10000.0。
//   * angle_i = NormalizeDeg(host.angle + i×kChainSplitAngleStepDeg
//                            + (u_a - 0.5)×kChainSplitAngleJitterDeg)
//   * speed_i = host.speed × (kChainSplitChildSpeedLo
//                            + (kChainSplitChildSpeedHi - lo)×u_s)
//   * spawn_index_i = host_spawn_index + kChainSplitSpawnIndexBase + i
//   * max_children 钳制只改变循环次数（RNG 按颗粒序号取用，不预滚），
//     因此「预算不足只生成前 n 颗」在两端一致。
//
// 请求判定不在此函数内：调用方必须先用 ChainSplitRequested(host) 判定
// （AdvanceBullets 第一趟即如此），本函数只对已判定的宿主计算参数。
// host 只读；max_children ≤ 0 时返回空向量。
[[nodiscard]] std::vector<ChainSplitChild> ComputeChainSplitChildren(
    const BulletMotion& host,
    std::uint64_t seed,
    std::uint64_t tick,
    std::uint32_t pattern_index,
    std::uint32_t host_spawn_index,
    int max_children
);

// 组装一颗子子弹的完整 BulletMotion（位置 = 宿主当前位置，t/life 归零，
// delay/rotate_flg 归零，p2 归零；bullet_kind / bullet_func_no / p1 /
// hit_dis 继承宿主；speed/speed_init/spd_init2 取 child.speed）。
[[nodiscard]] BulletMotion MakeChainSplitChildMotion(
    const BulletMotion& host,
    const ChainSplitChild& child
);

}  // namespace phk::battle::danmaku
