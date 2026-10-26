// 对战输入面 AEAD 的测试。
//
// 三层，各自回答一个不同的问题：
//
//  1. RFC 8439 §2.8.2 官方向量 —— OpenSSL 的用法对不对。这一步不能省：AEAD
//     的接口误用（漏喂 AAD、tag 设置时机不对）不会报错，只会让 tag 永远
//     校验不过，或者更糟——AAD 根本没参与认证而我们看不出来。
//  2. 信封解析 —— 手写的 protobuf wire 解码对不对，以及它对畸形输入是否
//     拒绝而不是崩溃或"尽力解析"。
//  3. 端到端解封 —— nonce/AAD 的派生约定与客户端是否一致（跨端互操作），
//     以及每条校验分支是否真的会拦住对应的伪造。

#include <openssl/evp.h>

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "phk/battle/aead.hpp"
#include "phk/battle/match_lifecycle.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    if (condition) {
        std::cout << "  ok   " << what << "\n";
        return;
    }
    std::cout << "  FAIL " << what << "\n";
    g_failures += 1;
}

std::vector<std::uint8_t> FromHex(const std::string& hex) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const std::string byte = hex.substr(i, 2);
        out.push_back(static_cast<std::uint8_t>(std::stoul(byte, nullptr, 16)));
    }
    return out;
}

