#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "phk/battle/boss_race.hpp"  // PlayerLoadoutSpec（自机 loadout 结构）
#include "phk/battle/protocol.hpp"
#include "phk/battle/simulation.hpp"  // CardCastEvent（F3 事件收集器形状）

// 合作模式 "coop_boss"：2 或 4 名玩家共享同一个 Boss（池中心），各自从不同
// 方向进攻。自机方向按座位完全相对（2 人=上下；4 人=四向）。
//
// 与 BossRaceSimulation 的关系：**加法式新模块**，不触碰 race 的任何代码路径
// （mvp_boss_race 行为与既有测试逐字节不变）。子弹用简单线性运动（毫厘单位），
// 不走 danmaku 运动库——coop 是自机向心的对称竞技场，race 的屏幕空间弹幕库
// 语义不适用；确定性统一走 lobby_rng 的 LobbyDeterministicUnit（与大厅同源）。
//
// 机制（COOP_MODE_CONTRACT.md §2.3，常量冻结）：
//   * fan_attack   多向齐射：每 90 tick 向每个座位方向同时 7 发扇形；
//   * shield       方向格挡：窗口期 Boss 格挡 1 或 3 个方向，该方向伤害归零；
//   * cannon_lock  巨炮封锁：每 600 tick 的后 150 tick 向某方向巨宽弹幕，
//                   该座位玩家位移与射击冻结（封锁）；
//   * spiral       全屏旋转：窗口期每 6 tick 四向对称一发，角度随 tick 旋转。
namespace phk::battle {

inline constexpr const char* kCoopBossModeId = "coop_boss";
// 64 tick 基准（计划 §2.1）：真实加速，非重采样；周期常量数值原样不动。
inline constexpr std::uint32_t kCoopBossTickRateHz = 64;
inline constexpr std::int32_t kCoopArenaHalfMilli = 90000;
inline constexpr std::uint64_t kCoopBossHpPerPlayer = 5000;
inline constexpr std::uint64_t kCoopDamagePerShotTick = 10;
inline constexpr std::int32_t kCoopPlayerSpawnMilli = 70200;  // 0.78 × half
inline constexpr std::int32_t kCoopMoveMilli = 3000;
inline constexpr std::size_t kCoopMaxPlayers = 4;
inline constexpr std::size_t kCoopMinPlayers = 2;
inline constexpr std::uint32_t kCoopShieldWindowTicks = 180;
inline constexpr std::uint32_t kCoopFanIntervalTicks = 90;
inline constexpr std::uint32_t kCoopFanBulletsPerDir = 7;
inline constexpr std::uint32_t kCoopCannonIntervalTicks = 600;
inline constexpr std::uint32_t kCoopCannonStartOffset = 450;
// 巨炮段 [450,600) = 150 tick：以契约 §2.3.5 冻结时间表为准（§2.2 的 120 为笔误，
// main 2026-10-22 裁决；窗口在 600 tick 大循环边界收口）。
inline constexpr std::uint32_t kCoopCannonDurationTicks = 150;
inline constexpr std::uint32_t kCoopCannonBulletRadiusMilli = 12000;
inline constexpr std::uint32_t kCoopSpiralWindowTicks = 300;
inline constexpr std::uint32_t kCoopSpiralEveryTicks = 6;
inline constexpr std::uint32_t kCoopSpiralRotationStepMilliDeg = 900;
inline constexpr std::size_t kCoopMaxBullets = 2048;

// 弹型：0=fan 1=cannon 2=spiral（快照 wire kind，契约 §2.4）。
enum class CoopBulletKind : std::uint32_t {
    Fan = 0,
    Cannon = 1,
    Spiral = 2,
};

enum class CoopBossState {
    Waiting,
    Running,
    Finished,
};

struct CoopBossConfig {
    std::string match_id;
    std::string mode_id = kCoopBossModeId;
    std::string ruleset_version = "coop-boss-s0";
    std::int64_t match_seed = 0;
    // 0 = 5000 × 玩家数（AddPlayer 后由 StartIfReady 定填）。
    std::uint64_t boss_max_hp = 0;
    std::uint32_t tick_rate_hz = kCoopBossTickRateHz;
    // P4 难度（默认 1=normal；透传快照 difficulty 字段，客户端只消费。
    // coop 自身的发射缩放不在本契约内——契约只覆盖 race 的 BossRaceConfig）。
    std::int32_t difficulty = 1;
    std::vector<PlayerLoadoutSpec> loadouts;
};

struct CoopBossBullet {
    std::string bullet_id;
    // fan/cannon：目标座位玩家（观察方）；spiral："boss"（全场共有）。
    std::string owner_player_id;
    std::int32_t x_milli = 0;
    std::int32_t y_milli = 0;
    std::int32_t vx_milli = 0;
    std::int32_t vy_milli = 0;
    std::uint32_t radius_milli = 4000;
    std::uint32_t kind = 0;
    std::string pattern_id;
    // §2.8 增补（coop 没有链式分裂，恒为 0/0/false；字段存在以统一双端解析）。
    std::uint32_t chain_id = 0;
    std::uint32_t generation = 0;
    bool hidden = false;
};

struct CoopBossPlayerSnapshot {
    std::string player_id;
    std::uint32_t seat = 0;
    std::int32_t x_milli = 0;
    std::int32_t y_milli = 0;
    // 毫度，0=上，9000=右，18000=下，27000=左（契约 §2.2 座位表）。
    std::int32_t facing_millideg = 0;
    std::uint64_t damage_dealt = 0;
    bool connected = true;
    bool locked = false;     // cannon 封锁中（位移/射击冻结）
    bool shielded = false;   // 本方向被格挡（伤害归零）
    // §2.8 增补（与 race 同名字段同语义）。
    std::int32_t vx_milli = 0;
    std::int32_t vy_milli = 0;
    std::uint32_t lives = kSnapshotDefaultLives;
    std::uint32_t bombs = kSnapshotDefaultBombs;
    std::uint64_t invulnerable_until_tick = 0;
    std::uint32_t graze = 0;
};

struct CoopBossSnapshot {
    std::uint64_t tick = 0;
    CoopBossState state = CoopBossState::Waiting;
    // P4 难度（客户端只消费；与 race 快照同名字段同语义）。
    std::int32_t difficulty = 1;
    std::uint64_t boss_hp = 0;
    std::uint64_t boss_max_hp = 0;
    std::uint32_t seat_count = 0;
    std::uint32_t shield_dir_mask = 0;  // bit i = 座位 i 被格挡
    std::int32_t lock_dir = -1;          // 巨炮目标座位，-1 = 无
    std::uint64_t lock_until_tick = 0;
    bool spiral_active = false;
    std::vector<CoopBossPlayerSnapshot> players;
    std::vector<CoopBossBullet> bullets;
    std::string state_hash;
    // §2.8 增补（共享 Boss 视角；coop 无链树/手牌/判定事件，P1 保持默认空值）。
    BossStateWire boss;
    std::vector<BossChainWire> chains;
    std::vector<BossHandCardWire> hand;
    std::uint32_t deck_remaining = 0;
    std::uint32_t draw_cooldown_ticks = 0;
    std::vector<BossEventWire> events;
    std::uint64_t event_cursor = 0;
};

[[nodiscard]] bool IsCoopBossMode(const std::string& mode_id);

// 座位的出生点与朝向（seat 按 sorted(player_ids) 下标；2p: 0=下朝上/1=上朝下；
// 4p: 0=下朝上/1=右朝左/2=上朝下/3=左朝右）。
[[nodiscard]] void CoopSeatSpawnMilli(std::size_t seat_count, std::size_t seat, std::int32_t* x_milli, std::int32_t* y_milli);
[[nodiscard]] std::int32_t CoopSeatFacingMilliDeg(std::size_t seat_count, std::size_t seat);

class CoopBossSimulation {
public:
    explicit CoopBossSimulation(CoopBossConfig config);

