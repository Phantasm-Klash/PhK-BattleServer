#pragma once

#include <cstdint>

// 节点模式 id（Enemy 节点 +0x28）→ 发射行为参数 的权威查表。
//
// 来源（千夜追忆 1.40 逆向，实测）：`Enemy::createBullets1` @0x37bacc
// （libMyGame.so arm64）读 command+0x00 的模式 id，经**双跳转表**映射到两个
// 16 位初始化参数 (w2, w3)：
//   * id ∈ [9,136]   → 表1 @0xACFC03（128 项），分支基 0x37bc3c；
//   * id ∈ [137,223] → 表2 @0xACFC83（87 项），分支基 0x37bc2c；
//   * id == 224      → (25, 211)；
//   * 其余（含 id=205 经默认分支）→ (24, 12)。
// 每个分支把 (w2, w3) 作为参数 2/3 传给每颗子弹的虚初始化（vtable+0x218）。
//
// 语义标注（证据分级，遵循 rev/patterns 约定）：
//   * w3 ∈ [178,211]：32 个值，与 kEnemyBullet 弹种值域（9..224）相容 **[推断]**；
//   * w2 ∈ [24,59]：31 个值，与 bulletFunc 编号值域（0..78）相容 **[推断]**；
//   * 精确字段赋值（w2→bullet_func_no / w3→bullet_kind）尚未逐指令验证，
//     因此本模块只暴露**原始数对**，由使用方按上下文解释。
//
// 结构：216 个弹种 id（9..224）被压缩为 36 个行为组（每组约 9 个连续 id）。
// 完整 JSON 产物：rev/patterns/bullet_modeid_map.json（由 rev/tmp/gen_modeid_map.py 生成）。
//
// 本模块闭合了 knowledge-base/notes/tna-reverse-notes.md 记录的「唯一缺口」：
// 节点模式 id → 发射行为映射。TS 镜像：core/sim/danmaku/bullet_mode_map.ts。
namespace phk::battle::danmaku {

struct BulletModePair {
    std::int32_t w2 = 24;  // 参数 2（24..59）
    std::int32_t w3 = 12;  // 参数 3（12 / 178..211）
};

// 默认对（越界 id 与 id=205 的默认分支）。
inline constexpr BulletModePair kBulletModeDefault{24, 12};

// 权威区间表：36 组（lo, hi, pair），按 lo 升序，覆盖 0..255 全空间。
struct BulletModeRange {
    std::int32_t lo;
    std::int32_t hi;
    BulletModePair pair;
};

[[nodiscard]] const BulletModeRange* BulletModeRanges(std::size_t* out_count);

// 查表：模式 id → 行为数对。id < 0 或 > 255 返回默认对。
[[nodiscard]] BulletModePair BulletModeForId(std::int32_t id);

}  // namespace phk::battle::danmaku
