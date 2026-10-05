#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "phk/battle/protocol.hpp"

// MVP "mvp_boss_race" simulation.
//
// Two players each face an identical Boss copy (same match seed, so the bullet
// patterns are byte-for-byte identical). Whoever defeats their own Boss first
// wins the race. This module is intentionally additive: it does not touch the
// existing shared-Boss `BattleSimulation` modes (world_boss / instance_boss),
// so their invariants and tests stay intact. It will be folded into the
// authoritative server transport in a later step.
namespace phk::battle {

inline constexpr const char* kMvpBossRaceModeId = "mvp_boss_race";
inline constexpr std::uint32_t kBossRaceTickRateHz = 60;
inline constexpr std::uint64_t kBossRaceDefaultBossHp = 6000;
inline constexpr std::uint64_t kBossRaceDamagePerShotTick = 10;
inline constexpr std::uint32_t kBossRacePatternPeriodTicks = 45;
inline constexpr std::uint32_t kBossRaceMaxBulletsPerPlayer = 384;
inline constexpr std::size_t kBossRaceMaxPlayers = 2;
inline constexpr std::size_t kBossRaceMinPlayers = 2;
inline constexpr std::size_t kBossRacePatternCount = 10;

enum class BossRaceState {
    Waiting,
    Running,
    Finished,
};

struct BossRaceConfig {
    std::string match_id;
    std::string mode_id = kMvpBossRaceModeId;
    std::string ruleset_version = "mvp-boss-race-s0";
    std::uint64_t match_seed = 0;
    std::uint64_t boss_max_hp = kBossRaceDefaultBossHp;
    std::uint32_t tick_rate_hz = kBossRaceTickRateHz;
    std::uint32_t pattern_period_ticks = kBossRacePatternPeriodTicks;
};

struct BossRaceBullet {
    std::string bullet_id;
    std::string owner_player_id;  // the player whose Boss copy fired it
    std::int32_t x_milli = 0;
    std::int32_t y_milli = 0;
    std::int32_t vx_milli = 0;
    std::int32_t vy_milli = 0;
    std::uint32_t radius_milli = 4000;
    std::string pattern_id;
};

struct BossRacePlayerSnapshot {
    std::string player_id;
    std::int32_t x_milli = 0;
    std::int32_t y_milli = 0;
    std::uint64_t boss_current_hp = 0;
    std::uint64_t damage_dealt = 0;
    bool connected = true;
};

struct BossRaceSnapshot {
    std::uint64_t tick = 0;
    BossRaceState state = BossRaceState::Waiting;
    std::string winner_player_id;
    std::uint64_t winner_tick = 0;
    std::vector<BossRacePlayerSnapshot> players;
    std::vector<BossRaceBullet> bullets;
    std::string state_hash;
};

// Returns the ten MVP pattern ids, in scheduler order.
[[nodiscard]] const std::vector<std::string>& BossRacePatternIds();

// Deterministic RNG shared with the Go / client convention:
// random input = (match_seed, tick, pattern_index, spawn_index).
[[nodiscard]] std::uint32_t BossRaceDeterministicU32(
    std::uint64_t seed,
    std::uint64_t tick,
    std::uint32_t pattern_index,
    std::uint32_t spawn_index
);
[[nodiscard]] double BossRaceDeterministicUnit(
    std::uint64_t seed,
    std::uint64_t tick,
    std::uint32_t pattern_index,
    std::uint32_t spawn_index
);

class BossRaceSimulation {
public:
    explicit BossRaceSimulation(BossRaceConfig config);

    [[nodiscard]] const BossRaceConfig& Config() const;
    [[nodiscard]] BossRaceState State() const;
    [[nodiscard]] std::uint64_t CurrentTick() const;
    [[nodiscard]] std::size_t PlayerCount() const;
    [[nodiscard]] const std::string& WinnerPlayerId() const;
    [[nodiscard]] std::string PatternIdForTick(std::uint64_t tick) const;

    bool AddPlayer(const std::string& player_id, std::int32_t x_milli, std::int32_t y_milli);
    bool SetPlayerConnected(const std::string& player_id, bool connected);

    // Validates seq/tick monotonicity, buffers the input for its target tick.
    bool SubmitInput(const BattleInput& input);

    // Advances one fixed tick and returns the resulting snapshot.
    BossRaceSnapshot Tick();

    [[nodiscard]] BossRaceSnapshot Snapshot() const;
    [[nodiscard]] std::string CanonicalStateHash() const;

private:
    struct PlayerState {
        std::string player_id;
        std::int32_t x_milli = 0;
        std::int32_t y_milli = 0;
        std::uint64_t last_seq = 0;
        std::uint64_t boss_current_hp = 0;
        std::uint64_t damage_dealt = 0;
        bool connected = true;
        bool alive = true;
        BattleInput last_input;
    };

    void StartIfReady();
    void ApplyPlayerTick(std::uint64_t tick);
    void SpawnBulletsForTick(std::uint64_t tick);
    void AdvanceBullets();
    void CheckRaceWinner(std::uint64_t tick);
    [[nodiscard]] const PlayerState* FirstOpponentOf(const std::string& player_id) const;

    BossRaceConfig config_;
    std::uint64_t current_tick_ = 0;
    std::uint64_t next_bullet_id_ = 1;
    BossRaceState state_ = BossRaceState::Waiting;
    std::string winner_player_id_;
    std::uint64_t winner_tick_ = 0;
    std::map<std::string, PlayerState> players_;
    std::map<std::uint64_t, std::map<std::string, BattleInput>> pending_inputs_by_tick_;
    std::vector<BossRaceBullet> bullets_;
};

}  // namespace phk::battle
