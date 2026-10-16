#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "phk/battle/danmaku/pattern_timeline.hpp"

// Boss 目录（线程A「游戏内容」）：每个 boss = 有序 pattern 阶段组 + HP + 移动方式。
//
// 归属说明（APK 1.40 低成本归属结论，详见 knowledge-base/notes/tna-reverse-notes.md
// 与 patterns/boss_pattern_tables.md）：
//   * 1.40 libMyGame.so 的舞台名表（.rodata 0xac595f，JP/CN 共用）共 7 项：
//     「妖 永 風 地 輝 紺 怪」。`hui`（辉）即其中第 5 个舞台「輝」，已确证。
//   * 「天」在 1.40 字符串表里没有独立舞台名（仅成就「通关［紅〜天］所有关卡」
//     与开发者注释「这是我在『天』关卡中最喜欢的符卡」提到「天」关卡），归属为
//     「游戏内存中存在但名称串不在本二进制」——按任务书授权回退路径处理：
//     阶段组从**已移植的 10 个 TNA pattern 库**组合而成，组合理由记录在此注释
//     与 KB，不新造哈希/浮点库。
//   * 阶段组的 pattern 全部取自 danmaku::BossRacePatternLibrary()（顺序固定的
//     10 个模式），三引擎（Go 协议层只传 id / C++ 权威 / TS 镜像）数值完全一致。
namespace phk::battle::danmaku {

// Boss 移动方式。全部用**整数三角波**实现（无新增浮点运算，三引擎逐位一致）：
//   offset(tick) = -amplitude + (2*amplitude/half) * tri(tick)
//   tri(tick)    = tick % period; if (tri > half) tri = period - tri;  // 0..half
// 要求 (2*amplitude) % half == 0，保证整数整除精确。
enum class BossMovement {
    Static,  // 不动（= 现状默认行为）
    SwayX,   // 水平往复
    BobY,    // 垂直往复
};

struct BossMovementSpec {
    BossMovement kind = BossMovement::Static;
    std::int32_t amplitude_milli = 0;  // 幅度（milli，> 0 时生效）
    std::uint32_t period_ticks = 0;    // 往复周期（tick）
    std::uint32_t half_ticks = 0;      // = period_ticks / 2（运行期预算，避免重复除）
};

// 一个阶段：连续重复某个 pattern 若干波（每波 = 该 pattern 的 period_ticks）。
struct BossPatternPhase {
    std::string_view pattern_id;  // 库内模式 id（见 BossRacePatternLibrary）
    std::uint32_t waves = 0;      // 连续波次数（>= 1）
};

struct BossCatalogEntry {
    std::string_view id;          // boss id（协议值，如 "tian" / "hui"）
    std::string_view display;     // 展示名（汉字）
    std::uint64_t boss_max_hp = 0;
    BossMovementSpec movement;
    std::vector<BossPatternPhase> phases;  // 有序阶段组（循环）
};

// 权威目录（顺序即目录顺序；新增 boss 只允许追加，不得重排）。
[[nodiscard]] const std::vector<BossCatalogEntry>& BossCatalog();

// 按 id 查目录项；找不到返回 nullptr（调用方回退默认 timeline）。
[[nodiscard]] const BossCatalogEntry* FindBossCatalogEntry(std::string_view boss_id);

// boss 是否存在（协议校验用）。
[[nodiscard]] bool IsKnownBossId(std::string_view boss_id);

// 该 boss 展开后的 pattern index 序列长度（波数总和）。
[[nodiscard]] std::size_t BossTimelineLength(const BossCatalogEntry& entry);

// 该 boss 第 wave_index 波（0-based，循环）使用的库内 pattern index。
// 找不到 entry/越界返回 0（kome_ring，库首项）。
[[nodiscard]] std::size_t BossTimelinePatternIndexAt(
    const BossCatalogEntry& entry,
    std::size_t wave_index
);

// 某 tick 的发射点偏移（milli，整数三角波）。tick 为**对局内 tick**（从 1 起）。
// Static 或 amplitude/period 为 0 时返回 (0, 0)。
[[nodiscard]] void BossEmitterOffsetMilli(
    const BossMovementSpec& movement,
    std::uint64_t tick,
    std::int32_t* out_x_milli,
    std::int32_t* out_y_milli
);

}  // namespace phk::battle::danmaku
