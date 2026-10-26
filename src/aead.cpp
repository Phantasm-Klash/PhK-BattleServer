// 对战输入面的 AEAD 封装 —— 见 include/phk/battle/aead.hpp 的设计说明。

#include "phk/battle/aead.hpp"

#include <openssl/evp.h>

namespace phk::battle {
namespace {

// ---------------------------------------------------------------------------
// protobuf wire 原语
// ---------------------------------------------------------------------------

constexpr int kWireTypeVarint = 0;
constexpr int kWireTypeLengthDelimited = 2;

struct Cursor {
    const std::vector<std::uint8_t>& bytes;
    std::size_t pos = 0;

    [[nodiscard]] bool exhausted() const { return pos >= bytes.size(); }
    [[nodiscard]] std::size_t remaining() const { return bytes.size() - pos; }
};

/**
 * 读一个 varint。
 *
 * 上限 10 字节是 uint64 的编码上限；超过说明编码非法。不检查"非最小编码"
 * （protobuf 允许），因为信封由我们自己的客户端产生，且严格性应放在"是否
 * 能解出正确的值"上，而不是编码风格上。
 */
[[nodiscard]] bool ReadVarint(Cursor& c, std::uint64_t& out) {
    std::uint64_t value = 0;
    for (int i = 0; i < 10; i += 1) {
        if (c.exhausted()) {
            return false;
        }
        const std::uint64_t byte = c.bytes[c.pos];
        c.pos += 1;
        value |= (byte & 0x7fULL) << (7 * i);
        if ((byte & 0x80) == 0) {
            out = value;
            return true;
        }
    }
    return false;
}

/** 读一个长度分隔的字段，返回其字节区间；长度越界返回 false。 */
[[nodiscard]] bool ReadBytes(Cursor& c, std::vector<std::uint8_t>& out) {
    std::uint64_t length = 0;
    if (!ReadVarint(c, length)) {
        return false;
    }
    if (length > c.remaining()) {
        return false;
    }
    out.assign(c.bytes.begin() + static_cast<std::ptrdiff_t>(c.pos),
               c.bytes.begin() + static_cast<std::ptrdiff_t>(c.pos + length));
    c.pos += length;
    return true;
}

/** 读一个 tag，拆出字段号与 wire type。 */
[[nodiscard]] bool ReadTag(Cursor& c, int& field_number, int& wire_type) {
    std::uint64_t packed = 0;
    if (!ReadVarint(c, packed)) {
        return false;
    }
    field_number = static_cast<int>(packed >> 3);
    wire_type = static_cast<int>(packed & 0x07);
    return field_number > 0;
}

/** 字节 → 小写 hex，与客户端 `aeadBytesToHex` 及 `nonce_hex` 的表示一致。 */
[[nodiscard]] std::string BytesToHex(const std::vector<std::uint8_t>& bytes) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        out.push_back(kDigits[(byte >> 4) & 0x0f]);
        out.push_back(kDigits[byte & 0x0f]);
    }
    return out;
}

/**
 * 解析 `BattlePacketHeader`。
 *
 * 字段号与类型与 `PhK-Protocol/proto/phk/v1/battle.proto` 的
 * `BattlePacketHeader` 一一对应。
 *
 * `version` 子消息**只跳过不解析**：它是信息性的，真正的协议版本校验发生在
 * 解封之后、明文 `BattleInput` 的 `VersionStamp::IsCompatible()` 里。跳过可以
 * 让这里不依赖 `VersionStamp` 的字段布局，少一处需要两边同步的地方。
 */