std::vector<std::uint8_t> FromText(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

// ---------------------------------------------------------------------------
// 1. RFC 8439 §2.8.2 测试向量
// ---------------------------------------------------------------------------

void TestRfc8439Vector() {
    std::cout << "# aead: RFC 8439 2.8.2 vector\n";

    const std::vector<std::uint8_t> key = FromHex(
        "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    const std::vector<std::uint8_t> nonce = FromHex("070000004041424344454647");
    const std::vector<std::uint8_t> aad = FromHex("50515253c0c1c2c3c4c5c6c7");
    const std::string plaintext_text =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one "
        "tip for the future, sunscreen would be it.";
    const std::vector<std::uint8_t> ciphertext = FromHex(
                "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
        "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
        "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
        "3ff4def08e4b7a9de576d26586cec64b6116"
    );
    const std::vector<std::uint8_t> tag = FromHex("1ae10b594f09e26a7e902ecbd0600691");

    // 手抄向量很容易漏掉一个 hex 字符，而漏了之后的表现只是"解不出来"，
    // 看不出是向量错了还是实现错了。先钉长度。
    Check(ciphertext.size() == plaintext_text.size(),
          "the vector's ciphertext is the same length as its plaintext (114)");
    Check(ciphertext.size() == 114, "the vector's ciphertext is 114 bytes");

    const std::vector<std::uint8_t> opened =
        phk::battle::ChaCha20Poly1305Open(key, nonce, aad, ciphertext, tag);

    Check(!opened.empty(), "the official vector decrypts");
    Check(opened == FromText(plaintext_text), "and the plaintext matches byte for byte");

    // 反例：改一个 tag 字节必须失败。这是"tag 真的参与了校验"的证明。
    std::vector<std::uint8_t> bad_tag = tag;
    bad_tag[0] ^= 0xff;
    Check(phk::battle::ChaCha20Poly1305Open(key, nonce, aad, ciphertext, bad_tag).empty(),
          "a flipped auth tag is rejected");

    // 反例：改 AAD 必须失败。这是"AAD 真的进了认证"的证明——漏喂 AAD 的
    // 实现能通过 tag 校验，却对 header 篡改毫无防备。
    std::vector<std::uint8_t> bad_aad = aad;
    if (!bad_aad.empty()) {
        bad_aad[0] ^= 0xff;
    }
    Check(phk::battle::ChaCha20Poly1305Open(key, nonce, bad_aad, ciphertext, tag).empty(),
          "a modified AAD is rejected (proves the AAD is authenticated)");

    // 长度不符直接拒绝，不进 OpenSSL。
    Check(phk::battle::ChaCha20Poly1305Open(key, nonce, aad, ciphertext,
                                            std::vector<std::uint8_t>(15, 0)).empty(),
          "a 15-byte tag is refused");
    Check(phk::battle::ChaCha20Poly1305Open(std::vector<std::uint8_t>(16, 0), nonce, aad,
                                            ciphertext, tag).empty(),
          "a 16-byte key is refused");
}

// ---------------------------------------------------------------------------
// 信封编码小工具（测试用；生产侧由客户端产生）
// ---------------------------------------------------------------------------

void AppendVarint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

void AppendTag(std::vector<std::uint8_t>& out, int field, int wire) {
    AppendVarint(out, (static_cast<std::uint64_t>(field) << 3) | static_cast<std::uint64_t>(wire));
}

void AppendLengthDelimited(std::vector<std::uint8_t>& out, int field,
                           const std::vector<std::uint8_t>& bytes) {
    AppendTag(out, field, 2);
    AppendVarint(out, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void AppendVarintField(std::vector<std::uint8_t>& out, int field, std::uint64_t value) {
    AppendTag(out, field, 0);
    AppendVarint(out, value);
}

/** 构造一个 header（版本子消息留空，与 ParseHeader 的"跳过"约定一致）。 */
std::vector<std::uint8_t> BuildHeader(const phk::battle::BattlePacketHeader& h) {
    std::vector<std::uint8_t> out;
    AppendLengthDelimited(out, 1, {});  // version: empty message
    AppendLengthDelimited(out, 2, FromText(h.match_id));
    AppendLengthDelimited(out, 3, FromText(h.player_id));
    AppendVarintField(out, 4, h.tick);
    AppendVarintField(out, 5, h.seq);
    AppendVarintField(out, 6, h.ack);
    AppendVarintField(out, 7, static_cast<std::uint64_t>(h.payload_type));
    AppendLengthDelimited(out, 8, FromText(h.key_id));
    AppendLengthDelimited(out, 9, FromHex(h.nonce_hex));
    return out;
}

std::vector<std::uint8_t> BuildEnvelope(const phk::battle::BattlePacketHeader& h,
                                        const std::vector<std::uint8_t>& ciphertext,
                                        const std::vector<std::uint8_t>& tag) {
    std::vector<std::uint8_t> out;
    AppendLengthDelimited(out, 1, BuildHeader(h));
    AppendLengthDelimited(out, 2, ciphertext);
    AppendLengthDelimited(out, 3, tag);
    return out;
}

// ---------------------------------------------------------------------------
// 2. 信封解析
// ---------------------------------------------------------------------------

void TestEnvelopeParsing() {
    std::cout << "# aead: envelope parsing\n";

    phk::battle::BattlePacketHeader header;
    header.match_id = "match-1";
    header.player_id = "p-1";
    header.tick = 7;
    header.seq = 42;
    header.ack = 0;
    header.payload_type = phk::battle::BattlePayloadType::Input;
    header.key_id = "bsess-1";
    header.nonce_hex = "00112233445566778899aabb";

    const std::vector<std::uint8_t> wire =
        BuildEnvelope(header, FromText("ciphertext-bytes"), std::vector<std::uint8_t>(16, 0xAB));

    const phk::battle::EnvelopeResult parsed = phk::battle::ParseEnvelope(wire);
    Check(parsed.ok(), "a well-formed envelope parses");
    Check(parsed.packet.header.match_id == "match-1", "match_id round-trips");
    Check(parsed.packet.header.player_id == "p-1", "player_id round-trips");
    Check(parsed.packet.header.tick == 7u, "tick round-trips");
    Check(parsed.packet.header.seq == 42u, "seq round-trips");
    Check(parsed.packet.header.key_id == "bsess-1", "key_id round-trips");
    Check(parsed.packet.header.nonce_hex == "00112233445566778899aabb", "nonce round-trips to hex");
    Check(parsed.packet.ciphertext == FromText("ciphertext-bytes"), "ciphertext round-trips");
    Check(parsed.packet.auth_tag.size() == 16, "auth_tag is 16 bytes");

    // 畸形输入必须被拒绝，不能崩溃也不能"尽力解析"。
    Check(phk::battle::ParseEnvelope({}).reason() == std::string("envelope_empty"),
          "an empty payload is refused");

    std::vector<std::uint8_t> truncated = wire;
    truncated.resize(wire.size() - 4);
    Check(!phk::battle::ParseEnvelope(truncated).ok(), "a truncated envelope is refused");

    // 长度前缀越界：声称 200 字节却只有 3 个。
    std::vector<std::uint8_t> overlong;
    AppendTag(overlong, 2, 2);
    AppendVarint(overlong, 200);
    overlong.push_back(1);
    overlong.push_back(2);
    overlong.push_back(3);
    Check(!phk::battle::ParseEnvelope(overlong).ok(), "an over-long length prefix is refused");

    // 未知字段号（4）在信封层属于意外，拒绝而不是跳过。
    std::vector<std::uint8_t> extra_field = wire;
    AppendLengthDelimited(extra_field, 4, FromText("surprise"));
    Check(phk::battle::ParseEnvelope(extra_field).reason() ==
              std::string("envelope_unexpected_field"),
          "an unexpected envelope field is refused, not skipped");
}

// ---------------------------------------------------------------------------
// 3. 端到端解封：每条校验分支
// ---------------------------------------------------------------------------

void TestOpenEnvelope() {
    std::cout << "# aead: opening a sealed frame\n";

    phk::battle::BattlePacketHeader header;
    header.match_id = "match-1";
    header.player_id = "p-1";
    header.tick = 7;
    header.seq = 1;
    header.payload_type = phk::battle::BattlePayloadType::Input;
    header.key_id = "bsess-1";
    // nonce 必须由 header 派生，与客户端 devBattleNonceHex 同法。
    header.nonce_hex = phk::battle::DevAeadNonceHex(header);

    const std::vector<std::uint8_t> key(32, 0x55);
    const std::vector<std::uint8_t> nonce_bytes = FromHex(header.nonce_hex);
    Check(nonce_bytes.size() == phk::battle::kNonceBytes,
          "the derived nonce is 24 hex characters (12 bytes)");

    // AAD 的构造必须与客户端一致，否则解密出来的 tag 对不上。这里复算一次：
    // direction ‖ key_id ‖ match_id ‖ player_id ‖ u64le(payload_type, tick, seq, ack)
    std::vector<std::uint8_t> aad;
    const std::string direction = "client_to_server";
    aad.insert(aad.end(), direction.begin(), direction.end());
    aad.insert(aad.end(), header.key_id.begin(), header.key_id.end());
    aad.insert(aad.end(), header.match_id.begin(), header.match_id.end());
    aad.insert(aad.end(), header.player_id.begin(), header.player_id.end());
    for (int i = 0; i < 8; i += 1) {
        aad.push_back(static_cast<std::uint8_t>(
            (static_cast<std::uint64_t>(header.payload_type) >> (8 * i)) & 0xff));
    }
    const std::uint64_t fields[] = {header.tick, header.seq, header.ack};
    for (const std::uint64_t value : fields) {
        for (int i = 0; i < 8; i += 1) {
            aad.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xff));
        }
    }

    // 用 OpenSSL 自己封一个，模拟客户端的封包。
    const std::vector<std::uint8_t> plaintext = FromText("sealed-input-frame");
    std::vector<std::uint8_t> ciphertext(plaintext.size(), 0);
    std::vector<std::uint8_t> tag(16, 0);
    {
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        int written = 0;
        EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr);
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN,
                            static_cast<int>(nonce_bytes.size()), nullptr);
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce_bytes.data());
        EVP_EncryptUpdate(ctx, nullptr, &written, aad.data(), static_cast<int>(aad.size()));
        EVP_EncryptUpdate(ctx, ciphertext.data(), &written, plaintext.data(),
                          static_cast<int>(plaintext.size()));
        EVP_EncryptFinal_ex(ctx, ciphertext.data() + written, &written);
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag.data());
        EVP_CIPHER_CTX_free(ctx);
    }

    const phk::battle::EnvelopeResult parsed =
        phk::battle::ParseEnvelope(BuildEnvelope(header, ciphertext, tag));
    Check(parsed.ok(), "the sealed frame parses");

    const phk::battle::AeadResult opened =
        phk::battle::OpenEnvelope(parsed.packet, "bsess-1", key);
    Check(opened.ok(), "and opens");
    Check(opened.plaintext == plaintext, "recovering the original input frame");

    // --- 每条校验分支 ---------------------------------------------------
    Check(phk::battle::OpenEnvelope(parsed.packet, "bsess-OTHER", key).reason() ==
              std::string("aead_key_id_mismatch"),
          "a key id from a previous rotation is rejected");

    phk::battle::BattlePacketHeader wrong_nonce = header;
    wrong_nonce.nonce_hex = "0000000000000000000000ff";
    Check(phk::battle::OpenEnvelope(
              phk::battle::ParseEnvelope(BuildEnvelope(wrong_nonce, ciphertext, tag)).packet,
              "bsess-1", key).reason() == std::string("aead_nonce_mismatch"),
          "a nonce that does not match its header is rejected");

    Check(phk::battle::OpenEnvelope(
              phk::battle::ParseEnvelope(
                  BuildEnvelope(header, ciphertext, std::vector<std::uint8_t>(8, 0))).packet,
              "bsess-1", key).reason() == std::string("aead_bad_tag_length"),
          "an 8-byte auth tag is rejected");

    Check(phk::battle::OpenEnvelope(parsed.packet, "bsess-1", std::vector<std::uint8_t>(16, 0))
              .reason() == std::string("aead_no_key_for_player"),
          "a player with no session key is rejected");

    // 密文被篡改 → tag 校验失败。
    std::vector<std::uint8_t> tampered_ciphertext = ciphertext;
    if (!tampered_ciphertext.empty()) {
        tampered_ciphertext[0] ^= 0xff;
    }
    Check(phk::battle::OpenEnvelope(
              phk::battle::ParseEnvelope(BuildEnvelope(header, tampered_ciphertext, tag)).packet,
              "bsess-1", key).reason() == std::string("aead_decrypt_failed"),
          "a tampered ciphertext fails the tag check");
}

