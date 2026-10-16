#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "phk/battle/card_hand.hpp"
#include "phk/battle/chain_log.hpp"
#include "phk/battle/protocol.hpp"
#include "phk/battle/result.hpp"

namespace phk::battle {

inline constexpr std::uint32_t kBattleTickRateHz = 60;
inline constexpr std::int32_t kArenaHalfWidthMilli = 120000;
inline constexpr std::int32_t kArenaHalfHeightMilli = 90000;
inline constexpr std::size_t kDefaultMaxModeActionIdBytes = 128;
inline constexpr std::size_t kDefaultMaxModeActionTypeBytes = 64;
inline constexpr std::size_t kDefaultMaxModeActionPayloadBytes = 4096;
inline constexpr std::size_t kDefaultMaxTransferCardInstanceIdBytes = 128;
inline constexpr std::size_t kDefaultMaxBossIdentityBytes = 128;
inline constexpr std::size_t kBossModeMinPlayers = 4;
inline constexpr std::size_t kBossModeMaxPlayers = 8;

// B3 命中判定（子弹↔玩家接触）：镜像 Go resolveBulletContactsLocked
// （Gensoulkyo/runtime/core/simulation.go:191-229）。竞技场单位为 milli
// （1 px = 1000 milli），故半径与 BulletState.radius_milli 同单位、可直接相加。
// 玩家判定半径 = Go playerRadius = 4.5（simulation.go:16）的数值，即 4500 milli。
inline constexpr std::int32_t kPlayerHitRadiusMilli = 4500;
// 擦弹半径 = Go grazeRadius = 28.0（simulation.go:17），即 28000 milli。
inline constexpr std::int32_t kPlayerGrazeRadiusMilli = 28000;
// 命中后的无敌帧时长 = Go simulation.go:209 的 `tick + 90`。
inline constexpr std::int64_t kPlayerHitInvulnerableTicks = 90;

enum class InputValidationCode {
    Ok,
    VersionIncompatible,
    MatchUnknown,
    MatchSettled,
    MatchMismatch,
    PlayerUnknown,
    SeqMissing,
    SeqReplay,
    DuplicateInputForTick,
    TickTooOld,
    TickTooFarAhead,
    InvalidDirectionBits,
    InvalidCardSlot,
    InvalidModeAction,
    PlayerDisconnected,
    SeqTooFarAhead,
    EventCursorAhead,
    DuplicateModeAction,
    ReadyAlreadySet,
    BossMinPlayersNotMet,
};

struct InputValidationResult {
    bool ok = false;
    InputValidationCode code = InputValidationCode::Ok;
    std::string reason = "ok";
};

struct ReplaySummary {
    std::string match_id;
    std::string mode_id;
    std::string ruleset_version;
    std::string input_stream_hash;
    std::string event_stream_hash;
    std::string final_state_hash;
    std::string last_mode_action_id;
    std::string last_mode_action_type;
    std::string last_mode_action_player_id;
    std::uint64_t match_seed = 0;
    std::uint64_t final_tick = 0;
    std::uint64_t input_count = 0;
    std::uint64_t fallback_input_count = 0;
    std::uint64_t neutral_fallback_count = 0;
    std::uint64_t held_input_fallback_count = 0;
    std::uint64_t mode_action_count = 0;
    std::uint64_t event_count = 0;
    std::uint64_t last_mode_action_tick = 0;
    std::uint64_t last_mode_action_seq = 0;
    std::uint64_t boss_max_hp = 0;
    std::uint64_t boss_current_hp = 0;
    std::uint64_t boss_damage_total = 0;
    std::uint64_t boss_defeated = 0;
    std::uint64_t boss_defeated_tick = 0;
    std::vector<std::string> player_ids;
    std::map<std::string, std::uint64_t> boss_damage_by_player;
    std::map<std::string, std::string> boss_spawn_slot_by_player;
    std::map<std::string, std::string> boss_fire_target_by_player;
    std::vector<std::string> input_trace;
    std::vector<std::string> event_trace;
};

struct ReplayInputStreamSummaryRecord {
    VersionStamp version;
    std::string replay_id;
    std::string owner_user_id;
    std::string match_id;
    std::uint64_t input_count = 0;
    std::uint64_t fallback_input_count = 0;
    std::uint64_t neutral_fallback_count = 0;
    std::uint64_t held_input_fallback_count = 0;
    std::uint64_t event_count = 0;
    std::string input_stream_hash;
    std::string event_stream_hash;
    std::string final_state_hash;
    std::uint64_t match_seed = 0;
    std::uint64_t final_tick = 0;
    std::uint64_t boss_max_hp = 0;
    std::uint64_t boss_current_hp = 0;
    std::uint64_t boss_damage_total = 0;
    std::uint64_t boss_defeated = 0;
    std::uint64_t boss_defeated_tick = 0;
};

struct ReplayFixture {
    std::string replay_id;
    std::string owner_user_id;
    std::string match_id;
    std::string mode_id;
    std::string ruleset_version;
    std::string result_hash;
    std::vector<std::string> player_ids;
    std::vector<std::string> input_trace;
    std::vector<std::string> event_trace;
    ReplaySummary summary;
    ReplayInputStreamSummaryRecord replay_summary_record;
    BattleSnapshot final_snapshot;
    std::uint32_t tick_rate_hz = kBattleTickRateHz;
    std::uint64_t match_seed = 0;
    std::uint64_t event_cursor = 0;
    bool server_authoritative = true;
};

struct ReplayLoadoutBridge {
    std::string user_id;
    std::string player_id;
    std::string character_id;
    std::string stage_id;
    std::string rating_code;
    std::string deck_snapshot_hash;
    std::string deck_ruleset_version;
    std::vector<std::string> deck_card_ids;
};

struct ReplayRecordBridge {
    VersionStamp version;
    std::string replay_id;
    std::string match_id;
    std::string owner_user_id;
    std::string mode_id;
    std::string stage_id;
    std::vector<ReplayLoadoutBridge> loadout;
    ReplayInputStreamSummaryRecord stream;
    SignedBattleResult settlement;
    bool server_authoritative = true;
    std::int64_t created_at_ms = 0;
};

struct SimulationConfig {
    std::string match_id;
    std::string mode_id;
    std::string ruleset_version = std::string(kDefaultRulesetVersion);
    std::uint64_t match_seed = 0;
    std::uint32_t tick_rate_hz = kBattleTickRateHz;
    std::uint32_t max_input_ahead_ticks = 8;
    std::uint32_t max_seq_ahead = 32;
    // 0 disables silence detection (tests/tools); production sets 3 seconds.
    std::uint32_t input_timeout_ticks = 0;
    std::uint32_t spawn_period_ticks = 30;
    std::uint32_t max_bullets = 256;
    std::size_t max_mode_action_id_bytes = kDefaultMaxModeActionIdBytes;
    std::size_t max_mode_action_type_bytes = kDefaultMaxModeActionTypeBytes;
    std::size_t max_mode_action_payload_bytes = kDefaultMaxModeActionPayloadBytes;
    std::string boss_instance_id;
    std::string boss_season_id;
    std::string boss_phase_id;
    std::uint64_t boss_max_hp = 1000;
    std::string boss_friendly_fire_policy = "disabled";
};

struct TransferableCardState {
    std::string card_instance_id;
    std::string owner_player_id;
    bool mode_allowed = true;
    bool cost_paid = true;
    bool cooldown_ready = true;
};

// 出牌执行器收集的一行事件（T3 step4）。字段与大厅 BattleEventRow
// （Gensoulkyo/runtime/core/battle_events.go:41-50）一一对应，Step 2 逐字段
// 映射成回传 JSON。event_value 语义 = 受影响的子弹计数（spawned/removed/
// cleared；card_accepted 时为卡伤害），不是费用。hit / graze 行为 B3 接触行：
// event_value 镜像 Go（hit = bullet.Damage、graze = 字面 1，Go 侧实际均 1），
// 且**不带 card_id**（大厅 battle_events.go:249-256 对两者早退）。
//
// 故意不进 event_trace_/event_stream_hash_/event_count_：后三者参与
// ReplaySummary 与 CanonicalStateHash，卡行进重放哈希会扰动既有确定性断言。
struct CardCastEvent {
    std::string event_type;   // card_accepted | card_effect_applied | card_rejected | boss_defeated | all_players_disconnected | hit | graze
    std::int64_t tick = 0;
    std::string player_id;
    std::string card_id;
    std::int64_t event_value = 0;
    std::string target_user_id;
    int slot = -1;
    std::string reason;
};

// T10 rule 2：对局级完成信号（battle_events.go:45 / isMatchLevelBattleEvent）。
// 当整支队伍都被动掉线、boss 进程全部结束时发出，**不携带 user_id / card_id**
// （大厅对该类型拒绝两者，battle_events.go:231-236）。大厅收到后裁定为"双方
// 逃跑"（no-contest，不计分），对战服只提供信号、不自作主张。
inline constexpr const char* kAllPlayersDisconnectedEventType = "all_players_disconnected";

// 事件行收集器容量上限（有界防 OOM；超限丢最旧）。lobby 每次提交上限 64 行
// （battle_events.go:23 maxBattleEventsPerSubmit），宿主应按 tick 周期调用
// TakeCardCastEvents 排空；本上限只防宿主完全不上报时的无界增长。
inline constexpr std::size_t kCardCastEventRowLimit = 1024;

class BattleSimulation {
public:
    explicit BattleSimulation(SimulationConfig config);

