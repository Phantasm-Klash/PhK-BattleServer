// 节点模式 id → 发射行为参数 权威表（数据）。
// 由 rev/tmp/gen_modeid_map.py 从 libMyGame.so 1.40 解出，请勿手改；
// 改动需重新运行该脚本并同步 TS 镜像 core/sim/danmaku/bullet_mode_map.ts。
#include "phk/battle/danmaku/bullet_mode_map.hpp"

namespace phk::battle::danmaku {
namespace {

// 36 组，覆盖 0..255。生成顺序即 gen_modeid_map.py 的区间压缩输出。
const BulletModeRange kRanges[] = {
    {0, 8, {24, 12}},
    {9, 17, {47, 178}},
    {18, 26, {46, 179}},
    {27, 35, {45, 180}},
    {36, 44, {44, 181}},
    {45, 53, {53, 182}},
    {54, 62, {54, 183}},
    {63, 71, {48, 184}},
    {72, 80, {55, 185}},
    {81, 89, {56, 186}},
    {90, 98, {43, 187}},
    {99, 107, {51, 188}},
    {108, 116, {49, 189}},
    {117, 125, {50, 190}},
    {126, 134, {52, 191}},
    {135, 137, {37, 192}},
    {138, 146, {33, 193}},
    {147, 155, {34, 194}},
    {156, 164, {42, 195}},
    {165, 173, {35, 196}},
    {174, 182, {32, 197}},
    {183, 191, {36, 198}},
    {192, 196, {28, 199}},
    {197, 203, {31, 200}},
    {204, 204, {24, 201}},
    {205, 205, {24, 12}},
    {206, 211, {41, 203}},
    {212, 216, {38, 204}},
    {217, 217, {39, 205}},
    {218, 218, {40, 206}},
    {219, 219, {59, 207}},
    {220, 220, {59, 208}},
    {221, 221, {29, 209}},
    {222, 223, {26, 210}},
    {224, 224, {25, 211}},
    {225, 255, {24, 12}},
};

}  // namespace

const BulletModeRange* BulletModeRanges(std::size_t* out_count) {
    if (out_count != nullptr) {
        *out_count = sizeof(kRanges) / sizeof(kRanges[0]);
    }
    return kRanges;
}

BulletModePair BulletModeForId(std::int32_t id) {
    if (id < 0) {
        return kBulletModeDefault;
    }
    for (const BulletModeRange& range : kRanges) {
        if (id <= range.hi) {
            return id >= range.lo ? range.pair : kBulletModeDefault;
        }
    }
    return kBulletModeDefault;
}

}  // namespace phk::battle::danmaku
