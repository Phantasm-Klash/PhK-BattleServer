// 链式操作 + 种子日志测试（T5）。镜像设计文档 §8 测试矩阵：
//   ① 有效链验证 + 确定性（同参数两次构建 digest 逐一相等）
//   ② 翻转任一 payload 一字节 → 该条目及之后全断，first_bad_seq 定位
//   ③ 删除中间条目 → seq 断档
//   ④ seq 跳号 / 改号掩盖断档 → 失败
//   ⑤ genesis prev_digest = 文档化常量 "00000000"
//   ⑥ canonical payload 逐字节金标准（含 Python hashlib 跨语言金锚）
//   ⑦ 尾删边界（链自身不可检，HeadDigest 外部锚定）——回归保护
//   ⑧ 集成：BattleSimulation 接受序入链，被拒输入不入链
//
// 版权：东方同人逆向成果，仅用于自研玩法。

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "phk/battle/chain_log.hpp"
#include "phk/battle/lobby_rng.hpp"
#include "phk/battle/simulation.hpp"

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
using phk::battle::BattleSimulation;
using phk::battle::ChainEntry;
using phk::battle::ChainEntryDigestMaterial;
using phk::battle::ChainPayloadDigest;
using phk::battle::CanonicalChainInputPayload;
using phk::battle::CanonicalChainModeActionPayload;
using phk::battle::CanonicalChainSeedPayload;
using phk::battle::ComputeChainEntryDigest;
using phk::battle::kChainGenesisPrevDigest;
using phk::battle::kChainSeedKindBattleDerived;
using phk::battle::OperationChainLog;
using phk::battle::ShortHashHex;
using phk::battle::SimulationConfig;
using phk::battle::VerifyChain;

// 样本链：seed genesis + 2 input + 1 mode action。
OperationChainLog SampleChain() {
    OperationChainLog log;
    log.AppendMatchSeed(0, std::string(kChainSeedKindBattleDerived), 12345);
    log.AppendInput(1, "p1", 1, 0x01u, true, true, false, -1, "");
    log.AppendInput(2, "p2", 1, 0x02u, false, true, false, 2, "mode-action-1");
    log.AppendModeAction(3, "p1", "action-cast-1", 7, "cast_card", "{\"card_slot\":1}");
    return log;
}

bool IsShortHashHex(const std::string& value) {
    if (value.size() != 8u) {
        return false;
    }
    for (const char ch : value) {
        const bool digit = ch >= '0' && ch <= '9';
        const bool lower = ch >= 'a' && ch <= 'f';
        if (!digit && !lower) {
            return false;
        }
    }
    return true;
}

// 保证把字符改成另一个十六进制字符。
char FlipHexChar(char ch) {
    return ch == '0' ? '1' : '0';
}

// ---- ① 有效链 + 确定性 ----

void TestValidChainVerifiesAndIsDeterministic() {
    const OperationChainLog log = SampleChain();
    Check(log.Size() == 4u, "sample chain has 4 entries");
    Check(log.Entries()[0].seq == 1 && log.Entries()[3].seq == 4, "seq runs 1..4");

    const auto result = VerifyChain(log.Entries());
    Check(result.ok && result.reason == "ok", "valid chain verifies");
    Check(log.Verify().ok, "member Verify() agrees");

    for (const ChainEntry& entry : log.Entries()) {
        Check(IsShortHashHex(entry.payload_digest), "payload_digest is 8 lowercase hex");
        Check(IsShortHashHex(entry.entry_digest), "entry_digest is 8 lowercase hex");
        Check(entry.entry_digest == ComputeChainEntryDigest(entry), "digest recomputes");
        Check(entry.entry_digest == ShortHashHex(ChainEntryDigestMaterial(entry)),
              "entry_digest == ShortHashHex(material)");
    }

    const OperationChainLog again = SampleChain();
    bool identical = log.Entries().size() == again.Entries().size();
    for (std::size_t i = 0; identical && i < log.Entries().size(); ++i) {
        identical = log.Entries()[i].entry_digest == again.Entries()[i].entry_digest;
    }
    Check(identical, "same inputs -> byte-identical chain (determinism)");
}

// ---- ② 翻转一字节：该条目及之后全断 ----

void TestPayloadByteFlipBreaksChainFromThatEntry() {
    OperationChainLog log = SampleChain();

    std::vector<ChainEntry> entries = log.Entries();
    entries[1].payload_digest[0] = FlipHexChar(entries[1].payload_digest[0]);
    const auto mid = VerifyChain(entries);
    Check(!mid.ok, "flipped payload breaks verification");
    Check(mid.first_bad_seq == 2, "first_bad_seq == flipped entry seq");
    Check(mid.reason == "chain_entry_digest_mismatch", "flip reason is digest mismatch");

    entries = log.Entries();
    entries[3].payload_digest[7] = FlipHexChar(entries[3].payload_digest[7]);
    const auto tail = VerifyChain(entries);
    Check(!tail.ok && tail.first_bad_seq == 4, "last entry flip breaks at seq 4");

    // 直接改 entry_digest 同样在重算处失败。
    entries = log.Entries();
    entries[2].entry_digest[0] = FlipHexChar(entries[2].entry_digest[0]);
    const auto forged = VerifyChain(entries);
    Check(!forged.ok && forged.first_bad_seq == 3, "forged entry_digest breaks at seq 3");

    // 改 prev_digest（伪装链接）在重算处失败。
    entries = log.Entries();
    entries[2].prev_digest = std::string("deadbeef");
    const auto relink = VerifyChain(entries);
    Check(!relink.ok && relink.first_bad_seq == 3, "forged prev_digest breaks at seq 3");
}