    [[nodiscard]] const SimulationConfig& Config() const;
    [[nodiscard]] std::uint64_t CurrentTick() const;
    [[nodiscard]] std::size_t PlayerCount() const;
    [[nodiscard]] std::size_t BulletCount() const;
    [[nodiscard]] std::uint64_t AcceptedInputCount() const;
    // T5 链式操作日志（append-only；只读访问，验证用 VerifyChain）。
    [[nodiscard]] const OperationChainLog& OperationLog() const;
    [[nodiscard]] bool IsPlayerConnected(const std::string& player_id) const;
    [[nodiscard]] bool BossRosterReadyToLock() const;

    bool AddPlayer(const std::string& player_id, std::int32_t x_milli, std::int32_t y_milli);
    bool ConfigureTransferableCard(TransferableCardState card);
    [[nodiscard]] InputValidationResult SetPlayerConnected(const std::string& player_id, bool connected);
    [[nodiscard]] InputValidationResult ValidateInput(const BattleInput& input) const;
    InputValidationResult AcceptInput(const BattleInput& input);
    [[nodiscard]] InputValidationResult ValidateModeAction(const BattleModeAction& action) const;
    InputValidationResult AcceptModeAction(const BattleModeAction& action);
    BattleSnapshot Tick();
    [[nodiscard]] BattleSnapshot Snapshot(std::string snapshot_kind = "full") const;
    [[nodiscard]] BattleSnapshot ReconnectSnapshot(
        const std::string& player_id,
        std::uint64_t last_seen_event_cursor
    ) const;
    [[nodiscard]] ReplaySummary Summary() const;
    [[nodiscard]] ReplayInputStreamSummaryRecord BuildReplayInputStreamSummary(
        std::string owner_user_id = ""
    ) const;
    [[nodiscard]] ReplayFixture BuildReplayFixture(std::string owner_user_id = "") const;

