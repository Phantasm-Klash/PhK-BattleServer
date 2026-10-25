#pragma once

#include <cmath>
#include <cstdint>

// 难度切换契约（计划 v2 P4「难度切换协调统一」）。
//
// 权威在服务端：发射时按难度缩放（弹速 / 密度 / 间隔），客户端只消费快照
// `difficulty` 字段（HUD 显示），不做本地难度换算。
//
// 启用条件（硬约束）：只在 ruleset_version == kRulesetDanmakuMechanics
// （"tna-1.40-danmaku"）下应用；默认 ruleset 完全不读 difficulty。
//
// **数值为冻结占位**：以下系数未经 APK 实测校准，先冻结一组单调档位
// （easy 慢而疏 → lunatic 快而密）保证契约形状与双端同源；P4 后续按 APK
// 实测调值时只改本表（双端同步），调用方不得在表外特调。
namespace phk::battle::danmaku {

struct DifficultyScale {
    double speed_mul = 1.0;     // 发射速度倍率
    double density_mul = 1.0;   // 每波 count 倍率（llround 后生效）
    double interval_mul = 1.0;  // 发射间隔倍率（周期 llround；相位 offset 不变）
};

// 难度 → 缩放系数。取值 0/1/2/3 = easy/normal/hard/lunatic（默认 1 = normal）。
//   0 = 0.85 / 0.80 / 1.15（慢而疏）
//   1 = 1.00 / 1.00 / 1.00（基准；现有玩法零变化，单测钉死）
//   2 = 1.10 / 1.15 / 0.90
//   3 = 1.25 / 1.35 / 0.80（快而密）
// 未知难度（含负值/越界）→ normal 基准（防御性回落，防配置漂移静默改玩法）。
[[nodiscard]] inline constexpr DifficultyScale DifficultyScaleFor(std::int32_t difficulty) {
    switch (difficulty) {
        case 0: return DifficultyScale{0.85, 0.80, 1.15};
        case 1: return DifficultyScale{1.0, 1.0, 1.0};
        case 2: return DifficultyScale{1.10, 1.15, 0.90};
        case 3: return DifficultyScale{1.25, 1.35, 0.80};
        default: return DifficultyScale{1.0, 1.0, 1.0};
    }
}

// 恒等判定（难度 1 / 未启用难度 = 全部 1.0）。用于热点路径的零成本短路，
// 也便于测试钉帧「难度 1 = 无操作」。
[[nodiscard]] inline constexpr bool IsIdentityScale(const DifficultyScale& scale) {
    return scale.speed_mul == 1.0 && scale.density_mul == 1.0 && scale.interval_mul == 1.0;
}

// 周期缩放（llround）。倍率 1.0 时 llround(x·1.0) == x（IEEE 精确），
// 因此恒等缩放下周期逐字节不变。
[[nodiscard]] inline std::uint32_t ScalePeriod(std::uint32_t period, double mul) {
    return static_cast<std::uint32_t>(std::llround(static_cast<double>(period) * mul));
}

// 每波弹数缩放（llround；倍率 1.0 时逐字节不变）。
[[nodiscard]] inline int ScaleCount(int count, double mul) {
    return static_cast<int>(std::llround(static_cast<double>(count) * mul));
}

}  // namespace phk::battle::danmaku