[[nodiscard]] EnvelopeStatus ParseHeader(
    const std::vector<std::uint8_t>& bytes,
    BattlePacketHeader& header
) {
    Cursor c{bytes};
    while (!c.exhausted()) {
        int field = 0;
        int wire = 0;
        if (!ReadTag(c, field, wire)) {
            return EnvelopeStatus::BadVarint;
        }
        switch (field) {
            case 1: {  // version (message) —— 跳过
                if (wire != kWireTypeLengthDelimited) {
                    return EnvelopeStatus::BadWireType;
                }
                std::vector<std::uint8_t> skipped;
                if (!ReadBytes(c, skipped)) {
                    return EnvelopeStatus::Truncated;
                }
                break;
            }
            case 2: {  // match_id
                if (wire != kWireTypeLengthDelimited) {
                    return EnvelopeStatus::BadWireType;
                }
                std::vector<std::uint8_t> raw;
                if (!ReadBytes(c, raw)) {
                    return EnvelopeStatus::Truncated;
                }
                header.match_id.assign(raw.begin(), raw.end());
                break;
            }
            case 3: {  // player_id
                if (wire != kWireTypeLengthDelimited) {
                    return EnvelopeStatus::BadWireType;
                }
                std::vector<std::uint8_t> raw;
                if (!ReadBytes(c, raw)) {
                    return EnvelopeStatus::Truncated;
                }
                header.player_id.assign(raw.begin(), raw.end());
                break;
            }
            case 4:  // tick
            case 5:  // seq
            case 6:  // ack
            case 7:  // payload_type
            {
                if (wire != kWireTypeVarint) {
                    return EnvelopeStatus::BadWireType;
                }
                std::uint64_t value = 0;
                if (!ReadVarint(c, value)) {
                    return EnvelopeStatus::Truncated;
                }
                if (field == 4) {
                    header.tick = value;
                } else if (field == 5) {
                    header.seq = value;
                } else if (field == 6) {
                    header.ack = value;
                } else {
                    header.payload_type = static_cast<BattlePayloadType>(value & 0xff);
                }
                break;
            }
            case 8: {  // key_id
                if (wire != kWireTypeLengthDelimited) {
                    return EnvelopeStatus::BadWireType;
                }
                std::vector<std::uint8_t> raw;
                if (!ReadBytes(c, raw)) {
                    return EnvelopeStatus::Truncated;
                }
                header.key_id.assign(raw.begin(), raw.end());
                break;
            }
            case 9: {  // nonce (bytes) —— 以 hex 字符串持有，与 DevAeadNonceHex 对齐
                if (wire != kWireTypeLengthDelimited) {
                    return EnvelopeStatus::BadWireType;
                }
                std::vector<std::uint8_t> raw;
                if (!ReadBytes(c, raw)) {
                    return EnvelopeStatus::Truncated;
                }
                header.nonce_hex = BytesToHex(raw);
                break;
            }
            default:
                return EnvelopeStatus::UnexpectedField;
        }
    }
    return EnvelopeStatus::Ok;
}

// ---------------------------------------------------------------------------
// hex 与 AAD
// ---------------------------------------------------------------------------

[[nodiscard]] int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/**
 * 全串是否由 hex 字符组成。
 *
 * 没有复用 `ticket.hpp` 的 `IsHex`：那个符号定义在 `ticket.cpp`，而
 * `ticket.cpp` 又依赖卡片定义表，为一个 hex 判定向整条票据链借依赖不值当。
 */
[[nodiscard]] bool AllHex(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    for (const char c : value) {
        if (HexValue(c) < 0) {
            return false;
        }
    }
    return true;
}

/** 24 hex 字符 → 12 字节 nonce；格式不符返回空。 */
[[nodiscard]] std::vector<std::uint8_t> NonceBytes(std::string_view hex) {
    if (hex.size() != kNonceBytes * 2 || !AllHex(hex)) {
        return {};
    }
    std::vector<std::uint8_t> out;
    out.reserve(kNonceBytes);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int high = HexValue(hex[i]);
        const int low = HexValue(hex[i + 1]);
        if (high < 0 || low < 0) {
            return {};
        }
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return out;
}

void AppendU64Le(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; i += 1) {
        out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xff));
    }
}

/**
 * 客户端 `core/net/battle_crypto.ts` 的 `devBattleAadBytes` 的 C++ 镜像。
 *
 * ★ 注意它与 nonce **不是同一种构造**：nonce 走 FNV-1a 哈希链
 * （`DevAeadNonceHex`），AAD 走字节拼接。两者输入的字段集合与顺序相同，
 * 但算法不同，任何"顺便复用"都会让 tag 永远校验不过。
 *
 * 方向标签必须与发送方一致：客户端封 C→S 帧时用 `client_to_server`。
 */