    // ---- T3 step4：出牌执行器（手牌播种 / 执行 / 事件收集） ----
    // 大厅权威种子（票据 server_seed 十进制文本解析值）；仅 bullet_purge 选取
    // 使用（T7 H-2），DeriveMatchSeed（config_.match_seed）不参与。
    [[nodiscard]] std::int64_t LobbySeed() const;
    // 由已验票据播种一名玩家的开局手牌/能量，并（首张票据）设定对局级大厅种子。
    // deck_card_ids（W-1 phase 2，票据 field 18）：大厅权威有序牌堆；非空时播种
    // HandState.deck 且 draw_cursor = hand_card_ids.size()（补抽从大厅发牌游标续跑）。
    // 空 = 未下发牌堆（旧票据：DrawServerCard 保持 no-op）。
    // 返回 false：玩家未知、种子文本非法、或与已设定的对局种子冲突（保持首个值）。
    bool SeedPlayerHand(
        const std::string& player_id,
        const std::vector<std::string>& hand_card_ids,
        double initial_energy,
        const std::string& server_seed_decimal,
        const std::vector<std::string>& deck_card_ids = {}
    );
    [[nodiscard]] std::size_t HandSize(const std::string& player_id) const;
    [[nodiscard]] double PlayerEnergy(const std::string& player_id) const;
    [[nodiscard]] std::int64_t PlayerInvulnerableUntil(const std::string& player_id) const;
    // B3 命中判定观测点（簿记，不进哈希）：命中次数 / 擦弹次数。
    [[nodiscard]] std::uint64_t PlayerHitCount(const std::string& player_id) const;
    [[nodiscard]] std::uint64_t PlayerGrazeCount(const std::string& player_id) const;
    // 执行器收集的事件行（ oldest first；Step 2 消费/清空）。
    [[nodiscard]] const std::vector<CardCastEvent>& CardCastEvents() const;
    void ClearCardCastEvents();
    // 取走并移除前 limit 行（Step 2 回传消费；limit=0 表示不限）。
    // 失败重发时用 RestoreCardCastEvents 放回队首。
    [[nodiscard]] std::vector<CardCastEvent> TakeCardCastEvents(std::size_t limit = 0);
    void RestoreCardCastEvents(const std::vector<CardCastEvent>& rows);

private:
    struct PlayerState {
        std::string player_id;
        std::int32_t x_milli = 0;
        std::int32_t y_milli = 0;
        std::uint64_t last_seq = 0;
        bool connected = true;
        // Consecutive simulation ticks without a real client input.
        std::uint32_t ticks_without_input = 0;
        BattleInput last_input;
        // T3 step4：局内手牌状态（由票据播种，跨 tick 保持）。
        HandState hand;
        // invulnerable 效果到期 tick（Go InvulnerableUntil 语义）。B3 起由
        // ResolveBulletContacts 消费：tick > invulnerable_until 才受伤。
        std::int64_t invulnerable_until = 0;
        // B3 命中/擦弹簿记（派生自接触判定）。**不进 CanonicalStateHash**：
        // 簿记值不得扰动签名结果哈希（TODO §十 教训）。
        std::uint64_t hit_count = 0;
        std::uint64_t graze_count = 0;
        // 擦弹去重集（Go GrazedBullets）：同一颗子弹对同一玩家只算一次。
        std::set<std::string> grazed_bullet_ids;
    };

