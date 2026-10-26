#pragma once

// 对战输入面的 AEAD 封装（docs/battle-wire-scheme.md §P2）。
//
// 客户端把 `BattleInput` 装进 `BattleEncryptedPacket`（battle.proto），用大厅
// 随票据下发的会话密钥封起来；本模块负责拆信封并解封。两个刻意的选择：
//
//  1. **手写 protobuf wire 解析，不引 protobuf 运行时。** 本仓 CMake 只依赖
//     ZLIB，`third_party/` 只有 ikcp 和协议常量清单，没有序列化代码。而信封
//     只有 3 个字段、header 9 个，手写比拖进一个运行时轻得多。
//  2. **密码学走 OpenSSL，不手写。** gateway 上有 OpenSSL 3.0.13（头文件与
//     pkg-config 齐全），ChaCha20-Poly1305 通过 EVP 高级接口取。自己实现
//     AEAD 是这类代码最容易出错的地方，没有理由手写。
//
// 密钥粒度：**每个玩家、每场对局一把**（用户 2026-10-26 决定）。因此解封入口
// 按 player_id 取密钥，一次查不到就是拒绝——不做"退而求其次用默认密钥"。
//
// 兼容性铁律：未配置任何会话密钥时，调用方必须完全走原来的明文路径。本模块
// 在这种情况下不会被调用，收包行为与加密存在前**逐字节一致**。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "phk/battle/protocol.hpp"

namespace phk::battle {

// ---------------------------------------------------------------------------
// 信封：BattleEncryptedPacket 的字节编解码
// ---------------------------------------------------------------------------

enum class EnvelopeStatus {
    Ok,
    /** 空载荷：连一个 tag 都没有。 */
    Empty,
    /** 长度前缀越过载荷末尾。 */
    Truncated,
    /** 字段的 wire type 不是本模块会产生的那几种（varint / 长度分隔）。 */
    BadWireType,
    /** varint 编码超过 10 字节，或数值溢出 64 位。 */
    BadVarint,
    /** 信封里出现了 1/2/3 之外的字段号。未知字段本可跳过，但这里选择拒绝：
     *  信封是攻击面，宁可让一个未来的字段显式地打破兼容，也不要静默放行。 */
    UnexpectedField,
};

struct EnvelopeResult {
    EnvelopeStatus status = EnvelopeStatus::Empty;
    BattleEncryptedPacket packet;

    [[nodiscard]] bool ok() const { return status == EnvelopeStatus::Ok; }
    /** 日志与统计用的稳定字符串，不要拿来做控制流。 */
    [[nodiscard]] std::string_view reason() const;
};

/**
 * 解析一个 `BattleEncryptedPacket`。
 *
 * 严格解析：越界、坏 varint、非常见 wire type、意外字段一律拒绝并返回状态，
 * 不抛异常、不做"尽力解析"。
 */
[[nodiscard]] EnvelopeResult ParseEnvelope(const std::vector<std::uint8_t>& payload);

// ---------------------------------------------------------------------------
// 解封：ChaCha20-Poly1305（RFC 8439）
// ---------------------------------------------------------------------------

enum class AeadStatus {
    Ok,
    /** 该玩家没有配置会话密钥 —— 调用方应判为"不该收到信封"。 */
    NoKeyForPlayer,
    /** header.key_id 与本进程配置的 key id 不一致。先于解密比对，省掉开销。 */
    KeyIdMismatch,
    /** auth_tag 不是 16 字节（HasExpectedAeadTagShape）。 */
    BadTagLength,
    /** nonce 与 header 派生的 DevAeadNonceHex 不一致 —— 伪造或篡改。 */
    NonceMismatch,
    /** OpenSSL 解密或 tag 校验失败。 */
    DecryptFailed,
};

struct AeadResult {
    AeadStatus status = AeadStatus::NoKeyForPlayer;
    std::vector<std::uint8_t> plaintext;

    [[nodiscard]] bool ok() const { return status == AeadStatus::Ok; }
    [[nodiscard]] std::string_view reason() const;
};

/**
 * 打开一个已解析的信封。
 *
 * 校验顺序是刻意的，任何一步失败都立刻返回（不做多余工作）：
 *
 *   1. `key_id` 与本进程配置的一致（最便宜，先挡掉密钥轮换期的旧包）
 *   2. `auth_tag` 长度 == 16
 *   3. nonce == `DevAeadNonceHex(header)` —— 客户端用同样的约定派生，
 *      且 nonce 由 header 决定，因此它同时是防重放的第一次过滤
 *      （真正的重放由调用方的 seq 表与 seen_encrypted_nonces_ 兜住）
 *   4. ChaCha20-Poly1305 解密，AEAD 的 tag 校验同步完成
 *
 * @param packet          {@link ParseEnvelope} 的产物
 * @param expected_key_id 本对局配置的 key id
 * @param session_key     该玩家的 32 字节会话密钥
 */
[[nodiscard]] AeadResult OpenEnvelope(
    const BattleEncryptedPacket& packet,
    std::string_view expected_key_id,
    const std::vector<std::uint8_t>& session_key
);

/**
 * ChaCha20-Poly1305 解密（RFC 8439）。
 *
 * 单独暴露的用意是**可测**：`OpenEnvelope` 的 nonce 与 AAD 都由 header 派生，
 * 没法塞进任意值，于是标准测试向量就无法直接喂给它。把密码学核心抽出来后，
 * 可以用 RFC 8439 §2.8.2 的向量独立验证 OpenSSL 的用法是否正确——这一步不能
 * 跳过，AEAD 的用法错误往往表现为"能解密但 tag 永远不过"或"能过但 AAD 没参与"。
 *
 * @return 明文；密钥/nonce/tag 长度不符或 tag 校验失败时返回空向量。
 */
[[nodiscard]] std::vector<std::uint8_t> ChaCha20Poly1305Open(
    const std::vector<std::uint8_t>& key,
    const std::vector<std::uint8_t>& nonce,
    const std::vector<std::uint8_t>& aad,
    const std::vector<std::uint8_t>& ciphertext,
    const std::vector<std::uint8_t>& auth_tag
);

/** 会话密钥长度（RFC 8439 的 256 位密钥）。 */
inline constexpr std::size_t kSessionKeyBytes = 32;

/** AEAD tag 长度（Poly1305）。 */
inline constexpr std::size_t kAuthTagBytes = 16;

/** nonce 长度（RFC 8439 的 96 位 nonce）。 */
inline constexpr std::size_t kNonceBytes = 12;

}  // namespace phk::battle
