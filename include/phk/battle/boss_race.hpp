#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "phk/battle/danmaku/boss_catalog.hpp"
#include "phk/battle/danmaku/bullet_motion.hpp"
#include "phk/battle/danmaku/pattern_timeline.hpp"
#include "phk/battle/danmaku/player_loadout.hpp"
#include "phk/battle/protocol.hpp"
#include "phk/battle/simulation.hpp"  // CardCastEvent（F3 事件收集器）

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

// 每玩家自机 loadout（任务「自机机制」）。字段名与 Go 侧 BattleAgentLoadout
// 及 .dem header 的 players[].loadout{character_id,shot_type,bomb_type} 对齐：
//   * character_id 目前仅作展示/透传（协议白名单在 Go）；
//   * shot_type ∈ danmaku::PlayerShotTable() 的 name（如 "ReimuA1"）；
//   * bomb_type ∈ danmaku::PlayerBombTable() 的 name（如 "Reimu"）。
// 任一为空 = 走默认自机行为（复现现状）。
struct PlayerLoadoutSpec {
    std::string player_id;
    std::string character_id;
    std::string shot_type;
    std::string bomb_type;
};

struct BossRaceConfig {
    std::string match_id;
    std::string mode_id = kMvpBossRaceModeId;
    std::string ruleset_version = "tna-1.40";
    std::uint64_t match_seed = 0;
    std::uint64_t boss_max_hp = kBossRaceDefaultBossHp;
    std::uint32_t tick_rate_hz = kBossRaceTickRateHz;
    std::uint32_t pattern_period_ticks = kBossRacePatternPeriodTicks;
    // 房间级 boss 选择（空 = 默认 timeline，逐字节保持现状）。
    std::string boss_id;
    // 每玩家自机 loadout（空 = 默认自机行为）。
    std::vector<PlayerLoadoutSpec> loadouts;
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
    // 真实弹幕扩展（tna-1.40）：原版 kEnemyBullet 值 + bulletFunc 编号。
    std::int32_t bullet_kind = 0;
    std::int32_t motion_func = 0;
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

// Returns the ten pattern ids, in scheduler order (real tna-1.40 patterns).
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

    // 已解析的 boss 目录项（boss_id 为空或未知时返回 nullptr = 默认 timeline）。
    [[nodiscard]] const danmaku::BossCatalogEntry* BossEntry() const;
    // 该 tick 使用的库内 pattern index（boss 目录展开；默认路径 = 原 10 模式轮转）。
    [[nodiscard]] std::size_t PatternIndexForTick(std::uint64_t tick) const;
    // 玩家已解析的自机射击/炸弹 spec（无 loadout / 名称未知返回 nullptr）。
    [[nodiscard]] const danmaku::PlayerShotSpec* PlayerShotSpecFor(const std::string& player_id) const;
    [[nodiscard]] const danmaku::PlayerBombSpec* PlayerBombSpecFor(const std::string& player_id) const;

    bool AddPlayer(const std::string& player_id, std::int32_t x_milli, std::int32_t y_milli);
    bool SetPlayerConnected(const std::string& player_id, bool connected);

    // F3 事件收集器（与 BattleSimulation 的 card_cast_events_ 同形）：
    // Push 入队（超过 kCardCastEventRowLimit 时丢弃，防宿主不上报时无界增长），
    // Take 排空（limit=0 = 全部），Restore 失败重发时放回队首。
    // MVP 只承载对局级 all_players_disconnected（不带 user_id / card_id）。
    void PushEvent(CardCastEvent row);
    [[nodiscard]] std::vector<CardCastEvent> TakeEvents(std::size_t limit = 0);
    void RestoreEvents(const std::vector<CardCastEvent>& rows);

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

    // 带完整运动学状态的实时子弹（真实弹幕）。
    struct LiveBullet {
        std::string bullet_id;
        std::string owner_player_id;
        std::string pattern_id;
        std::uint32_t pattern_index = 0;
        std::uint32_t spawn_index = 0;
        danmaku::BulletMotion motion;
    };

    void ResolveBossAndLoadouts();
    void StartIfReady();
    void ApplyPlayerTick(std::uint64_t tick);
    void SpawnBulletsForTick(std::uint64_t tick);
    void SpawnRealBulletsForTick(std::uint64_t tick);
    void SpawnSyntheticBulletsForTick(std::uint64_t tick);  // mvp-boss-race-s0 兼容
    void AdvanceBullets(std::uint64_t tick);
    void CheckRaceWinner(std::uint64_t tick);
    [[nodiscard]] const PlayerState* FirstOpponentOf(const std::string& player_id) const;
    [[nodiscard]] std::size_t EmittedBulletsFor(const std::string& player_id) const;

    BossRaceConfig config_;
    std::uint64_t current_tick_ = 0;
    std::uint64_t next_bullet_id_ = 1;
    BossRaceState state_ = BossRaceState::Waiting;
    std::string winner_player_id_;
    std::uint64_t winner_tick_ = 0;
    std::map<std::string, PlayerState> players_;
    std::map<std::uint64_t, std::map<std::string, BattleInput>> pending_inputs_by_tick_;
    std::vector<LiveBullet> live_bullets_;
    // 解析后的 boss / 自机（构造期固定，运行期不可变 → 确定性）。
    const danmaku::BossCatalogEntry* boss_entry_ = nullptr;
    std::map<std::string, PlayerLoadoutSpec> loadout_by_player_;
    std::map<std::string, const danmaku::PlayerShotSpec*> shot_by_player_;
    std::map<std::string, const danmaku::PlayerBombSpec*> bomb_by_player_;
    // F3 事件收集器（见 PushEvent 注释）。
    std::vector<CardCastEvent> events_;
};

}  // namespace phk::battle