[[nodiscard]] std::vector<std::uint8_t> AadBytes(
    const BattlePacketHeader& header,
    std::string_view direction
) {
    std::vector<std::uint8_t> out;
    out.reserve(64);
    // 这些 id 在协议里都是 ASCII，std::string 的字节即 UTF-8 字节，与客户端
    // 的 utf8() 编码一致（客户端对非 ASCII 会做真正的 UTF-8 编码，这里若将来
    // 出现非 ASCII id 需要同步改成显式 UTF-8 编码）。
    out.insert(out.end(), direction.begin(), direction.end());
    out.insert(out.end(), header.key_id.begin(), header.key_id.end());
    out.insert(out.end(), header.match_id.begin(), header.match_id.end());
    out.insert(out.end(), header.player_id.begin(), header.player_id.end());
    AppendU64Le(out, static_cast<std::uint64_t>(header.payload_type));
    AppendU64Le(out, header.tick);
    AppendU64Le(out, header.seq);
    AppendU64Le(out, header.ack);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// 公开接口
// ---------------------------------------------------------------------------

EnvelopeResult ParseEnvelope(const std::vector<std::uint8_t>& payload) {
    EnvelopeResult result;
    if (payload.empty()) {
        result.status = EnvelopeStatus::Empty;
        return result;
    }

    Cursor c{payload};
    while (!c.exhausted()) {
        int field = 0;
        int wire = 0;
        if (!ReadTag(c, field, wire)) {
            result.status = EnvelopeStatus::BadVarint;
            return result;
        }
        switch (field) {
            case 1: {  // header
                if (wire != kWireTypeLengthDelimited) {
                    result.status = EnvelopeStatus::BadWireType;
                    return result;
                }
                std::vector<std::uint8_t> raw;
                if (!ReadBytes(c, raw)) {
                    result.status = EnvelopeStatus::Truncated;
                    return result;
                }
                const EnvelopeStatus status = ParseHeader(raw, result.packet.header);
                if (status != EnvelopeStatus::Ok) {
                    result.status = status;
                    return result;
                }
                break;
            }
            case 2: {  // ciphertext
                if (wire != kWireTypeLengthDelimited) {
                    result.status = EnvelopeStatus::BadWireType;
                    return result;
                }
                if (!ReadBytes(c, result.packet.ciphertext)) {
                    result.status = EnvelopeStatus::Truncated;
                    return result;
                }
                break;
            }
            case 3: {  // auth_tag
                if (wire != kWireTypeLengthDelimited) {
                    result.status = EnvelopeStatus::BadWireType;
                    return result;
                }
                if (!ReadBytes(c, result.packet.auth_tag)) {
                    result.status = EnvelopeStatus::Truncated;
                    return result;
                }
                break;
            }
            default:
                result.status = EnvelopeStatus::UnexpectedField;
                return result;
        }
    }

    result.status = EnvelopeStatus::Ok;
    return result;
}

AeadResult OpenEnvelope(
    const BattleEncryptedPacket& packet,
    std::string_view expected_key_id,
    const std::vector<std::uint8_t>& session_key
) {
    AeadResult result;

    // 1. key id —— 最便宜的一次比对，先挡掉密钥轮换期到达的旧包。
    if (packet.header.key_id != expected_key_id) {
        result.status = AeadStatus::KeyIdMismatch;
        return result;
    }
    // 2. tag 形状（HasExpectedAeadTagShape）。
    if (packet.auth_tag.size() != kAuthTagBytes) {
        result.status = AeadStatus::BadTagLength;
        return result;
    }
    // 3. nonce —— 客户端按同一约定从 header 派生，不一致即伪造或篡改。
    if (packet.header.nonce_hex != DevAeadNonceHex(packet.header)) {
        result.status = AeadStatus::NonceMismatch;
        return result;
    }
    const std::vector<std::uint8_t> nonce = NonceBytes(packet.header.nonce_hex);
    if (nonce.size() != kNonceBytes) {
        result.status = AeadStatus::NonceMismatch;
        return result;
    }
    if (session_key.size() != kSessionKeyBytes) {
        result.status = AeadStatus::NoKeyForPlayer;
        return result;
    }

    // 4. ChaCha20-Poly1305。tag 校验由 EVP_DecryptFinal_ex 完成。
    result.plaintext = ChaCha20Poly1305Open(
        session_key,
        nonce,
        AadBytes(packet.header, "client_to_server"),
        packet.ciphertext,
        packet.auth_tag
    );
    result.status = result.plaintext.empty() && !packet.ciphertext.empty()
        ? AeadStatus::DecryptFailed
        : AeadStatus::Ok;
    return result;
}

std::vector<std::uint8_t> ChaCha20Poly1305Open(
    const std::vector<std::uint8_t>& key,
    const std::vector<std::uint8_t>& nonce,
    const std::vector<std::uint8_t>& aad,
    const std::vector<std::uint8_t>& ciphertext,
    const std::vector<std::uint8_t>& auth_tag
) {
    if (key.size() != kSessionKeyBytes || nonce.size() != kNonceBytes ||
        auth_tag.size() != kAuthTagBytes) {
        return {};
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        return {};
    }

    bool ok = EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1;
    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(
                 ctx, EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) == 1;
    }
    if (ok) {
        ok = EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) == 1;
    }

    int written = 0;
    if (ok && !aad.empty()) {
        // AAD 单独喂一次，不产出明文。漏掉这一步不会报错，只是 tag 永远对不上。
        ok = EVP_DecryptUpdate(ctx, nullptr, &written, aad.data(), static_cast<int>(aad.size())) == 1;
    }

    std::vector<std::uint8_t> plaintext(ciphertext.size(), 0);
    if (ok && !ciphertext.empty()) {
        ok = EVP_DecryptUpdate(ctx, plaintext.data(), &written,
                               ciphertext.data(), static_cast<int>(ciphertext.size())) == 1;
    }
    int plain_len = ok ? written : 0;

    if (ok) {
        // 期望的 tag 必须在 Final 之前交给 ctx。
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, static_cast<int>(auth_tag.size()),
                                 const_cast<std::uint8_t*>(auth_tag.data())) == 1;
    }
    if (ok) {
        int final_written = 0;
        ok = EVP_DecryptFinal_ex(ctx, plaintext.data() + plain_len, &final_written) == 1;
        plain_len += ok ? final_written : 0;
    }
    EVP_CIPHER_CTX_free(ctx);

    if (!ok) {
        return {};
    }
    plaintext.resize(static_cast<std::size_t>(plain_len));
    return plaintext;
}

std::string_view EnvelopeResult::reason() const {
    switch (status) {
        case EnvelopeStatus::Ok:
            return "ok";
        case EnvelopeStatus::Empty:
            return "envelope_empty";
        case EnvelopeStatus::Truncated:
            return "envelope_truncated";
        case EnvelopeStatus::BadWireType:
            return "envelope_bad_wire_type";
        case EnvelopeStatus::BadVarint:
            return "envelope_bad_varint";
        case EnvelopeStatus::UnexpectedField:
            return "envelope_unexpected_field";
    }
    return "envelope_unknown";
}

std::string_view AeadResult::reason() const {
    switch (status) {
        case AeadStatus::Ok:
            return "ok";
        case AeadStatus::NoKeyForPlayer:
            return "aead_no_key_for_player";
        case AeadStatus::KeyIdMismatch:
            return "aead_key_id_mismatch";
        case AeadStatus::BadTagLength:
            return "aead_bad_tag_length";
        case AeadStatus::NonceMismatch:
            return "aead_nonce_mismatch";
        case AeadStatus::DecryptFailed:
            return "aead_decrypt_failed";
    }
    return "aead_unknown";
}

}  // namespace phk::battle
