// .dem v1 容器 + DemRecorder 单元测试（thread B）。
// 覆盖：
//   ① 容器 roundtrip（Write → Parse 一致，header/条目逐字段）
//   ② 篡改检测：magic / 长度 / sha256 / zlib 坏流 / header 过大 / 链断
//   ③ header JSON 往返（字段顺序 + players）
//   ④ 空链
//   ⑤ 超长字段（player_id > u16 限）
//   ⑥ DemRecorder 链序 = 接受序（payload 与链对齐）
//   ⑦ WriteDemDocument 一致性护栏（entry_count / head_digest / payload 数）
//   ⑧ 编码原语金标准（sha256 / md5 / base64）与本地路径防护
//
// 版权：东方同人逆向成果，仅用于自研玩法。

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "phk/battle/chain_log.hpp"
#include "phk/battle/dem.hpp"
#include "phk/battle/lobby_rng.hpp"
#include "phk/battle/protocol.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& label) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << label << "\n";
    }
}

using phk::battle::BattleInput;
using phk::battle::BattleModeAction;
using phk::battle::CanonicalChainInputPayload;
using phk::battle::CanonicalChainModeActionPayload;
using phk::battle::CanonicalChainSeedPayload;
using phk::battle::ChainEntry;
using phk::battle::ChainPayloadDigest;
using phk::battle::DemBase64Encode;
using phk::battle::DemBodyEntry;
using phk::battle::DemHeader;
using phk::battle::DemLocalPath;
using phk::battle::DemMd5Hex;
using phk::battle::DemPlayer;
using phk::battle::DemPlayerLoadout;
using phk::battle::DemRecorder;
using phk::battle::DemSha256Hex;
using phk::battle::DecodeDemBody;
using phk::battle::DecodeDemHeaderJson;
using phk::battle::DemUploadEnabled;
using phk::battle::DemZlibCompress;
using phk::battle::EncodeDemBody;
using phk::battle::EncodeDemHeaderJson;
using phk::battle::kChainGenesisPrevDigest;
using phk::battle::kChainSeedKindBattleDerived;
using phk::battle::kDemFieldMaxBytes;
using phk::battle::kDemMagic;
using phk::battle::ParseDemDocument;
using phk::battle::Sha256Bytes;
using phk::battle::WriteDemDocument;

void AppendU32(std::string& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xffu));
    }
}

void AppendU64(std::string& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xffu));
    }
}

// 手工拼一个容器（用于篡改场景；正常路径走 WriteDemDocument）。
std::string BuildContainer(const std::string& header_json, const std::string& compressed) {
    std::string out;
    out.append(kDemMagic, 8);
    AppendU32(out, static_cast<std::uint32_t>(header_json.size()));
    out += header_json;
    AppendU64(out, static_cast<std::uint64_t>(compressed.size()));
    out += compressed;
    const std::array<std::uint8_t, 32> digest = Sha256Bytes(header_json + compressed);
    out.append(reinterpret_cast<const char*>(digest.data()), digest.size());
    return out;
}

DemHeader BaseHeader() {
    DemHeader header;
    header.match_id = "match-dem-1";
    header.mode_id = "pvp_duel";
    header.boss_id = "boss-sanae";
    header.ruleset_version = "ruleset-v1";
    header.server_seed = "12345";
    header.tick_rate_hz = 60;
    header.final_tick = 120;
    header.final_state_hash = "deadbeefdeadbeef";
    header.boss_max_hp = 300;
    DemPlayerLoadout loadout;
    loadout.character_id = "reimu";
    loadout.shot_type = "spread";
    loadout.bomb_type = "fantasy_seal";
    DemPlayer player;
    player.player_id = "p1";
    player.user_id = "user-1";
    player.loadout = loadout;
    header.players.push_back(player);
    header.created_at = 1700000000;
    return header;
}

