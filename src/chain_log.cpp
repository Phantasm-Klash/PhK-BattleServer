// 链式操作 + 种子日志实现（T5）。规范化与验证语义的唯一真值是设计文档：
// /root/gotouhou/task-queue/out/T5_chain_log.md（§3 序列化 / §4 验证）。
//
// 版权：东方同人逆向成果，仅用于自研玩法。

#include "phk/battle/chain_log.hpp"

#include "phk/battle/lobby_rng.hpp"

namespace phk::battle {
namespace {

// 布尔渲染与 simulation.cpp:46 BoolToken 一致（"1"/"0"）。
const char* ChainBoolToken(bool value) {
    return value ? "1" : "0";
}

}  // namespace

std::string CanonicalChainInputPayload(
    std::uint64_t seq,
    std::uint32_t direction_bits,
    bool slow,
    bool shoot,
    bool bomb,
    int card_slot,
    const std::string& mode_action_id
) {
    return "seq=" + std::to_string(seq) +
        "|dir=" + std::to_string(direction_bits) +
        "|slow=" + ChainBoolToken(slow) +
        "|shoot=" + ChainBoolToken(shoot) +
        "|bomb=" + ChainBoolToken(bomb) +
        "|card=" + std::to_string(card_slot) +
        "|mode_action_id=" + mode_action_id;
}

std::string CanonicalChainModeActionPayload(
    const std::string& action_id,
    std::uint64_t seq,
    const std::string& payload_json
) {
    // payload_json 必须是最后一段：其中可含任意字节（含 '|'），
    // 但 digest 只要求确定性，不要求可解析。
    return "action_id=" + action_id + "|seq=" + std::to_string(seq) + "|payload=" + payload_json;
}

std::string CanonicalChainSeedPayload(const std::string& seed_kind, std::uint64_t seed) {
    return "seed_kind=" + seed_kind + "|seed=" + std::to_string(seed);
}

std::string ChainEntryDigestMaterial(const ChainEntry& entry) {
    return std::to_string(entry.seq) +
        "|" + std::to_string(entry.tick) +
        "|" + entry.player_id +
        "|" + entry.action_type +
        "|" + entry.payload_digest +
        "|" + entry.prev_digest;
}

std::string ComputeChainEntryDigest(const ChainEntry& entry) {
    return ShortHashHex(ChainEntryDigestMaterial(entry));
}

std::string ChainPayloadDigest(const std::string& canonical_payload) {
    return ShortHashHex(canonical_payload);
}

ChainVerificationResult VerifyChain(const std::vector<ChainEntry>& entries) {
    ChainVerificationResult result;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const ChainEntry& entry = entries[i];
        if (i == 0) {
            if (entry.seq != 1) {
                result.ok = false;
                result.first_bad_seq = entry.seq;
                result.reason = "chain_first_seq_not_one";
                return result;
            }
            if (entry.prev_digest != kChainGenesisPrevDigest) {
                result.ok = false;
                result.first_bad_seq = entry.seq;
                result.reason = "chain_genesis_prev_digest_mismatch";
                return result;
            }
        } else {
            const ChainEntry& prev = entries[i - 1];
            if (entry.seq != prev.seq + 1) {
                result.ok = false;
                result.first_bad_seq = entry.seq;
                result.reason = "chain_seq_gap";
                return result;
            }
            if (entry.prev_digest != prev.entry_digest) {
                result.ok = false;
                result.first_bad_seq = entry.seq;
                result.reason = "chain_prev_link_broken";
                return result;
            }
        }
        if (entry.entry_digest != ComputeChainEntryDigest(entry)) {
            result.ok = false;
            result.first_bad_seq = entry.seq;
            result.reason = "chain_entry_digest_mismatch";
            return result;
        }
    }
    result.reason = entries.empty() ? "chain_empty" : "ok";
    return result;
}

void OperationChainLog::AppendMatchSeed(std::int64_t tick, const std::string& seed_kind, std::uint64_t seed) {
    AppendEntry(
        tick,
        std::string(kChainMatchScopePlayerId),
        std::string(kChainActionTypeMatchSeed),
        ChainPayloadDigest(CanonicalChainSeedPayload(seed_kind, seed))
    );
}

void OperationChainLog::AppendInput(
    std::int64_t tick,
    const std::string& player_id,
    std::uint64_t seq,
    std::uint32_t direction_bits,
    bool slow,
    bool shoot,
    bool bomb,
    int card_slot,
    const std::string& mode_action_id
) {
    AppendEntry(
        tick,
        player_id,
        std::string(kChainActionTypeInput),
        ChainPayloadDigest(CanonicalChainInputPayload(
            seq, direction_bits, slow, shoot, bomb, card_slot, mode_action_id
        ))
    );
}

void OperationChainLog::AppendModeAction(
    std::int64_t tick,
    const std::string& player_id,
    const std::string& action_id,
    std::uint64_t seq,
    const std::string& action_type,
    const std::string& payload_json
) {
    AppendEntry(
        tick,
        player_id,
        action_type,
        ChainPayloadDigest(CanonicalChainModeActionPayload(action_id, seq, payload_json))
    );
}

void OperationChainLog::AppendEntry(
    std::int64_t tick,
    const std::string& player_id,
    const std::string& action_type,
    const std::string& payload_digest
) {
    ChainEntry entry;
    entry.seq = entries_.empty() ? 1u : entries_.back().seq + 1u;
    entry.tick = tick;
    entry.player_id = player_id;
    entry.action_type = action_type;
    entry.payload_digest = payload_digest;
    entry.prev_digest = entries_.empty() ? std::string(kChainGenesisPrevDigest) : entries_.back().entry_digest;
    entry.entry_digest = ComputeChainEntryDigest(entry);
    entries_.push_back(std::move(entry));
}

const std::vector<ChainEntry>& OperationChainLog::Entries() const {
    return entries_;
}

std::size_t OperationChainLog::Size() const {
    return entries_.size();
}

bool OperationChainLog::Empty() const {
    return entries_.empty();
}

std::uint64_t OperationChainLog::NextSeq() const {
    return entries_.empty() ? 1u : entries_.back().seq + 1u;
}

std::string OperationChainLog::HeadDigest() const {
    return entries_.empty() ? std::string(kChainGenesisPrevDigest) : entries_.back().entry_digest;
}

ChainVerificationResult OperationChainLog::Verify() const {
    return VerifyChain(entries_);
}

}  // namespace phk::battle