// ---- ③ 删除中间条目 ----

void TestDeletedMiddleEntryBreaksChain() {
    OperationChainLog log = SampleChain();
    std::vector<ChainEntry> entries = log.Entries();
    entries.erase(entries.begin() + 1);  // 删 seq=2

    const auto result = VerifyChain(entries);
    Check(!result.ok, "deleted middle entry breaks verification");
    Check(result.first_bad_seq == 3, "first_bad_seq == entry after the hole");
    Check(result.reason == "chain_seq_gap", "deletion surfaces as seq gap");

    // 改号掩盖断档 → prev 链接断裂（entry 3 的 prev_digest 仍指向被删条目）。
    entries[1].seq = 2;
    const auto renumbered = VerifyChain(entries);
    Check(!renumbered.ok, "renumbering to hide the gap still fails");
    Check(renumbered.reason == "chain_prev_link_broken", "renumbered gap surfaces as broken link");
}

// ---- ④ seq 断档 / 首条非 1 / genesis 常量被改 ----

void TestSeqGapAndGenesisConstantBreakChain() {
    std::vector<ChainEntry> entries = SampleChain().Entries();
    entries[3].seq = 99;  // 跳号
    const auto gap = VerifyChain(entries);
    Check(!gap.ok && gap.first_bad_seq == 99 && gap.reason == "chain_seq_gap", "seq jump breaks");

    entries = SampleChain().Entries();
    entries[0].seq = 5;
    const auto not_one = VerifyChain(entries);
    Check(!not_one.ok && not_one.first_bad_seq == 5 && not_one.reason == "chain_first_seq_not_one",
          "first seq must be 1");

    entries = SampleChain().Entries();
    entries[0].prev_digest = "deadbeef";
    const auto genesis = VerifyChain(entries);
    Check(!genesis.ok && genesis.first_bad_seq == 1 &&
              genesis.reason == "chain_genesis_prev_digest_mismatch",
          "genesis prev_digest must be the documented constant");
}

// ---- ⑤ genesis 常量 ----

void TestGenesisPrevDigestConstant() {
    Check(std::string(kChainGenesisPrevDigest) == "00000000", "genesis prev_digest is 00000000");
    Check(std::string(kChainGenesisPrevDigest).size() == 8u, "genesis constant is 8 chars");
    for (const char ch : std::string(kChainGenesisPrevDigest)) {
        Check(ch == '0', "genesis constant is all zeros");
    }
    Check(SampleChain().Entries()[0].prev_digest == kChainGenesisPrevDigest,
          "first appended entry carries the constant");
}

// ---- ⑥ canonical payload 金标准 ----

void TestCanonicalPayloadGoldens() {
    Check(CanonicalChainInputPayload(7, 5, true, false, true, -1, "mode-action-9") ==
              "seq=7|dir=5|slow=1|shoot=0|bomb=1|card=-1|mode_action_id=mode-action-9",
          "input payload canonical bytes");
    Check(CanonicalChainModeActionPayload("action-cast-1", 7, "{\"card_slot\":1}") ==
              "action_id=action-cast-1|seq=7|payload={\"card_slot\":1}",
          "mode action payload canonical bytes");
    Check(CanonicalChainSeedPayload("battle_derived", 12345) == "seed_kind=battle_derived|seed=12345",
          "seed payload canonical bytes");

    // 跨语言金锚（Python hashlib.sha256(...).hexdigest()[:8] 预计算）：
    Check(ShortHashHex("2|5|p1|input|1a2b3c4d|00000000") == "0c669de5", "digest material golden");
    Check(ChainPayloadDigest("seed_kind=battle_derived|seed=12345") == "65fd53b6", "seed payload golden");
    Check(ChainPayloadDigest(
              "seq=7|dir=5|slow=1|shoot=0|bomb=1|card=-1|mode_action_id=mode-action-9") == "6e046fee",
          "input payload golden");
    Check(ChainPayloadDigest("action_id=action-cast-1|seq=7|payload={\"card_slot\":1}") == "333e9712",
          "mode action payload golden");

    // 同字段同 digest、异字段异 digest。
    Check(ChainPayloadDigest(CanonicalChainInputPayload(1, 0, true, true, false, -1, "")) ==
              ChainPayloadDigest(CanonicalChainInputPayload(1, 0, true, true, false, -1, "")),
          "same fields -> same digest");
    Check(ChainPayloadDigest(CanonicalChainInputPayload(1, 0, true, true, false, -1, "")) !=
              ChainPayloadDigest(CanonicalChainInputPayload(1, 0, true, true, false, 2, "")),
          "card_slot change -> different digest");
}