// seed genesis + 3 input + 1 mode action，返回录制器与规范化 payload。
DemRecorder SampleRecorder(std::vector<std::string>* payloads_out) {
    DemRecorder recorder;
    recorder.RecordMatchSeed(12345);
    for (int index = 0; index < 3; ++index) {
        BattleInput input;
        input.match_id = "match-dem-1";
        input.player_id = index == 2 ? "p2" : "p1";
        input.tick = 10 + index;
        input.seq = static_cast<std::uint64_t>(index + 1);
        input.direction_bits = static_cast<std::uint32_t>(index);
        input.slow = index == 0;
        input.shoot = true;
        input.bomb = index == 2;
        input.card_slot = index == 1 ? 2 : -1;
        input.mode_action_id = index == 1 ? "mode-action-1" : "";
        recorder.RecordInput(input);
    }
    BattleModeAction action;
    action.match_id = "match-dem-1";
    action.player_id = "p1";
    action.tick = 20;
    action.seq = 9;
    action.action_id = "action-cast-1";
    action.action_type = "cast_card";
    action.payload_json = "{\"card_slot\":1}";
    recorder.RecordModeAction(action);

    if (payloads_out != nullptr) {
        payloads_out->clear();
        payloads_out->push_back(CanonicalChainSeedPayload(std::string(kChainSeedKindBattleDerived), 12345));
        for (int index = 0; index < 3; ++index) {
            payloads_out->push_back(CanonicalChainInputPayload(
                static_cast<std::uint64_t>(index + 1),
                static_cast<std::uint32_t>(index),
                index == 0,
                true,
                index == 2,
                index == 1 ? 2 : -1,
                index == 1 ? "mode-action-1" : ""));
        }
        payloads_out->push_back(CanonicalChainModeActionPayload("action-cast-1", 9, "{\"card_slot\":1}"));
    }
    return recorder;
}

DemHeader SampleHeader(const DemRecorder& recorder) {
    DemHeader header = BaseHeader();
    header.entry_count = recorder.Size();
    header.chain_head_digest = recorder.Chain().HeadDigest();
    return header;
}

// ---- ① 容器 roundtrip ----

void TestRoundtrip() {
    std::vector<std::string> payloads;
    DemRecorder recorder = SampleRecorder(&payloads);
    const DemHeader header = SampleHeader(recorder);
    const auto write = recorder.Finalize(header);
    Check(write.ok, "roundtrip finalize ok: " + write.reason);
    if (!write.ok) return;

    const auto parse = ParseDemDocument(write.bytes);
    Check(parse.ok, "roundtrip parse ok: " + parse.reason);
    if (!parse.ok) return;

    const DemHeader& got = parse.document.header;
    Check(got.match_id == header.match_id && got.mode_id == header.mode_id, "roundtrip header ids");
    Check(got.boss_id == header.boss_id && got.server_seed == header.server_seed, "roundtrip boss/seed");
    Check(got.tick_rate_hz == header.tick_rate_hz && got.final_tick == header.final_tick, "roundtrip tick fields");
    Check(got.final_state_hash == header.final_state_hash, "roundtrip final_state_hash");
    // Boss HP is part of the state hash: without it a recording made with
    // --boss-hp cannot be replayed to the same final state.
    Check(got.boss_max_hp == header.boss_max_hp, "roundtrip boss_max_hp");
    Check(got.entry_count == header.entry_count && got.entry_count == 5u, "roundtrip entry_count");
    Check(got.chain_head_digest == header.chain_head_digest, "roundtrip chain_head_digest");
    Check(got.players.size() == 1 && got.players[0].player_id == "p1" &&
              got.players[0].user_id == "user-1" && got.players[0].loadout.character_id == "reimu" &&
              got.players[0].loadout.shot_type == "spread" &&
              got.players[0].loadout.bomb_type == "fantasy_seal",
          "roundtrip players/loadout");
    Check(got.created_at == header.created_at, "roundtrip created_at");

    Check(parse.document.entries.size() == payloads.size(), "roundtrip entry count equals payload count");
    for (std::size_t index = 0; index < parse.document.entries.size() && index < payloads.size(); ++index) {
        Check(parse.document.entries[index].seq == index + 1, "roundtrip entry seq monotonic");
        Check(parse.document.entries[index].payload == payloads[index], "roundtrip entry payload identical");
    }
    Check(parse.document.entries[0].action_type == "match_seed", "roundtrip genesis action_type");
    Check(parse.document.entries[4].action_type == "cast_card", "roundtrip mode action_type");
}

