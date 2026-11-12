#pragma once

#include <cstdint>

// 原版 EnemyBullet::setBulletType @0x512140 逐分支解出的 (hitDis, artScale)。
// 生成脚本：docs/apk-reverse-notes/patterns/decode_bullet_size.py
// hitDis 写对象 +0x54c；scale 写 +0x5f0（fmov 立即数）。
// 216/216 个 kEnemyBullet 全部覆盖。
namespace phk::battle::danmaku {

struct BulletSizeSpec {
    std::int32_t lo;
    std::int32_t hi;
    float hit_dis;
    float art_scale;
};

inline constexpr BulletSizeSpec kBulletSizeSpecs[] = {
    {  9, 121,   2.4000f, 1.0f},
    {122, 122,   2.4000f, 1.5f},
    {123, 124,   2.4000f, 1.0f},
    {125, 125,   8.0000f, 1.5f},
    {126, 126,   2.4000f, 1.5f},
    {127, 127,   8.0000f, 1.5f},
    {128, 129,   2.4000f, 1.0f},
    {130, 130,   2.4000f, 1.5f},
    {131, 131,   2.4000f, 1.0f},
    {132, 136,   2.4000f, 1.5f},
    {137, 137,   8.0000f, 1.5f},
    {138, 139,   2.4000f, 1.5f},
    {140, 143,   8.0000f, 1.5f},
    {144, 144,   2.4000f, 1.5f},
    {145, 183,   8.0000f, 1.5f},
    {184, 185,   2.4000f, 1.0f},
    {186, 186,  14.1400f, 2.0f},
    {187, 187,   2.4000f, 1.0f},
    {188, 188,   8.0000f, 1.5f},
    {189, 190,  14.1400f, 2.0f},
    {191, 191,   8.0000f, 1.5f},
    {192, 196,  14.1400f, 2.0f},
    {197, 200,   8.0000f, 1.5f},
    {201, 201,  86.0000f, 5.0f},
    {202, 202,   8.0000f, 1.5f},
    {203, 204,  86.0000f, 5.0f},
    {205, 209,   2.4000f, 1.0f},
    {210, 218,   0.0000f, 1.0f},
    {219, 219,   0.0000f, 2.0f},
    {220, 220,   0.0000f, 1.0f},
    {221, 224,  66.0000f, 3.0f},
};

inline constexpr std::size_t kBulletSizeSpecCount =
    sizeof(kBulletSizeSpecs) / sizeof(kBulletSizeSpecs[0]);

[[nodiscard]] inline float BulletHitDisForKind(int kind) {
    for (const BulletSizeSpec& s : kBulletSizeSpecs) {
        if (kind >= s.lo && kind <= s.hi) return s.hit_dis;
    }
    return 4.0f;  // 兜底（与原实现一致）
}

[[nodiscard]] inline float BulletArtScaleForKind(int kind) {
    for (const BulletSizeSpec& s : kBulletSizeSpecs) {
        if (kind >= s.lo && kind <= s.hi) return s.art_scale;
    }
    return 1.0f;
}

}  // namespace phk::battle::danmaku
