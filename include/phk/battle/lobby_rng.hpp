// 大厅确定性流（SHA-256 shortHash）的 C++ 移植。
//
// 真值来源（三方逐字节对齐）：
//   - Go   Gensoulkyo/runtime/core/simulation.go:349 deterministicUnit /
//         shortHash（服务端权威）
//   - TS   SpellKard/laya/.test-build/src/core/math/lobby_rng.js
//         （客户端镜像；种子必须以十进制文本传入，见头注释）
//   - C++  本文件（对战服执行层）
//
// 警告：本流与弹幕 FNV-1a 流（danmaku/DeterministicRng）算法完全不同，
// 同名函数不可互换，混用会导致 bullet_purge 选取静默分叉。
//
// 版权：东方同人逆向成果，仅用于自研玩法。
#ifndef PHK_BATTLE_LOBBY_RNG_HPP
#define PHK_BATTLE_LOBBY_RNG_HPP

#include <array>
#include <cstdint>
#include <string>

namespace phk::battle {

// FIPS 180-4 SHA-256：对 UTF-8 字节流（Go 语义：直接哈希原始字节）。
[[nodiscard]] std::array<std::uint8_t, 32> Sha256Bytes(const std::string& text);

// simulation.go shortHash：SHA-256 前 4 字节的小写十六进制（8 字符）。
[[nodiscard]] std::string ShortHashHex(const std::string& value);

// simulation.go deterministicUnit(seed, tick, patternID, index)：
// 材料 "<seed>:<tick>:<stream_id>:<index>"（seed 为带符号 int64 的 Go %d
// 十进制文本），短哈希十六进制折叠为整数后 (value % 10000) / 10000。
// 返回值域 [0,1)，粒度 1e-4。C++ 侧 seed 用 int64 直接格式化，天然避开
// JS number 的 2^53 精度损失（TS 侧必须以字符串传种子）。
[[nodiscard]] double LobbyDeterministicUnit(
    std::int64_t seed,
    std::int64_t tick,
    const std::string& stream_id,
    std::int64_t index
);

}  // namespace phk::battle

#endif  // PHK_BATTLE_LOBBY_RNG_HPP
