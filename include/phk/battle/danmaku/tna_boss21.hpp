#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// TNA 1.40 `Enemy::setBossBullet21` @0x4d6f28 的弹幕节奏表（**实测**）。
//
// 一、节奏点（实测）
//   capstone 扫函数体内的帧判定 `(frame - phase) % period == 0`
//   （`subs w8,w8,#phase` → `msub` 求模 → `cbnz`），得到 11 个：
//   4×(300,1)、4×(400,1)、(20,1)、(112,91)、(120,91)。
//
// 二、每次触发的形状（实测，0x4d7e68 区块）
//   触发后进入一个 **9 次循环**（`add w22,w22,#1` / `cmp w22,#9` / `b.ne`），
//   每轮 `sub w23,w23,#0xc`（**角度递减 12°**），每轮 `operator new(0x80)` 造一个
//   节点并拷贝 `.rodata` 模板 **0xb052c0**：
//     * 节点 +0x10 起 = 96B `SpawnCommand`；
//     * `q1 = tpl[0x14..0x23]` 整块写到 `node+0x3c` ⇒ **count = i16(tpl+0x14) = 1**；
//     * `str s14,[x0,#0x24]` 覆盖角度 = `270 + random(-10,10)`；
//     * `str s15,[x0,#0x38]` 覆盖速度抖动 = `random(0,6)·0.5+0.5`；
//     * 速度 = `[+0x6bc] + cos(θ)·(80+20·n)`，角度基准 = `[+0x6c0] + sin(θ)·(100+20·n)`。
//
// 三、因此每次触发 = 9 个节点 × 1 发 = **9 发**（角度 270°±10°、每轮基准偏 -12°）。
//
// 四、**未解码**：11 个节奏点里，(20,1)/(112,91)/(120,91) 与部分 (300,1) 的模板
//   未被 `adrp+add` 直接寻址（走寄存器间接），count 取不到。这些条目的
//   `count_per_node` 记为 0，在 {@link TnaBoss21BulletsAt} 中不发射：
//   宁可少发，也不编造数值。
namespace phk::battle::danmaku {

struct TnaCadenceEntry {
    std::uint32_t period;         // 帧周期
    std::uint32_t phase;          // 相位：frame >= phase 且 (frame-phase) % period == 0
    std::uint16_t nodes;          // 每次触发的节点数（循环次数；实测 9）
    std::uint16_t count_per_node; // 每节点弹数（实测 count = tpl+0x14 = 1；0 = 未解码）
    float base_angle_deg;         // 基准角度（实测 270）
    float angle_jitter_deg;       // 角度抖动 ±（实测 10）
    float angle_step_deg;         // 每轮角度步进（实测 -12）
};

// 实测常量（0x4d7e68 区块 + 模板 0xb052c0）。
inline constexpr std::uint16_t kTnaBoss21NodesPerTrigger = 9;
inline constexpr std::uint16_t kTnaBoss21CountPerNode = 1;
inline constexpr float kTnaBoss21BaseAngleDeg = 270.0f;
inline constexpr float kTnaBoss21AngleJitterDeg = 10.0f;
inline constexpr float kTnaBoss21AngleStepDeg = -12.0f;

inline constexpr std::array<TnaCadenceEntry, 11> kTnaBoss21Cadence = {{
    // 4×(400,1)：已配对模板 0xb05550 / 0xb05660 / 0xb05770 / 0xaced98
    {400u, 1u, kTnaBoss21NodesPerTrigger, kTnaBoss21CountPerNode, 270.0f, 10.0f, -12.0f},
    {400u, 1u, kTnaBoss21NodesPerTrigger, kTnaBoss21CountPerNode, 270.0f, 10.0f, -12.0f},
    {400u, 1u, kTnaBoss21NodesPerTrigger, kTnaBoss21CountPerNode, 270.0f, 10.0f, -12.0f},
    {400u, 1u, kTnaBoss21NodesPerTrigger, kTnaBoss21CountPerNode, 270.0f, 10.0f, -12.0f},
    // 4×(300,1)：模板走寄存器间接，count 未解出 → 不发射
    {300u, 1u, kTnaBoss21NodesPerTrigger, 0, 270.0f, 10.0f, -12.0f},
    {300u, 1u, kTnaBoss21NodesPerTrigger, 0, 270.0f, 10.0f, -12.0f},
    {300u, 1u, kTnaBoss21NodesPerTrigger, 0, 270.0f, 10.0f, -12.0f},
    {300u, 1u, kTnaBoss21NodesPerTrigger, 0, 270.0f, 10.0f, -12.0f},
    // 高频小波与 91 相位波：同样未解码
    {20u, 1u, kTnaBoss21NodesPerTrigger, 0, 270.0f, 10.0f, -12.0f},
    {112u, 91u, kTnaBoss21NodesPerTrigger, 0, 270.0f, 10.0f, -12.0f},
    {120u, 91u, kTnaBoss21NodesPerTrigger, 0, 270.0f, 10.0f, -12.0f},
}};

// 本 tick 是否触发该节奏点。
[[nodiscard]] constexpr bool TnaCadenceFires(const TnaCadenceEntry& entry, std::uint64_t tick) noexcept {
    return tick >= entry.phase && (tick - entry.phase) % entry.period == 0;
}

// 该条目本次触发的弹数（节点数 × 每节点弹数）。
[[nodiscard]] constexpr std::uint32_t TnaCadenceBullets(const TnaCadenceEntry& entry) noexcept {
    return static_cast<std::uint32_t>(entry.nodes) * entry.count_per_node;
}

// 本 tick 应发射的弹数合计。
[[nodiscard]] constexpr std::uint32_t TnaBoss21BulletsAt(std::uint64_t tick) noexcept {
    std::uint32_t total = 0;
    for (const TnaCadenceEntry& entry : kTnaBoss21Cadence) {
        if (TnaCadenceFires(entry, tick)) {
            total += TnaCadenceBullets(entry);
        }
    }
    return total;
}

// 本 tick 触发的条目数（只计会发射的）。
[[nodiscard]] constexpr std::size_t TnaBoss21FiringCount(std::uint64_t tick) noexcept {
    std::size_t fired = 0;
    for (const TnaCadenceEntry& entry : kTnaBoss21Cadence) {
        if (TnaCadenceBullets(entry) > 0 && TnaCadenceFires(entry, tick)) {
            ++fired;
        }
    }
    return fired;
}

// 一段 tick 区间内累计发射弹数（用于与合成模式对比密度）。
[[nodiscard]] constexpr std::uint64_t TnaBoss21BulletsInRange(
    std::uint64_t first_tick,
    std::uint64_t last_tick
) noexcept {
    std::uint64_t total = 0;
    for (std::uint64_t tick = first_tick; tick <= last_tick; ++tick) {
        total += TnaBoss21BulletsAt(tick);
    }
    return total;
}

// 第 index 个节点的角度（发射器用）：基准 + 步进×index；抖动由调用方用服务端 RNG 加。
[[nodiscard]] constexpr float TnaBoss21NodeAngleDeg(const TnaCadenceEntry& entry, std::uint16_t index) noexcept {
    return entry.base_angle_deg + entry.angle_step_deg * static_cast<float>(index);
}

}  // namespace phk::battle::danmaku
