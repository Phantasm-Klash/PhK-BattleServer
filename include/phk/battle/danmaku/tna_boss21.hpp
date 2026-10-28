#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// TNA 1.40 `Enemy::setBossBullet21` @0x4d6f28 的弹幕节奏表（**全部实测**）。
//
// 一、节奏点：capstone 扫帧判定 `(frame - phase) % period == 0`
//   （`subs w8,w8,#phase` → `msub` 求模 → `cbnz`），实测 11 个：
//   4×(300,1)、4×(400,1)、(20,1)、(112,91)、(120,91)。
//
// 二、每波弹数：节点 +0x10 起是 96B `SpawnCommand`（count 在 +0x2c），
//   各块把 `.rodata` 模板的不同切片拷进去，所以「count 在模板哪个偏移」逐块不同，
//   必须按**该块实际的 `stur qX,[x0,#0x3c]` 来源**取：
//     * (400,1) 循环块（0x4d7e68）：`q1 = tpl[0x14]` → `node+0x3c` ⇒ count=1，
//       且是 **9 次循环**（`cmp w22,#9` + `sub w23,#0xc`，角度 270° 起每步 −12°）；
//     * (300,1) 共享尾块（0x4db624）：`q1 = tpl[0x28]` → `node+0x3c` ⇒ count=tpl+0x2c。
//
// 三、实测结果：
//   | 周期 | 相位 | 模板 | count | 说明 |
//   | 400 | 1 | 0xb05550/0xb05660/0xb05770/0xaced98 | 1×9 节点 | 角度 270°±10，每步 −12° |
//   | 18  | 1 | 0xb04f98 | 6 | 原为 300 帧周期内的 18 帧子节奏 |
//   | 18  | 1 | 0xb05078 | 8 | 同上 |
//   | 18  | 1 | 0xb05158 | 12 | 同上 |
//   | 18  | 1 | 0xb05238 | 12 | 同上 |
//   | 20  | 1 | 0xb05f28 | 30 | 高频小波 |
//   | 112 | 91 | 0xb06820 | 30 | — |
//   | 120 | 91 | — | 0 | **未解出**（模板 count 读为 0） |
//
// 四、未解码项保持 count=0、不发射；不编造数值。
namespace phk::battle::danmaku {

struct TnaCadenceEntry {
    std::uint32_t period;         // 帧周期
    std::uint32_t phase;          // 相位：frame >= phase 且 (frame-phase) % period == 0
    std::uint16_t nodes;          // 每次触发的节点数（循环次数）
    std::uint16_t count_per_node; // 每节点弹数（0 = 未解码，不发射）
    float base_angle_deg;         // 基准角度（度）
    float angle_jitter_deg;       // 角度抖动 ±（度）
    float angle_step_deg;         // 每轮角度步进（度）
};

// (400,1) 循环块的实测常量。
inline constexpr std::uint16_t kTnaBoss21NodesPerTrigger = 9;
inline constexpr std::uint16_t kTnaBoss21CountPerNode = 1;
inline constexpr float kTnaBoss21BaseAngleDeg = 270.0f;
inline constexpr float kTnaBoss21AngleJitterDeg = 10.0f;
inline constexpr float kTnaBoss21AngleStepDeg = -12.0f;

inline constexpr std::array<TnaCadenceEntry, 11> kTnaBoss21Cadence = {{
    // 4×(400,1)：9 节点循环 × 每节点 1 发
    {400u, 1u, kTnaBoss21NodesPerTrigger, kTnaBoss21CountPerNode, 270.0f, 10.0f, -12.0f},
    {400u, 1u, kTnaBoss21NodesPerTrigger, kTnaBoss21CountPerNode, 270.0f, 10.0f, -12.0f},
    {400u, 1u, kTnaBoss21NodesPerTrigger, kTnaBoss21CountPerNode, 270.0f, 10.0f, -12.0f},
    {400u, 1u, kTnaBoss21NodesPerTrigger, kTnaBoss21CountPerNode, 270.0f, 10.0f, -12.0f},
    // 4×(300,1) 的 18 帧子节奏：单节点、count 6/8/12/12
    {18u, 1u, 1, 6, 90.0f, 0.0f, 0.0f},
    {18u, 1u, 1, 8, 90.0f, 0.0f, 0.0f},
    {18u, 1u, 1, 12, 90.0f, 0.0f, 0.0f},
    {18u, 1u, 1, 12, 90.0f, 0.0f, 0.0f},
    // 高频小波与 91 相位波
    {20u, 1u, 1, 30, 90.0f, 0.0f, 0.0f},
    {112u, 91u, 1, 30, 90.0f, 0.0f, 0.0f},
    // (120,91)：模板 count 读为 0，未解出 → 不发射
    {120u, 91u, 1, 0, 90.0f, 0.0f, 0.0f},
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

// 一段 tick 区间内累计发射弹数（与合成模式对比密度）。
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