// ---------------------------------------------------------------------------
// 4. 收包路径：向后兼容与降级防护
//
// 这两条是 Step 1 的全部意义所在 —— 能力先落地、行为不变。没有它们，"未配置
// 时逐字节一致"就只是一句注释。
// ---------------------------------------------------------------------------

phk::battle::BattleInput MakeInput(const std::string& match_id, const std::string& player_id,
                                   std::uint64_t tick, std::uint64_t seq) {
    phk::battle::BattleInput in;
    in.match_id = match_id;
    in.player_id = player_id;
    in.tick = tick;
    in.seq = seq;
    in.card_slot = -1;
    return in;
}

phk::battle::MatchLifecycleConfig MakeConfig() {
    phk::battle::MatchLifecycleConfig config;
    config.match_id = "match-1";
    config.mode_id = "mvp_boss_race";
    config.player_ids = {"p-1", "p-2"};
    config.require_ticket = phk::battle::MatchTicketRequirement::Off;
    return config;
}

void TestReceivePath() {
    std::cout << "# aead: the receive path with and without a key\n";

    // --- 未配置密钥：明文输入照常接受，与加密存在之前逐字节一致 -----------
    {
        phk::battle::MatchServer server(MakeConfig());
        std::string error;
        Check(server.Start(&error), "no key: the server starts (" + error + ")");

        const auto payload =
            phk::battle::EncodeBattleInputPayload(MakeInput("match-1", "p-1", 1, 1));
        Check(server.InjectPayload(0x0000A011u, payload), "no key: a plaintext frame is injected");
        Check(server.BoundPlayerCount() == 1, "no key: the player binds");
        Check(server.AcceptedInputCount() == 1, "no key: the input is accepted");
        server.Stop();
    }

    // --- 配置密钥：明文输入必须被拒（降级防护）-----------------------------
    {
        auto config = MakeConfig();
        config.session_key_id = "bsess-1";
        config.session_keys_by_player["p-1"] = std::vector<std::uint8_t>(32, 0x55);
        phk::battle::MatchServer server(config);
        std::string error;
        Check(server.Start(&error), "sealed: the server starts (" + error + ")");

        const auto payload =
            phk::battle::EncodeBattleInputPayload(MakeInput("match-1", "p-1", 1, 1));
        server.InjectPayload(0x0000A011u, payload);
        Check(server.BoundPlayerCount() == 0,
              "sealed: a plaintext frame is rejected, not downgraded");
        Check(!server.TicketAuditLog().empty() &&
                  server.TicketAuditLog().back().reason == "plaintext_input_rejected",
              "sealed: and the rejection says why");
        server.Stop();
    }

    // --- 配置密钥：信封被打开、输入被接受 ---------------------------------
    {
        auto config = MakeConfig();
        config.session_key_id = "bsess-1";
        const std::vector<std::uint8_t> key(32, 0x55);
        config.session_keys_by_player["p-1"] = key;
        phk::battle::MatchServer server(config);
        std::string error;
        Check(server.Start(&error), "sealed: the server starts (" + error + ")");

        // 按客户端的约定造一个密封帧。
        phk::battle::BattlePacketHeader header;
        header.match_id = "match-1";
        header.player_id = "p-1";
        header.tick = 1;
        header.seq = 1;
        header.payload_type = phk::battle::BattlePayloadType::Input;
        header.key_id = "bsess-1";
        header.nonce_hex = phk::battle::DevAeadNonceHex(header);

        const std::vector<std::uint8_t> nonce = FromHex(header.nonce_hex);
        std::vector<std::uint8_t> aad;
        const std::string direction = "client_to_server";
        aad.insert(aad.end(), direction.begin(), direction.end());
        aad.insert(aad.end(), header.key_id.begin(), header.key_id.end());
        aad.insert(aad.end(), header.match_id.begin(), header.match_id.end());
        aad.insert(aad.end(), header.player_id.begin(), header.player_id.end());
        for (int i = 0; i < 8; i += 1) {
            aad.push_back(static_cast<std::uint8_t>(
                (static_cast<std::uint64_t>(header.payload_type) >> (8 * i)) & 0xff));
        }
        const std::uint64_t triple[] = {header.tick, header.seq, header.ack};
        for (const std::uint64_t value : triple) {
            for (int i = 0; i < 8; i += 1) {
                aad.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xff));
            }
        }

        const std::vector<std::uint8_t> body =
            phk::battle::EncodeBattleInputPayload(MakeInput("match-1", "p-1", 1, 1));
        std::vector<std::uint8_t> ciphertext(body.size(), 0);
        std::vector<std::uint8_t> tag(16, 0);
        {
            EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
            int written = 0;
            EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr);
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(nonce.size()), nullptr);
            EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data());
            EVP_EncryptUpdate(ctx, nullptr, &written, aad.data(), static_cast<int>(aad.size()));
            EVP_EncryptUpdate(ctx, ciphertext.data(), &written, body.data(),
                              static_cast<int>(body.size()));
            EVP_EncryptFinal_ex(ctx, ciphertext.data() + written, &written);
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag.data());
            EVP_CIPHER_CTX_free(ctx);
        }

        server.InjectPayload(0x0000A011u, BuildEnvelope(header, ciphertext, tag));
        Check(server.BoundPlayerCount() == 1, "sealed: the sealed frame binds the player");
        Check(server.AcceptedInputCount() == 1, "sealed: and its input is accepted");
        server.Stop();
    }
}

}  // namespace

int main() {
    TestRfc8439Vector();
    TestEnvelopeParsing();
    TestOpenEnvelope();
    TestReceivePath();
    if (g_failures == 0) {
        std::cout << "\naead tests: all passed\n";
        return 0;
    }
    std::cout << "\naead tests: " << g_failures << " failed\n";
    return 1;
}
