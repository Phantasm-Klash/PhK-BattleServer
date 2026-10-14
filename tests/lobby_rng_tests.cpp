// 大厅确定性流（SHA-256 shortHash）测试。
//
// 金标准值来源：
//   - NIST FIPS 180-4 向量（空串 / "abc" / 56 字节向量 / quick fox）。
//   - 6 组 parity 用例由 Python hashlib 与 TS lobby_rng.js（种子以十进制
//     文本传入）在服务器上双跑交叉确认，含 2^53+1 与 int64 极值种子。
//
// 版权：东方同人逆向成果，仅用于自研玩法。

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

#include "phk/battle/lobby_rng.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& label) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << label << "\n";
    }
}

using phk::battle::LobbyDeterministicUnit;
using phk::battle::Sha256Bytes;
using phk::battle::ShortHashHex;

std::string DigestHex(const std::array<std::uint8_t, 32>& digest) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::uint8_t byte : digest) {
        out.push_back(kHexDigits[(byte >> 4) & 0x0fu]);
        out.push_back(kHexDigits[byte & 0x0fu]);
    }
    return out;
}

// SHA-256 全量摘要对 FIPS 180-4 向量。
void TestSha256NistVectors() {
    Check(DigestHex(Sha256Bytes("")) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "sha256 empty");
    Check(DigestHex(Sha256Bytes("abc")) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256 abc");
    Check(DigestHex(Sha256Bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "sha256 56-byte vector");
    Check(DigestHex(Sha256Bytes("The quick brown fox jumps over the lazy dog")) ==
              "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592",
          "sha256 quick fox");

    // 跨块边界：119 字节触发第二个压缩块（金标准 = Python hashlib）。
    const std::string long_input(119, 'a');
    Check(DigestHex(Sha256Bytes(long_input)) ==
              "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb",
          "sha256 119-byte cross-block vector");
}

// 跨三语言 parity：金标准 = Python hashlib（服务器双跑确认，TS 以十进制
// 文本种子传入时 6/6 一致；用 JS number 传种子会因 2^53 精度分叉，见
// task-queue/out/T4_lib_parity.md）。
void TestShortHashParity() {
    struct Case {
        std::int64_t seed;
        std::int64_t tick;
        const char* stream_id;
        std::int64_t index;
        const char* expected_hash;
        std::uint64_t expected_folded;
        double expected_unit;
    };
    const Case cases[] = {
        {9007199254740993ll, 1, "purge_bell", 0, "752923df", 1965630431ull, 0.0431},
        {1234567890123456789ll, 7, "mirror_lance", 3, "ce1f06f0", 3458139888ull, 0.9888},
        {-1, 0, "test_seed", 0, "4c3c96ca", 1279039178ull, 0.9178},
        {0, 0, "", 0, "a1c182d5", 2713813717ull, 0.3717},
        {42, 100, "finale_arc", 2, "d9d53196", 3654627734ull, 0.7734},
        {-9223372036854775807ll - 1, 2147483647, "aegis_ward", 2147483647, "133a6119", 322593049ull, 0.3049},
    };
    for (const Case& c : cases) {
        const std::string material =
            std::to_string(c.seed) + ":" + std::to_string(c.tick) + ":" + c.stream_id + ":" + std::to_string(c.index);
        const std::string hash = ShortHashHex(material);
        Check(hash == c.expected_hash, "shortHash mismatch for material " + material);
        // 折叠值 = 十六进制文本 base-16 读入。
        std::uint64_t folded = 0;
        for (char ch : hash) {
            folded = folded * 16u + static_cast<std::uint64_t>(ch <= '9' ? ch - '0' : ch - 'a' + 10);
        }
        Check(folded == c.expected_folded, "folded mismatch for material " + material);
        const double unit = LobbyDeterministicUnit(c.seed, c.tick, c.stream_id, c.index);
        Check(std::fabs(unit - c.expected_unit) < 1e-12, "deterministicUnit mismatch for material " + material);
    }
}

// 同输入必须稳定（tick 集成前提）。
void TestDeterminism() {
    const std::string material = "9007199254740993:1:purge_bell:0";
    Check(ShortHashHex(material) == ShortHashHex(material), "shortHash not stable");
    const double a = LobbyDeterministicUnit(9007199254740993ll, 1, "purge_bell", 0);
    const double b = LobbyDeterministicUnit(9007199254740993ll, 1, "purge_bell", 0);
    Check(a == b, "deterministicUnit not stable");
}

}  // namespace

int main() {
    TestSha256NistVectors();
    TestShortHashParity();
    TestDeterminism();

    if (g_failures != 0) {
        std::cerr << g_failures << " lobby_rng test(s) failed\n";
        return 1;
    }
    std::cout << "lobby_rng tests passed\n";
    return 0;
}
