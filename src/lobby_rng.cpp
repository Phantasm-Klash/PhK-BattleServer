// 大厅确定性流（SHA-256 shortHash）实现。逐函数对照：
//   - Sha256Bytes      : FIPS 180-4（与 TS lobby_rng.js sha256Bytes 同构，
//                        但输入为 UTF-8 原始字节，与 Go crypto/sha256 语义一致）
//   - ShortHashHex     : simulation.go shortHash
//   - LobbyDeterministicUnit : simulation.go deterministicUnit
//   - HmacSha256Hex    : RFC 2104/4231 HMAC-SHA256（T6 服务回调签名，
//                        与 Go crypto/hmac 输出逐字节同形）
//
// 版权：东方同人逆向成果，仅用于自研玩法。

#include "phk/battle/lobby_rng.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace phk::battle {
namespace {

// FIPS 180-4 轮常量。
constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

// 32 位字右旋 n 位（无符号回绕，行为由标准保证）。
[[nodiscard]] std::uint32_t Rotr(std::uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32u - bits));
}

// simulation.go hexValue：单个十六进制字符的数值，非法字符计 0。
[[nodiscard]] int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return 0;
}

}  // namespace

std::array<std::uint8_t, 32> Sha256Bytes(const std::string& text) {
    const std::size_t length = text.size();

    // 填充：0x80 + 0x00... 使长度 % 64 == 56，再追加 64 位大端比特长度。
    std::string padded(text);
    padded.push_back(static_cast<char>(0x80));
    while (padded.size() % 64u != 56u) {
        padded.push_back('\0');
    }
    const std::uint64_t bit_length = static_cast<std::uint64_t>(length) * 8u;
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<char>((bit_length >> shift) & 0xffu));
    }

    std::uint32_t state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    std::uint32_t schedule[64] = {};
    for (std::size_t offset = 0; offset < padded.size(); offset += 64u) {
        for (int i = 0; i < 16; ++i) {
            const std::size_t at = offset + static_cast<std::size_t>(i) * 4u;
            schedule[i] = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(padded[at])) << 24) |
                          (static_cast<std::uint32_t>(static_cast<std::uint8_t>(padded[at + 1])) << 16) |
                          (static_cast<std::uint32_t>(static_cast<std::uint8_t>(padded[at + 2])) << 8) |
                          static_cast<std::uint32_t>(static_cast<std::uint8_t>(padded[at + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t x = schedule[i - 15];
            const std::uint32_t y = schedule[i - 2];
            const std::uint32_t s0 = Rotr(x, 7) ^ Rotr(x, 18) ^ (x >> 3);
            const std::uint32_t s1 = Rotr(y, 17) ^ Rotr(y, 19) ^ (y >> 10);
            schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
        }

        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        std::uint32_t e = state[4];
        std::uint32_t f = state[5];
        std::uint32_t g = state[6];
        std::uint32_t h = state[7];

        for (int i = 0; i < 64; ++i) {
            const std::uint32_t big_s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + big_s1 + choose + kSha256K[i] + schedule[i];
            const std::uint32_t big_s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = big_s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) {
        out[static_cast<std::size_t>(i) * 4u + 0u] = static_cast<std::uint8_t>((state[i] >> 24) & 0xffu);
        out[static_cast<std::size_t>(i) * 4u + 1u] = static_cast<std::uint8_t>((state[i] >> 16) & 0xffu);
        out[static_cast<std::size_t>(i) * 4u + 2u] = static_cast<std::uint8_t>((state[i] >> 8) & 0xffu);
        out[static_cast<std::size_t>(i) * 4u + 3u] = static_cast<std::uint8_t>(state[i] & 0xffu);
    }
    return out;
}

std::string ShortHashHex(const std::string& value) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    const std::array<std::uint8_t, 32> digest = Sha256Bytes(value);
    std::string out;
    out.reserve(8);
    for (int i = 0; i < 4; ++i) {
        out.push_back(kHexDigits[(digest[static_cast<std::size_t>(i)] >> 4) & 0x0fu]);
        out.push_back(kHexDigits[digest[static_cast<std::size_t>(i)] & 0x0fu]);
    }
    return out;
}

double LobbyDeterministicUnit(
    std::int64_t seed,
    std::int64_t tick,
    const std::string& stream_id,
    std::int64_t index
) {
    const std::string material =
        std::to_string(seed) + ":" + std::to_string(tick) + ":" + stream_id + ":" + std::to_string(index);
    const std::string hash = ShortHashHex(material);
    std::uint64_t value = 0;
    for (char ch : hash) {
        value = value * 16u + static_cast<std::uint64_t>(HexValue(ch));
    }
    return static_cast<double>(value % 10000u) / 10000.0;
}

std::string HmacSha256Hex(const std::string& key, const std::string& message) {
    // RFC 2104：块长 64B；key 超长先 SHA-256(key)，再零填充到块长；
    // HMAC = SHA256(opad || SHA256(ipad || message))，hex 小写输出。
    constexpr std::size_t kBlockSize = 64;
    std::string normalized_key(key);
    if (normalized_key.size() > kBlockSize) {
        const std::array<std::uint8_t, 32> digested = Sha256Bytes(normalized_key);
        normalized_key.assign(reinterpret_cast<const char*>(digested.data()), digested.size());
    }
    normalized_key.resize(kBlockSize, '\0');

    std::string inner(kBlockSize, '\0');
    std::string outer(kBlockSize, '\0');
    for (std::size_t i = 0; i < kBlockSize; ++i) {
        const auto key_byte = static_cast<std::uint8_t>(normalized_key[i]);
        inner[i] = static_cast<char>(key_byte ^ 0x36u);
        outer[i] = static_cast<char>(key_byte ^ 0x5cu);
    }
    inner.append(message);
    const std::array<std::uint8_t, 32> inner_digest = Sha256Bytes(inner);
    outer.append(reinterpret_cast<const char*>(inner_digest.data()), inner_digest.size());
    const std::array<std::uint8_t, 32> mac = Sha256Bytes(outer);

    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::uint8_t byte : mac) {
        out.push_back(kHexDigits[(byte >> 4) & 0x0fu]);
        out.push_back(kHexDigits[byte & 0x0fu]);
    }
    return out;
}

}  // namespace phk::battle