// ---- ⑦ 尾删边界（文档化行为，回归保护）----

void TestTailDeletionCaveat() {
    OperationChainLog log = SampleChain();
    const std::string head = log.HeadDigest();
    Check(head == log.Entries()[3].entry_digest, "HeadDigest is the last entry digest");
    Check(head != std::string(kChainGenesisPrevDigest), "HeadDigest differs from genesis constant");

    std::vector<ChainEntry> truncated = log.Entries();
    truncated.pop_back();
    Check(VerifyChain(truncated).ok,
          "tail deletion alone is NOT detectable by the chain (documented caveat §4; "
          "external HeadDigest anchor required)");
    Check(VerifyChain(truncated).reason == "ok", "truncated chain still verifies ok");

    OperationChainLog fresh;
    Check(fresh.Empty() && fresh.HeadDigest() == kChainGenesisPrevDigest && fresh.NextSeq() == 1u,
          "empty log: genesis head, next seq 1");
    Check(VerifyChain(fresh.Entries()).ok && VerifyChain(fresh.Entries()).reason == "chain_empty",
          "empty chain verifies as chain_empty");
}

// ---- ⑧ 集成：BattleSimulation 接受序入链 ----

BattleInput MakeChainTestInput(const std::string& player_id, std::uint64_t tick, std::uint64_t seq,
                               int card_slot) {
    BattleInput input;
    input.match_id = "match-001";
    input.player_id = player_id;
    input.tick = tick;
    input.seq = seq;
    input.direction_bits = 0x01u;
    input.slow = true;
    input.shoot = true;
    input.card_slot = card_slot;
    return input;
}

void TestSimulationIntegration() {
    SimulationConfig config;
    config.match_id = "match-001";
    config.mode_id = "world_boss";
    config.match_seed = 12345;
    config.spawn_period_ticks = 2;
    BattleSimulation sim(config);
    Check(sim.AddPlayer("p1", -20000, 0), "add p1");
    Check(sim.AddPlayer("p2", 20000, 0), "add p2");
    Check(sim.AddPlayer("p3", 0, 60000), "add p3");
    Check(sim.AddPlayer("p4", -60000, 0), "add p4");

    Check(sim.AcceptInput(MakeChainTestInput("p1", 1, 1, -1)).ok, "input 1 accepted");
    Check(sim.AcceptInput(MakeChainTestInput("p1", 2, 2, -1)).ok, "input 2 accepted");
    Check(!sim.AcceptInput(MakeChainTestInput("p1", 3, 3, 8)).ok, "card_slot 8 rejected at wire level");

    BattleModeAction action;
    action.match_id = "match-001";
    action.player_id = "p1";
    action.tick = 2;
    action.seq = 4;
    action.action_id = "action-cast-1";
    action.action_type = "cast_card";
    action.payload_json = "{\"card_slot\":1}";
    Check(sim.AcceptModeAction(action).ok, "cast_card mode action accepted");

    const OperationChainLog& chain = sim.OperationLog();
    Check(chain.Size() == 4u, "chain = 1 seed + 2 inputs + 1 action (rejected input excluded)");

    const std::vector<ChainEntry>& entries = chain.Entries();
    Check(entries[0].action_type == "match_seed" && entries[0].player_id.empty() && entries[0].tick == 0,
          "genesis entry is the match seed (match scope, tick 0)");
    Check(entries[0].payload_digest == ChainPayloadDigest("seed_kind=battle_derived|seed=12345"),
          "seed entry records DeriveMatchSeed value + kind (H-2: not the lobby seed)");
    Check(entries[1].action_type == "input" && entries[1].player_id == "p1" && entries[1].tick == 1,
          "input entry 1");
    Check(entries[2].action_type == "input" && entries[2].player_id == "p1" && entries[2].tick == 2,
          "input entry 2");
    Check(entries[3].action_type == "cast_card" && entries[3].player_id == "p1" && entries[3].tick == 2,
          "mode action entry keeps its real type");

    const auto verified = VerifyChain(entries);
    Check(verified.ok, "integration chain verifies");
    Check(!sim.OperationLog().Verify().ok ? false : true, "member verify also ok");
    Check(sim.OperationLog().HeadDigest() == entries[3].entry_digest, "head digest matches last entry");
}

}  // namespace

int main() {
    TestValidChainVerifiesAndIsDeterministic();
    TestPayloadByteFlipBreaksChainFromThatEntry();
    TestDeletedMiddleEntryBreaksChain();
    TestSeqGapAndGenesisConstantBreakChain();
    TestGenesisPrevDigestConstant();
    TestCanonicalPayloadGoldens();
    TestTailDeletionCaveat();
    TestSimulationIntegration();

    if (g_failures != 0) {
        std::cerr << g_failures << " chain_log test(s) failed\n";
        return 1;
    }
    std::cout << "chain_log tests passed\n";
    return 0;
}