    struct BulletState {
        std::string bullet_id;
        std::int32_t x_milli = 0;
        std::int32_t y_milli = 0;
        std::int32_t vx_milli = 0;
        std::int32_t vy_milli = 0;
        std::uint32_t radius_milli = 4000;
        std::string pattern_id = "basic_radial";
        std::string color = "red";
    };

    [[nodiscard]] std::pair<std::int32_t, std::int32_t> BossSpawnPointForIndex(
        std::size_t player_index
    ) const;
    [[nodiscard]] std::uint64_t MixSeed(std::uint64_t value) const;
    [[nodiscard]] bool BossReadyToStartForTick(std::uint64_t tick) const;
    [[nodiscard]] bool BossDefeated() const;
    [[nodiscard]] bool BattleInputEnabledForTick(std::uint64_t tick) const;
    [[nodiscard]] std::string CanonicalStateHash() const;
    [[nodiscard]] BattleInput InputForTick(const PlayerState& player) const;
    void ApplyInput(PlayerState& player, const BattleInput& input, std::uint64_t applied_tick);
    void ApplyBossDamageForInput(const PlayerState& player, const BattleInput& input, std::uint64_t applied_tick);
    void ApplyModeActionsForTick(std::uint64_t tick);
    void ApplyReadyModeAction(const BattleModeAction& action);
    bool ApplyTransferCardModeAction(const BattleModeAction& action);
    // T3 step4：出牌执行器内核（两通道共用）。slot == -1 为不出牌哨兵
    // （无效果、无事件行、无状态变更）；拒绝记 card_rejected；通过则落账本、
    // boss 伤害、效果与 card_accepted/card_effect_applied 事件行。
    void ExecuteCastCard(const std::string& player_id, int slot, std::int64_t tick);
    void ApplyCastCardModeAction(const BattleModeAction& action, std::int64_t tick);
    // 事件行入队（容量上限内丢最旧）。
    void PushCardCastEvent(CardCastEvent row);
    // T10 rule 2：整支队伍都被动掉线时发一次对局级信号（去重 latch）。只在
    // Tick() 尾部调用（current_tick_ 已推进），故 row.tick 是终局 tick。
    void MaybeEmitAllPlayersDisconnected();
    void RunCardEffect(
        const PlayerState& caster,
        const std::string& caster_id,
        const CardActivation& activation,
        std::int64_t tick
    );
    void ApplyBulletDeltas(const std::vector<CardBulletDelta>& deltas);
    [[nodiscard]] std::vector<CardBulletSnapshot> BuildBulletSnapshots() const;
    // 通用 boss 伤害记账（ApplyBossDamageForInput 与出牌执行器共用）；
    // 归零致死时写 boss_defeated trace 并 ++event_count_，返回是否致死。
    bool RegisterBossDamage(const std::string& player_id, std::uint64_t tick, std::uint64_t damage);
    [[nodiscard]] std::string TransferCardAuditMaterial() const;
    void SpawnBulletsForTick();
    void AdvanceBullets();
    // B3：接触判定（命中/擦弹 + 无敌窗口消费）。在 AdvanceBullets() 之后调用，
    // 与 Go 的 emit→move→resolve 同序（simulation.go:37-42）。
    void ResolveBulletContacts();
    void AccumulateAcceptedInput(const BattleInput& input);
    void AccumulateFallbackInput(const PlayerState& player, const BattleInput& input);
    void AccumulateAcceptedModeAction(const BattleModeAction& action);
    void AccumulateConnectionEvent(const PlayerState& player);