// ---- ② 篡改检测 ----

void TestTamperDetection() {
    std::vector<std::string> payloads;
    DemRecorder recorder = SampleRecorder(&payloads);
    const auto write = recorder.Finalize(SampleHeader(recorder));
    Check(write.ok, "tamper baseline ok");
    if (!write.ok) return;

    {
        std::string bad = write.bytes;
        bad[0] = 'X';
        const auto parse = ParseDemDocument(bad);
        Check(!parse.ok && parse.reason == "dem_bad_magic", "tamper magic detected: " + parse.reason);
    }
    {
        std::string bad = write.bytes;
        bad.resize(bad.size() - 1);  // 尾字节丢失 → body_len+32 != size
        const auto parse = ParseDemDocument(bad);
        Check(!parse.ok && parse.reason == "dem_length_mismatch", "tamper length detected: " + parse.reason);
    }
    {
        std::string bad = write.bytes;
        bad[bad.size() - 1] = static_cast<char>(bad[bad.size() - 1] ^ 0x01);
        const auto parse = ParseDemDocument(bad);
        Check(!parse.ok && parse.reason == "dem_sha256_mismatch", "tamper sha256 detected: " + parse.reason);
    }
    {
        // zlib 坏流：合法 header + 非法压缩体 + 正确 sha256。
        const std::string header_json = EncodeDemHeaderJson(BaseHeader());
        const std::string garbage = "not-a-zlib-stream";
        const std::string container = BuildContainer(header_json, garbage);
        const auto parse = ParseDemDocument(container);
        Check(!parse.ok && parse.reason == "dem_zlib_failed", "tamper zlib detected: " + parse.reason);
    }
    {
        // header_len 声明过大 → dem_header_too_large（容器总长须 ≥ 固定尾）。
        std::string container;
        container.append(kDemMagic, 8);
        AppendU32(container, 0xFFFFFFFFu);
        container.append(40, '\0');
        const auto parse = ParseDemDocument(container);
        Check(!parse.ok && parse.reason == "dem_header_too_large", "oversize header detected: " + parse.reason);
    }
    {
        // 链断：body 里 seq 1 → 3（跳号），重新压缩 + 正确 sha256。
        std::vector<DemBodyEntry> entries(2);
        entries[0].seq = 1;
        entries[0].tick = 1;
        entries[0].player_id = "p1";
        entries[0].action_type = "input";
        entries[0].payload = "a";
        entries[1].seq = 3;
        entries[1].tick = 2;
        entries[1].player_id = "p1";
        entries[1].action_type = "input";
        entries[1].payload = "b";
        const std::string compressed = DemZlibCompress(EncodeDemBody(entries));
        DemHeader header = BaseHeader();
        header.entry_count = 2;
        header.chain_head_digest = std::string(kChainGenesisPrevDigest);
        const std::string container = BuildContainer(EncodeDemHeaderJson(header), compressed);
        const auto parse = ParseDemDocument(container);
        Check(!parse.ok && parse.reason.rfind("dem_chain_verify_failed", 0) == 0,
              "broken chain detected: " + parse.reason);
    }
}

// ---- ③ header JSON 往返 ----

