#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// TNA 1.40 `Enemy::setBossBullet21` @0x4d6f28 的弹幕节奏表。
//
// 全部条目**实测**：capstone 扫函数体内的帧判定 `(frame - phase) % period == 0`
// （`subs w8,w8,#phase` → `msub` 求模 → `cbnz`），脚本 `cadence.py`。
// 实测到 11 个节奏点：4×(300,1)、4×(400,1)、(20,1)、(112,91)、(120,91)。
//
// 字段来源与置信度：
//   * period / phase —— **实测**（指令直接读出）；
//   * count / angle / spread —— 取自该节奏点后首个节点构造拷贝的 `.rodata` 模板
//     （节点 +0x10 起为 96B `SpawnCommand`；+0x2c=count、+0x14=angle、+0x30=spread）。
//     只有 4 个 (400,1) 条目的模板解出可信 count=180 / spread=2.0，其余模板的
//     count 为 0 —— **0 表示原版此处由运行期算式决定**（137 个节点里仅 1 个 count
//     是立即数，角度全部由 `random(1,360)×30±c` 现算，见
//     docs/apk-reverse-notes/patterns/boss21_states.md）。
//     本表对 count==0 的条目**不发射**：宁可少发，也不编造数值。
//
// 启用条件：ruleset == "tna-1.40-danmaku" 且 boss_id 属于本表（当前只有 boss21）。
namespace phk::battle::danmaku {

struct TnaCadenceEntry {
    std::uint32_t period;        // 帧周期
    std::uint32_t phase;         // 相位：frame >= phase 且 (frame-phase) % period == 0
    std::int16_t count;          // 每次发射数量（0 = 未解码，不发射）
    float angle_deg;             // 基准角度（度）
    float spread_deg;            // 张角 / 速度（模板 +0x30）
    std::uint32_t template_addr; // 溯源：.rodata 模板地址（0 = 未解析）
};

inline constexpr std::array<TnaCadenceEntry, 11> kTnaBoss21Cadence = {{
    // 4 个 (400, 1)：模板解出 count=180、spread=2.0（三条同参 + 一条 270°/120°）
    {400u, 1u, 180, 0.0f, 2.0f, 0xb05550u},
    {400u, 1u, 180, 0.0f, 2.0f, 0xb05660u},
    {400u, 1u, 180, 0.0f, 2.0f, 0xb05770u},
    {400u, 1u, 1, 270.0f, 120.0f, 0xaced98u},
    // 4 个 (300, 1)：模板 count 为运行期计算 → 不发射（未解码）
    {300u, 1u, 0, 0.0f, 0.0f, 0u},
    {300u, 1u, 0, 0.0f, 0.0f, 0u},
    {300u, 1u, 0, 0.0f, 0.0f, 0u},
    {300u, 1u, 0, 0.0f, 0.0f, 0u},
    // 高频小波：同样未解码
    {20u, 1u, 0, 0.0f, 0.0f, 0u},
    {112u, 91u, 0, 0.0f, 0.0f, 0u},
    {120u, 91u, 0, 0.0f, 0.0f, 0u},
}};

// 本 tick 是否触发该节奏点。
[[nodiscard]] constexpr bool TnaCadenceFires(const TnaCadenceEntry& entry, std::uint64_t tick) noexcept {
    return tick >= entry.phase && (tick - entry.phase) % entry.period == 0;
}

// 本 tick 应发射的条目数（只计 count > 0 的条目）。
[[nodiscard]] constexpr std::size_t TnaBoss21FiringCount(std::uint64_t tick) noexcept {
    std::size_t fired = 0;
    for (const TnaCadenceEntry& entry : kTnaBoss21Cadence) {
        if (entry.count > 0 && TnaCadenceFires(entry, tick)) {
            ++fired;
        }
    }
    return fired;
}

// 本 tick 应发射的弹数合计。
[[nodiscard]] constexpr std::uint32_t TnaBoss21BulletsAt(std::uint64_t tick) noexcept {
    std::uint32_t total = 0;
    for (const TnaCadenceEntry& entry : kTnaBoss21Cadence) {
        if (entry.count > 0 && TnaCadenceFires(entry, tick)) {
            total += static_cast<std::uint32_t>(entry.count);
        }
    }
    return total;
}

// 一段 tick 区间内累计发射弹数（用于和合成模式对比密度）。
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

}  // namespace phk::battle::danmaku
