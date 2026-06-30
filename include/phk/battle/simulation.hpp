#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "phk/battle/protocol.hpp"
#include "phk/battle/result.hpp"

namespace phk::battle {

inline constexpr std::uint32_t kBattleTickRateHz = 60;
inline constexpr std::int32_t kArenaHalfWidthMilli = 120000;
inline constexpr std::int32_t kArenaHalfHeightMilli = 90000;

enum class InputValidationCode {
    Ok,
    VersionIncompatible,
    MatchUnknown,
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
    std::uint64_t final_tick = 0;
    std::uint64_t input_count = 0;
    std::uint64_t fallback_input_count = 0;
    std::uint64_t neutral_fallback_count = 0;
    std::uint64_t held_input_fallback_count = 0;
    std::uint64_t mode_action_count = 0;
    std::uint64_t event_count = 0;
    std::uint64_t last_mode_action_tick = 0;
    std::uint64_t last_mode_action_seq = 0;
    std::vector<std::string> input_trace;
    std::vector<std::string> event_trace;
};

struct ReplayInputStreamSummaryRecord {
    VersionStamp version;
    std::string replay_id;
    std::string owner_user_id;
    std::string match_id;
    std::uint64_t input_count = 0;
    std::uint64_t event_count = 0;
    std::string input_stream_hash;
    std::string event_stream_hash;
    std::string final_state_hash;
    std::uint64_t final_tick = 0;
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
    std::uint32_t spawn_period_ticks = 30;
    std::uint32_t max_bullets = 256;
};

class BattleSimulation {
public:
    explicit BattleSimulation(SimulationConfig config);

    [[nodiscard]] const SimulationConfig& Config() const;
    [[nodiscard]] std::uint64_t CurrentTick() const;
    [[nodiscard]] std::size_t PlayerCount() const;
    [[nodiscard]] std::size_t BulletCount() const;
    [[nodiscard]] std::uint64_t AcceptedInputCount() const;
    [[nodiscard]] bool IsPlayerConnected(const std::string& player_id) const;

    bool AddPlayer(const std::string& player_id, std::int32_t x_milli, std::int32_t y_milli);
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

private:
    struct PlayerState {
        std::string player_id;
        std::int32_t x_milli = 0;
        std::int32_t y_milli = 0;
        std::uint64_t last_seq = 0;
        bool connected = true;
        BattleInput last_input;
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

    [[nodiscard]] std::uint64_t MixSeed(std::uint64_t value) const;
    [[nodiscard]] std::string CanonicalStateHash() const;
    [[nodiscard]] BattleInput InputForTick(const PlayerState& player) const;
    void ApplyInput(PlayerState& player, const BattleInput& input);
    void ApplyModeActionsForTick(std::uint64_t tick);
    void SpawnBulletsForTick();
    void AdvanceBullets();
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
    std::uint64_t input_stream_hash_ = 1469598103934665603ull;
    std::uint64_t event_stream_hash_ = 1469598103934665603ull;
    std::uint64_t event_count_ = 0;
    bool has_last_mode_action_ = false;
    BattleModeAction last_mode_action_;
    std::map<std::string, PlayerState> players_;
    std::map<std::uint64_t, std::map<std::string, BattleInput>> pending_inputs_by_tick_;
    std::map<std::uint64_t, std::vector<BattleModeAction>> pending_mode_actions_by_tick_;
    std::vector<BulletState> bullets_;
    std::vector<std::string> input_trace_;
    std::vector<std::string> event_trace_;
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