void TestHeaderJsonRoundtrip() {
    const DemHeader header = BaseHeader();
    const std::string json = EncodeDemHeaderJson(header);
    Check(json.rfind("{\"match_id\":", 0) == 0, "header json field order match_id first");
    Check(json.find("\"chain_head_digest\":") != std::string::npos &&
              json.rfind(",") < json.find("\"chain_head_digest\":"),
          "header json chain_head_digest last");
    DemHeader decoded;
    Check(DecodeDemHeaderJson(json, &decoded), "header json decodes");
    Check(decoded.match_id == header.match_id && decoded.boss_id == header.boss_id &&
              decoded.tick_rate_hz == header.tick_rate_hz && decoded.final_tick == header.final_tick,
          "header json decoded fields");
    Check(decoded.players.size() == 1 && decoded.players[0].loadout.character_id == "reimu",
          "header json decoded players");
    DemHeader bad;
    Check(!DecodeDemHeaderJson("{not json", &bad), "header json rejects garbage");
    Check(!DecodeDemHeaderJson("{\"match_id\":\"x\"}", &bad), "header json rejects missing fields");
}

// boss_max_hp 是**可选**键：没有它（老容器、或没用 --boss-hp 的对局）容器字节
// 必须与加这个字段之前完全一致，解析回来是 0（重放端沿用自己的默认值）。
void TestBossMaxHpOptional() {
    const DemHeader header = BaseHeader();
    const std::string withHp = EncodeDemHeaderJson(header);
    Check(withHp.find("\"boss_max_hp\":300") != std::string::npos, "recorded boss_max_hp is written");

    DemHeader noHp = BaseHeader();
    noHp.boss_max_hp = 0;
    const std::string without = EncodeDemHeaderJson(noHp);
    Check(without.find("boss_max_hp") == std::string::npos, "boss_max_hp omitted when 0");

    DemHeader decoded;
    Check(DecodeDemHeaderJson(without, &decoded), "container without boss_max_hp decodes");
    Check(decoded.boss_max_hp == 0, "missing boss_max_hp decodes to 0");

    DemHeader decodedWith;
    Check(DecodeDemHeaderJson(withHp, &decodedWith), "container with boss_max_hp decodes");
    Check(decodedWith.boss_max_hp == 300, "recorded boss_max_hp survives the round trip");
}

// ---- ④ 空链 ----

void TestEmptyChain() {
    DemRecorder recorder;
    DemHeader header = BaseHeader();
    header.entry_count = 0;
    header.chain_head_digest = std::string(kChainGenesisPrevDigest);
    const auto write = recorder.Finalize(header);
    Check(write.ok, "empty chain finalize ok: " + write.reason);
    if (!write.ok) return;
    const auto parse = ParseDemDocument(write.bytes);
    Check(parse.ok && parse.document.entries.empty(), "empty chain parses to no entries: " + parse.reason);
    Check(EncodeDemBody({}).empty(), "empty body encodes to zero bytes");
    std::vector<DemBodyEntry> decoded;
    Check(DecodeDemBody("", &decoded) && decoded.empty(), "empty body decodes to no entries");
}

// ---- ⑤ 超长字段 ----

void TestOversizeField() {
    std::vector<std::string> payloads;
    DemRecorder recorder = SampleRecorder(&payloads);
    std::vector<ChainEntry> entries = recorder.Chain().Entries();
    DemHeader header = SampleHeader(recorder);
    entries[0].player_id = std::string(kDemFieldMaxBytes + 10, 'x');
    header.chain_head_digest = entries.back().entry_digest;
    const auto write = WriteDemDocument(header, entries, payloads);
    Check(!write.ok && write.reason == "dem_payload_too_long", "oversize field rejected: " + write.reason);
}

// ---- ⑦ WriteDemDocument 一致性护栏 ----