    SimulationConfig config_;
    std::uint64_t current_tick_ = 0;
    std::uint64_t next_bullet_id_ = 1;
    std::uint64_t accepted_input_count_ = 0;
    std::uint64_t fallback_input_count_ = 0;
    std::uint64_t neutral_fallback_count_ = 0;
    std::uint64_t held_input_fallback_count_ = 0;
    std::uint64_t mode_action_count_ = 0;
    std::uint64_t input_buffer_peak_tick_count_ = 0;
    std::uint64_t input_buffer_peak_record_count_ = 0;
    std::uint64_t mode_action_buffer_peak_tick_count_ = 0;
    std::uint64_t mode_action_buffer_peak_record_count_ = 0;
    std::uint64_t input_stream_hash_ = 1469598103934665603ull;
    std::uint64_t event_stream_hash_ = 1469598103934665603ull;
    std::uint64_t event_count_ = 0;
    std::uint64_t boss_max_hp_ = 0;
    std::uint64_t boss_current_hp_ = 0;
    std::uint64_t boss_damage_total_ = 0;
    std::uint64_t boss_defeated_tick_ = 0;
    std::uint64_t transfer_card_count_ = 0;
    bool boss_combat_started_ = false;
    bool has_last_mode_action_ = false;
    BattleModeAction last_mode_action_;
    std::map<std::string, PlayerState> players_;
    std::set<std::string> ready_player_ids_;
    std::set<std::string> pending_ready_player_ids_;
    std::set<std::string> pending_reconnect_player_ids_;
    std::map<std::string, std::uint64_t> boss_damage_by_player_;
    std::map<std::string, TransferableCardState> transferable_cards_;
    std::map<std::string, TransferableCardState> pending_transfer_card_authority_by_action_id_;
    std::set<std::string> accepted_mode_action_ids_;
    std::set<std::string> reserved_transfer_card_instance_ids_;
    std::map<std::string, std::pair<std::string, std::string>> transferred_card_edges_;
    std::map<std::string, TransferableCardState> transferred_card_authority_by_card_instance_id_;
    std::string last_transfer_card_instance_id_;
    std::string last_transfer_from_player_id_;
    std::string last_transfer_to_player_id_;
    TransferableCardState last_transfer_card_authority_;
    std::map<std::uint64_t, std::map<std::string, BattleInput>> pending_inputs_by_tick_;
    std::map<std::uint64_t, std::vector<BattleModeAction>> pending_mode_actions_by_tick_;
    std::vector<BulletState> bullets_;
    // T3 step4：票据 server_seed（十进制文本）解析出的大厅权威种子；仅
    // ApplyBulletPurge 使用（T7 H-2）。首张含种子的票据设定。
    std::int64_t card_purge_seed_ = 0;
    bool card_purge_seed_set_ = false;
    std::vector<CardCastEvent> card_cast_events_;
    // T10 rule 2：整队被动掉线的对局级信号只发一次（派生自 connected 标志，
    // 不进 CanonicalStateHash，避免扰动确定性金标准）。
    bool all_players_disconnected_emitted_ = false;
    std::vector<std::string> input_trace_;
    std::vector<std::string> event_trace_;
    OperationChainLog operation_log_;
};

[[nodiscard]] std::string InputValidationCodeName(InputValidationCode code);
[[nodiscard]] std::string CanonicalReplayInputStreamSummaryRecord(
    const ReplayInputStreamSummaryRecord& record
);
[[nodiscard]] std::string DevReplayInputStreamSummaryHash(
    const ReplayInputStreamSummaryRecord& record
);
[[nodiscard]] std::string CanonicalReplayFixturePayload(const ReplayFixture& fixture);
[[nodiscard]] std::string DevReplayFixtureHash(const ReplayFixture& fixture);
[[nodiscard]] std::string CanonicalReplayLoadoutBridgePayload(
    const std::vector<ReplayLoadoutBridge>& loadout
);
[[nodiscard]] std::string CanonicalReplayRecordBridgePayload(const ReplayRecordBridge& record);
[[nodiscard]] std::string DevReplayRecordBridgeHash(const ReplayRecordBridge& record);
[[nodiscard]] std::string DevModeResultJsonFromReplayFixture(const ReplayFixture& fixture);
[[nodiscard]] std::string DevResultHashFromReplaySummary(const ReplaySummary& summary);
[[nodiscard]] std::string DevReplayIdFromReplaySummary(const ReplaySummary& summary);

}  // namespace phk::battle
