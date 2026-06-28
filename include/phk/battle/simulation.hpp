#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "phk/battle/protocol.hpp"

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
    TickTooOld,
    TickTooFarAhead,
    InvalidDirectionBits,
    InvalidCardSlot,
    InvalidModeAction,
    PlayerDisconnected,
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
    std::uint64_t event_count = 0;
    std::uint64_t last_mode_action_tick = 0;
    std::uint64_t last_mode_action_seq = 0;
};

struct SimulationConfig {
    std::string match_id;
    std::string mode_id;
    std::string ruleset_version = std::string(kDefaultRulesetVersion);
    std::uint64_t match_seed = 0;
    std::uint32_t tick_rate_hz = kBattleTickRateHz;
    std::uint32_t max_input_ahead_ticks = 8;
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

    bool AddPlayer(const std::string& player_id, std::int32_t x_milli, std::int32_t y_milli);
    [[nodiscard]] InputValidationResult SetPlayerConnected(const std::string& player_id, bool connected);
    [[nodiscard]] InputValidationResult ValidateInput(const BattleInput& input) const;
    InputValidationResult AcceptInput(const BattleInput& input);
    [[nodiscard]] InputValidationResult ValidateModeAction(const BattleModeAction& action) const;
    InputValidationResult AcceptModeAction(const BattleModeAction& action);
    BattleSnapshot Tick();
    [[nodiscard]] BattleSnapshot Snapshot(std::string snapshot_kind = "full") const;
    [[nodiscard]] ReplaySummary Summary() const;

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
    void SpawnBulletsForTick();
    void AdvanceBullets();
    void AccumulateAcceptedInput(const BattleInput& input);
    void AccumulateAcceptedModeAction(const BattleModeAction& action);
    void AccumulateConnectionEvent(const PlayerState& player);

    SimulationConfig config_;
    std::uint64_t current_tick_ = 0;
    std::uint64_t next_bullet_id_ = 1;
    std::uint64_t accepted_input_count_ = 0;
    std::uint64_t input_stream_hash_ = 1469598103934665603ull;
    std::uint64_t event_stream_hash_ = 1469598103934665603ull;
    std::uint64_t event_count_ = 0;
    bool has_last_mode_action_ = false;
    BattleModeAction last_mode_action_;
    std::map<std::string, PlayerState> players_;
    std::map<std::uint64_t, std::map<std::string, BattleInput>> pending_inputs_by_tick_;
    std::vector<BulletState> bullets_;
};

[[nodiscard]] std::string InputValidationCodeName(InputValidationCode code);

}  // namespace phk::battle
