// 对战端链式操作 + 随机种子日志（T5）。
//
// 设计文档（逐字节规范化与语义的唯一真值）：
//   /root/gotouhou/task-queue/out/T5_chain_log.md
//     §2 条目结构 / §3 规范化序列化 / §4 VerifyChain 语义 /
//     §5 链序=接受序 / §6 集成点 / §7 与既有 replay 的关系
//
// 摘要算法复用 src/lobby_rng.cpp 的 ShortHashHex（真实 SHA-256 前 4 字节的
// 8 位小写十六进制，真值来源大厅 simulation.go:349 shortHash），不新造哈希。
//
// 已知边界（文档 §4）：删除**尾部**条目仅靠链本身不可检测（无外部锚定），
// 须配合 HeadDigest() 单独留痕后比对；删中间条目 / 改字节 / seq 断档可定位。
//
// 版权：东方同人逆向成果，仅用于自研玩法。
#ifndef PHK_BATTLE_CHAIN_LOG_HPP
#define PHK_BATTLE_CHAIN_LOG_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace phk::battle {

// genesis 条目（seq==1）的 prev_digest 常量：8 个 '0'，与 shortHashHex 等宽。
inline constexpr const char* kChainGenesisPrevDigest = "00000000";

// 种子条目 seed_kind 词表（T7 H-2）：今日对战侧值为 battle_derived
// （DeriveMatchSeed，FNV-1a-64，server.cpp:1585）；大厅权威种子
// （seedFrom，SHA-256，service.go:6376）经 loadout server_seed 下发后
// 改为 lobby_authoritative——链结构与调用点不变。
inline constexpr const char* kChainSeedKindBattleDerived = "battle_derived";
inline constexpr const char* kChainSeedKindLobbyAuthoritative = "lobby_authoritative";

// 对局级条目（match_seed）的 player_id：空串。
inline constexpr const char* kChainMatchScopePlayerId = "";

// action_type 词表。mode action 条目用其真实 action_type
// （cast_card / ready / transfer_card / reconnect_*），不过本常量。
inline constexpr const char* kChainActionTypeInput = "input";
inline constexpr const char* kChainActionTypeMatchSeed = "match_seed";

// 链式条目。append-only：字段一旦写入不再变更（变更即验证失败）。
struct ChainEntry {
    std::uint64_t seq = 0;        // 链上序号：从 1 起，每条严格 +1（组件强制）
    std::int64_t tick = 0;        // 操作被接受时的 tick；种子条目 = 0
    std::string player_id;        // 操作者；对局级条目为空串
    std::string action_type;      // "match_seed" / "input" / mode action 原类型
    std::string payload_digest;   // ShortHashHex(规范化 payload)
    std::string prev_digest;      // 上一条 entry_digest；genesis = kChainGenesisPrevDigest
    std::string entry_digest;     // ShortHashHex(digest material)
};

// ---- 规范化 payload（确定性序列化；字段顺序即字节顺序，文档 §3.2）----

// input 条目：字段集对齐既有 input_trace_（simulation.cpp:2305）并补上
// trace 文本缺失的 mode_action_id（它本就在 input_stream_hash_ 里）。
[[nodiscard]] std::string CanonicalChainInputPayload(
    std::uint64_t seq,
    std::uint32_t direction_bits,
    bool slow,
    bool shoot,
    bool bomb,
    int card_slot,
    const std::string& mode_action_id
);

// mode action 条目：payload_json 按客户端原始字节入 digest（口径同既有
// event_stream_hash_，simulation.cpp:2359）。
[[nodiscard]] std::string CanonicalChainModeActionPayload(
    const std::string& action_id,
    std::uint64_t seq,
    const std::string& payload_json
);

// 种子条目：seed_kind 记录来源（battle_derived / lobby_authoritative）。
[[nodiscard]] std::string CanonicalChainSeedPayload(
    const std::string& seed_kind,
    std::uint64_t seed
);

// ---- 摘要 ----

// digest material（文档 §3.1）：
//   <seq>|<tick>|<player_id>|<action_type>|<payload_digest>|<prev_digest>
[[nodiscard]] std::string ChainEntryDigestMaterial(const ChainEntry& entry);
[[nodiscard]] std::string ComputeChainEntryDigest(const ChainEntry& entry);
[[nodiscard]] std::string ChainPayloadDigest(const std::string& canonical_payload);

// ---- 验证 ----

struct ChainVerificationResult {
    bool ok = true;
    std::uint64_t first_bad_seq = 0;  // 首个失败条目自带的 seq 字段值
    std::string reason;               // "ok" / "chain_empty" / 失败码（见下）
};

// 失败码：chain_first_seq_not_one / chain_genesis_prev_digest_mismatch /
//         chain_seq_gap / chain_prev_link_broken / chain_entry_digest_mismatch
// 空链：ok=true，first_bad_seq=0，reason="chain_empty"。
[[nodiscard]] ChainVerificationResult VerifyChain(const std::vector<ChainEntry>& entries);

// ---- 日志本体（append-only）----

class OperationChainLog {
public:
    // 种子 genesis 条目：player_id=kChainMatchScopePlayerId，
    // action_type=kChainActionTypeMatchSeed。约定每场对局的第一条。
    void AppendMatchSeed(std::int64_t tick, const std::string& seed_kind, std::uint64_t seed);

    // 接受的 BattleInput（AcceptInput 验证通过后调用）。
    void AppendInput(
        std::int64_t tick,
        const std::string& player_id,
        std::uint64_t seq,
        std::uint32_t direction_bits,
        bool slow,
        bool shoot,
        bool bomb,
        int card_slot,
        const std::string& mode_action_id
    );

    // 接受的 BattleModeAction（AcceptModeAction 验证通过后调用）；
    // action_type 用真实类型（cast_card/ready/transfer_card/reconnect_*）。
    void AppendModeAction(
        std::int64_t tick,
        const std::string& player_id,
        const std::string& action_id,
        std::uint64_t seq,
        const std::string& action_type,
        const std::string& payload_json
    );

    // 通用追加（未来事件类型 / 测试）：payload_digest 须为 ShortHashHex 输出形态。
    void AppendEntry(
        std::int64_t tick,
        const std::string& player_id,
        const std::string& action_type,
        const std::string& payload_digest
    );

    [[nodiscard]] const std::vector<ChainEntry>& Entries() const;
    [[nodiscard]] std::size_t Size() const;
    [[nodiscard]] bool Empty() const;
    [[nodiscard]] std::uint64_t NextSeq() const;
    // 末条 entry_digest；空链返回 genesis 常量。审计方应将其单独留痕
    // （尾删只有靠这个外部锚点能发现，文档 §4）。
    [[nodiscard]] std::string HeadDigest() const;
    [[nodiscard]] ChainVerificationResult Verify() const;

private:
    std::vector<ChainEntry> entries_;
};

}  // namespace phk::battle

#endif  // PHK_BATTLE_CHAIN_LOG_HPP