    [[nodiscard]] const CoopBossConfig& Config() const;
    [[nodiscard]] CoopBossState State() const;
    [[nodiscard]] std::uint64_t CurrentTick() const;
    [[nodiscard]] std::size_t PlayerCount() const;
    [[nodiscard]] bool BossDefeated() const;

    bool AddPlayer(const std::string& player_id, std::int32_t x_milli, std::int32_t y_milli);
    bool SetPlayerConnected(const std::string& player_id, bool connected);

    // F3 事件收集器（形状同 BossRaceSimulation / MatchServer 用法）。
    void PushEvent(CardCastEvent row);
    [[nodiscard]] std::vector<CardCastEvent> TakeEvents(std::size_t limit = 0);
    void RestoreEvents(const std::vector<CardCastEvent>& rows);

    // 输入校验语义与 BossRaceSimulation::SubmitInput 对齐（seq/tick 单调、
    // 8 向 bitmask、tick 窗口 ±8）。方向 bit 在战场坐标系（不随座位旋转）。
    bool SubmitInput(const BattleInput& input);

    [[nodiscard]] CoopBossSnapshot Tick();
    [[nodiscard]] CoopBossSnapshot Snapshot() const;
    [[nodiscard]] std::string CanonicalStateHash() const;

    // 机制查询（测试/断言用）。
    [[nodiscard]] std::uint32_t ShieldDirMask() const { return shield_dir_mask_; }
    [[nodiscard]] std::int32_t LockDir() const { return lock_dir_; }
    [[nodiscard]] bool SpiralActive() const { return spiral_active_; }

private:
    struct PlayerState {
        std::string player_id;
        std::int32_t x_milli = 0;
        std::int32_t y_milli = 0;
        // 本 tick 位移速度（§2.8 快照 vx/vy；封锁时为 0）。
        std::int32_t vx_milli = 0;
        std::int32_t vy_milli = 0;
        std::uint64_t last_seq = 0;
        std::uint64_t damage_dealt = 0;
        bool connected = true;
        BattleInput last_input;
    };

