// 216 个 BulletKind 的适配层。
//
// 权威定义在 PhK-Protocol 生成契约 `phk/v1/danmaku.hpp`（客户端/服务端共用），
// 本头文件只做别名与便捷查询，避免 216 个枚举值在两处重复、漂移。
// 值 = 原版 kEnemyBullet 9..224。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "phk/v1/danmaku.hpp"

namespace phk::battle::danmaku {

using BulletKind = phk::v1::BulletKind;
using BulletKindInfo = phk::v1::BulletKindInfo;

inline constexpr std::int32_t kBulletKindMin = 9;
inline constexpr std::int32_t kBulletKindMax = 224;
inline constexpr std::size_t kBulletKindCount = phk::v1::kBulletKindCount;

// 216 条 type→帧/图集 表（直接引用协议契约）。
[[nodiscard]] inline const std::array<BulletKindInfo, kBulletKindCount>& BulletKindTable() {
    return phk::v1::kBulletKinds;
}

[[nodiscard]] inline bool IsValidBulletKind(std::int32_t type) {
    return type >= kBulletKindMin && type <= kBulletKindMax;
}

// 按原版 type 值查询；越界返回 nullptr。表项按 type 升序排列。
[[nodiscard]] inline const BulletKindInfo* FindBulletKind(std::int32_t type) {
    if (!IsValidBulletKind(type)) {
        return nullptr;
    }
    return &phk::v1::kBulletKinds[static_cast<std::size_t>(type - kBulletKindMin)];
}

[[nodiscard]] inline const BulletKindInfo* FindBulletKind(BulletKind kind) {
    return FindBulletKind(static_cast<std::int32_t>(kind));
}

// 图集帧名（如 "kusabi_01"）；越界返回空。
[[nodiscard]] inline std::string_view BulletKindFrame(std::int32_t type) {
    const BulletKindInfo* info = FindBulletKind(type);
    return info != nullptr ? info->frame : std::string_view{};
}

// 物种名（如 kusabi/kome/heart）；越界返回空。
[[nodiscard]] inline std::string_view BulletKindSpecies(std::int32_t type) {
    const BulletKindInfo* info = FindBulletKind(type);
    return info != nullptr ? info->species : std::string_view{};
}

}  // namespace phk::battle::danmaku