void TestWriteGuards() {
    std::vector<std::string> payloads;
    DemRecorder recorder = SampleRecorder(&payloads);
    const DemHeader header = SampleHeader(recorder);

    {
        DemHeader bad = header;
        bad.entry_count = header.entry_count + 1;
        const auto write = WriteDemDocument(bad, recorder.Chain().Entries(), payloads);
        Check(!write.ok && write.reason == "dem_header_entry_count_mismatch", "guard entry_count: " + write.reason);
    }
    {
        DemHeader bad = header;
        bad.chain_head_digest = "00000000";
        const auto write = WriteDemDocument(bad, recorder.Chain().Entries(), payloads);
        Check(!write.ok && write.reason == "dem_header_head_digest_mismatch", "guard head_digest: " + write.reason);
    }
    {
        std::vector<std::string> short_payloads(payloads.begin(), payloads.end() - 1);
        const auto write = WriteDemDocument(header, recorder.Chain().Entries(), short_payloads);
        Check(!write.ok && write.reason == "dem_payload_count_mismatch", "guard payload count: " + write.reason);
    }
    {
        std::vector<std::string> tampered = payloads;
        tampered[0] += "!";
        const auto write = WriteDemDocument(header, recorder.Chain().Entries(), tampered);
        Check(!write.ok && write.reason == "dem_payload_digest_mismatch", "guard payload digest: " + write.reason);
    }
}

// ---- ⑥ DemRecorder 链序 = 接受序 ----

void TestRecorderOrder() {
    std::vector<std::string> payloads;
    DemRecorder recorder = SampleRecorder(&payloads);
    Check(recorder.Size() == 5, "recorder size");
    Check(recorder.Chain().Entries().size() == recorder.Payloads().size(), "chain/payload alignment");
    const auto& entries = recorder.Chain().Entries();
    for (std::size_t index = 0; index < entries.size(); ++index) {
        Check(entries[index].seq == index + 1, "acceptance order == seq order");
    }
    Check(entries[0].action_type == "match_seed" && entries[0].player_id.empty(), "genesis scope entry first");
    Check(entries[1].player_id == "p1" && entries[3].player_id == "p2", "player attribution preserved");
    Check(recorder.Payloads()[0] == payloads[0], "recorder payload matches canonical seed");
    Check(recorder.Chain().HeadDigest() == entries.back().entry_digest, "head digest is last entry digest");

    // 幂等：重复 RecordMatchSeed 不新增。
    recorder.RecordMatchSeed(999);
    Check(recorder.Size() == 5, "match seed idempotent");

    // NoteUserId：首个非空胜出。
    DemRecorder note;
    note.NoteUserId("p1", "user-1");
    note.NoteUserId("p1", "user-2");
    Check(note.UserIds().at("p1") == "user-1", "note user id first non-empty wins");
}

// ---- ⑧ 编码原语 / 本地路径 ----

void TestPrimitives() {
    // sha256("abc") 金标准。
    Check(DemSha256Hex("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256 abc vector");
    // md5("abc") 金标准（RFC 1321）。
    Check(DemMd5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72", "md5 abc vector");
    Check(DemBase64Encode("hello") == "aGVsbG8=", "base64 hello vector");
    Check(DemBase64Encode("") == "", "base64 empty vector");

    // 本地路径穿越防护：分隔符被替换，最终文件名不含 '/' 或 '\\'（无法跳出 dem 目录）。
    const std::string path = DemLocalPath("../../etc/passwd");
    const std::size_t slash = path.find_last_of("/\\");
    const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    Check(base.find('/') == std::string::npos && base.find('\\') == std::string::npos,
          "dem local path sanitizes separators: " + path);
    Check(DemLocalPath("").find("unknown_match") != std::string::npos, "dem local path empty fallback");
}

}  // namespace

int main() {
    TestRoundtrip();
    TestTamperDetection();
    TestHeaderJsonRoundtrip();
    TestBossMaxHpOptional();
    TestEmptyChain();
    TestOversizeField();
    TestWriteGuards();
    TestRecorderOrder();
    TestPrimitives();

    if (g_failures != 0) {
        std::cerr << g_failures << " dem test(s) failed\n";
        return 1;
    }
    std::cout << "dem tests passed\n";
    return 0;
}