    struct LiveBullet {
        std::string bullet_id;
        std::string owner_player_id;
        std::int32_t x_milli = 0;
        std::int32_t y_milli = 0;
        std::int32_t vx_milli = 0;
        std::int32_t vy_milli = 0;
        std::uint32_t radius_milli = 4000;
        std::uint32_t kind = 0;
        std::string pattern_id;
    };

    void ResolveLoadouts();
    void StartIfReady();
    void RecomputeMechanics(std::uint64_t tick);
    void ApplyPlayerTick(std::uint64_t tick);
    void SpawnBulletsForTick(std::uint64_t tick);
    void AdvanceBullets();
    void CheckBossDefeated(std::uint64_t tick);
    [[nodiscard]] std::uint32_t SeatOf(const std::string& player_id) const;
    [[nodiscard]] const danmaku::PlayerShotSpec* ShotSpecFor(const std::string& player_id) const;

    CoopBossConfig config_;
    std::uint64_t current_tick_ = 0;
    std::uint64_t next_bullet_id_ = 1;
    CoopBossState state_ = CoopBossState::Waiting;
    std::uint64_t boss_hp_ = 0;
    std::map<std::string, PlayerState> players_;  // key 序 = 座位序（确定性）
    std::map<std::uint64_t, std::map<std::string, BattleInput>> pending_inputs_by_tick_;
    std::vector<LiveBullet> live_bullets_;
    std::map<std::string, const danmaku::PlayerShotSpec*> shot_by_player_;
    // 机制状态（每 tick 由 RecomputeMechanics 重算，纯 schedule+seed 函数）。
    std::uint32_t shield_dir_mask_ = 0;
    std::int32_t lock_dir_ = -1;
    std::uint64_t lock_until_tick_ = 0;
    bool spiral_active_ = false;
    // F3 事件收集器。
    std::vector<CardCastEvent> events_;
};

}  // namespace phk::battle
