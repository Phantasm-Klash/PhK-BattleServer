#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "phk/battle/danmaku/boss_catalog.hpp"
#include "phk/battle/danmaku/bullet_motion.hpp"
#include "phk/battle/danmaku/chain_split.hpp"
#include "phk/battle/danmaku/difficulty_table.hpp"
#include "phk/battle/danmaku/judgment.hpp"
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
// 64 tick 基准（DANMAKU_CLIENT_ENGINE_PLAN §2.1，用户 2026-10-24 裁决）：
// 这是**真实加速**（每秒多算 4 tick），不是从 60 重采样。因此下面所有
// 「每帧」常量（速度、角度增量、周期 tick 数）数值原样保留，直接当每 tick 用；
// 禁止任何 ×60/64 换算，也禁止引入 FramesToTicks 之类的缩放工具。
inline constexpr std::uint32_t kBossRaceTickRateHz = 64;
inline constexpr std::uint64_t kBossRaceDefaultBossHp = 6000;
inline constexpr std::uint64_t kBossRaceDamagePerShotTick = 10;
inline constexpr std::uint32_t kBossRacePatternPeriodTicks = 45;
inline constexpr std::uint32_t kBossRaceMaxBulletsPerPlayer = 384;
inline constexpr std::size_t kBossRaceMaxPlayers = 2;
inline constexpr std::size_t kBossRaceMinPlayers = 2;
inline constexpr std::size_t kBossRacePatternCount = 10;

// 特殊机制（铁拳 / 黑色帷幕）开关用的 ruleset。默认关闭 → 现有模式逐字节不变；
// 只有显式把 ruleset_version 设成这个值才启用（计划 §2.4 / §2.7）。
// 同一开关也启用「玩家受击判定」（§2.4 第 2 条）：命中/擦弹/无敌帧/残机。
inline constexpr const char* kRulesetDanmakuMechanics = "tna-1.40-danmaku";

// 玩家受击判定常量（§2.4 第 2 条，单位 milli；只在 kRulesetDanmakuMechanics 下生效）。
inline constexpr std::int32_t kDanmakuHitRadiusMilli = 3000;    // r_hit ≈3px（判定点）
inline constexpr std::int32_t kDanmakuGrazeRadiusMilli = 20000;  // r_graze ≈20px
inline constexpr std::uint64_t kDanmakuInvulnerableTicks = 120;  // 命中/炸弹后的无敌帧
inline constexpr std::uint32_t kDanmakuDefaultLives = 3;
inline constexpr std::uint32_t kDanmakuDefaultBombs = 3;

// 判定事件类型（进快照 events[].type，与 judgment.hpp 的机制事件并列）。
inline constexpr const char* kPlayerHitEvent = "player_hit";
inline constexpr const char* kPlayerGrazeEvent = "player_graze";
inline constexpr const char* kPlayerDeadEvent = "player_dead";
inline constexpr const char* kBombUsedEvent = "bomb_used";
inline constexpr const char* kBossDamageEvent = "boss_damage";
inline constexpr const char* kSpellPhaseStartEvent = "spell_phase_start";
inline constexpr const char* kSpellPhaseEndEvent = "spell_phase_end";
inline constexpr const char* kSpellCaptureEvent = "spell_capture";

// 自机弹 vs Boss 判定（§2.4 第 1 条，只在 kRulesetDanmakuMechanics 下生效）。
inline constexpr std::int32_t kBossHitRadiusMilli = 40000;  // r_boss ≈40px（P4 进 boss_catalog）
inline constexpr std::uint64_t kBossDamageEventIntervalTicks = 8;  // boss_damage 聚合上报间隔
// 符卡阶段：3 段，阈值 100% / 66% / 33%（P4 搬 boss_catalog 数据表）。
inline constexpr std::uint32_t kSpellPhaseCount = 3;

// 特殊机制排期（P1 占位数值，P4 移到 boss_catalog 数据表，双端同值）。
// 触发规则：tick % interval == offset 且该机制当前 Idle → 起手；否则推进一 tick。
// 默认排期 = 铁拳 tick 300 / 900 / 1500…、帷幕 tick 600 / 1500 / 2400…（主控冻结）。
struct BossSpecialMechanicsSchedule {
    bool enabled = false;
    // 铁拳：每 600 tick 一次、相位 300；长度 200 px。
    std::uint32_t iron_fist_interval_ticks = 600;
    std::uint32_t iron_fist_offset_ticks = 300;
    std::int32_t iron_fist_length_milli = 200000;
    danmaku::IronFistParams iron_fist;
    // 黑色帷幕：每 900 tick 一次、相位 600。
    std::uint32_t curtain_interval_ticks = 900;
    std::uint32_t curtain_offset_ticks = 600;
    danmaku::BlackCurtainParams curtain;
};

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
    // 难度（P4 契约）：0/1/2/3 = easy/normal/hard/lunatic，默认 1=normal。
    // 只在 ruleset_version == kRulesetDanmakuMechanics 下按
    // danmaku::DifficultyScaleFor(difficulty) 缩放发射（速度/密度/间隔）；
    // 默认 ruleset 完全不读本字段（恒逐字节不变）。大厅/lifecycle 接线未覆盖，
    // 当前只能由对局配置直接设置（默认 1）。
    std::int32_t difficulty = 1;
    // 每玩家自机 loadout（空 = 默认自机行为）。
    std::vector<PlayerLoadoutSpec> loadouts;
    // 特殊机制排期（默认 disabled；ruleset_version == kRulesetDanmakuMechanics 时
    // 构造函数自动置 enabled）。
    BossSpecialMechanicsSchedule special_mechanics;
};

// ---------------------------------------------------------------------------
// §2.8 快照 wire 增补（race 与 coop 共用；全部字段带默认值 → 向后兼容）
//
// P1 只保证「字段存在且可解析」，数值先填现有状态或合理占位：
//   * effects 恒为空数组（铁拳/黑色帷幕状态机 P4 接线后才填）；
//   * hand 恒为空、deck_remaining/draw_cooldown_ticks 恒为 0（对局内抽卡 P3）；
//   * chains 由活跃弹幕聚合（chain_id = pattern_index，node_id = 0，rng_cursor = 0）；
//   * events 恒为空、event_cursor 恒为 0（判定事件 P3 才产生）。
// ---------------------------------------------------------------------------

// boss 移动方式（§2.6 wire 取值）。P1 只把既有整数三角波映射成 "sine"，
// move_to / move_by / angle_speed 留给 P4 的真实状态机。
struct BossMovementWire {
    std::string type = "static";  // static|move_to|move_by|sine|angle_speed
    std::int32_t phase_t = 0;     // 该移动方式内部的相位 tick
};

// 特殊机制状态（§2.4 铁拳 / 黑色帷幕）。state 取值：
//   iron_fist     : windup|strike|recover
//   black_curtain : fall|hold|sweep
struct BossEffectWire {
    std::string type;    // iron_fist | black_curtain
    std::string state;   // 见上
    std::int32_t t_remaining = 0;
    // iron_fist：起点 (x0,y0) + 单位方向 (dx,dy)（milli 定点）+ 长度。
    std::int32_t x0_milli = 0;
    std::int32_t y0_milli = 0;
    std::int32_t dx_milli = 0;
    std::int32_t dy_milli = 0;
    std::int32_t length_milli = 0;
    // black_curtain：sweep 期扫屏边缘线 y。
    std::int32_t edge_y_milli = 0;
};

// 链状态（§2.5）：客户端镜像重放校验用。
struct BossChainWire {
    std::uint32_t chain_id = 0;
    std::uint32_t pattern_index = 0;
    std::uint32_t node_id = 0;
    std::uint32_t generation = 0;
    std::uint32_t rng_cursor = 0;
};

// 手牌（§2.3 / §2.9 手牌栏）。
struct BossHandCardWire {
    std::string card_id;
    std::int32_t slot = -1;
    bool selected = false;
};

// 判定/机制事件行（§2.4.6）：tick + 类型 + 归属玩家 + 数值（伤害/阶段号等）。
struct BossEventWire {
    std::uint64_t tick = 0;
    std::string type;
    std::string player_id;
    std::int64_t value = 0;
};

// boss 本体视角：race = 本地玩家自己的 Boss 副本（广播负载共享一份，hp 取各
// 副本最大值；每玩家真实 HP 见 players[].boss_current_hp），coop = 共享 Boss。
struct BossStateWire {
    std::int32_t x_milli = 0;
    std::int32_t y_milli = 0;
    std::uint64_t hp = 0;
    std::uint64_t max_hp = 0;
    std::uint32_t phase = 0;
    BossMovementWire movement;
    std::vector<BossEffectWire> effects;
};

// P1 占位默认：判定系统落地前没有命/残机/炸弹消耗，故固定给「满状态」。
inline constexpr std::uint32_t kSnapshotDefaultLives = 1;
inline constexpr std::uint32_t kSnapshotDefaultBombs = 3;

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
    // §2.8 增补：所属链、分裂代数、帷幕 hold 期隐藏标记（判定照算）。
    std::uint32_t chain_id = 0;
    std::uint32_t generation = 0;
    bool hidden = false;
    // 阶段切换清屏标记（§2.4 第 3 条）：非 0 = 该 tick 被清除，客户端播消弹动画。
    std::uint64_t vanish_tick = 0;
};

struct BossRacePlayerSnapshot {
    std::string player_id;
    std::int32_t x_milli = 0;
    std::int32_t y_milli = 0;
    std::uint64_t boss_current_hp = 0;
    std::uint64_t damage_dealt = 0;
    bool connected = true;
    // §2.8 增补（客户端插值 §2.9 用 vx/vy，其余为判定系统预留）。
    std::int32_t vx_milli = 0;
    std::int32_t vy_milli = 0;
    std::int32_t facing_millideg = 0;
    std::uint32_t lives = kSnapshotDefaultLives;
    std::uint32_t bombs = kSnapshotDefaultBombs;
    std::uint64_t invulnerable_until_tick = 0;
    std::uint32_t graze = 0;
};

struct BossRaceSnapshot {
    std::uint64_t tick = 0;
    BossRaceState state = BossRaceState::Waiting;
    std::string winner_player_id;
    std::uint64_t winner_tick = 0;
    // P4 难度（客户端只消费：HUD 显示；服务端权威缩放，客户端不本地换算）。
    std::int32_t difficulty = 1;
    std::vector<BossRacePlayerSnapshot> players;
    std::vector<BossRaceBullet> bullets;
    std::string state_hash;
    // §2.8 增补。
    BossStateWire boss;
    std::vector<BossChainWire> chains;
    std::vector<BossHandCardWire> hand;
    std::uint32_t deck_remaining = 0;
    std::uint32_t draw_cooldown_ticks = 0;
    std::vector<BossEventWire> events;
    std::uint64_t event_cursor = 0;
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

    // 测试/调试注入：绕过 pattern 库直接放入一颗带完整运动学的子弹。
    // 默认 10 个 pattern 均不使用 bulletFunc31，链式分裂路径无法经产品
    // 发射自然触发，故开此口仅供测试（生产路径与 .dem 重放都不调用）。
    // 受 kBossRaceMaxBulletsPerPlayer 与玩家存在性约束；返回是否注入成功。
    bool InjectLiveBulletForTest(
        const std::string& owner_player_id,
        const danmaku::BulletMotion& motion,
        std::uint32_t generation = 0
    );

    // 测试/调试注入：放一颗自机弹（只在 kRulesetDanmakuMechanics 下参与判定）。
    // 生产路径由 SpawnMyBulletsForPlayer 按 player_loadout 表发射，不调用此口。
    bool InjectMyBulletForTest(
        const std::string& owner_player_id,
        std::int32_t x_milli,
        std::int32_t y_milli,
        std::int32_t vx_milli,
        std::int32_t vy_milli,
        std::uint64_t damage,
        std::int32_t penetrate = 0
    );

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
        // 本 tick 位移速度（§2.8 快照 vx/vy；客户端 §2.9 用它做速度外推插值）。
        std::int32_t vx_milli = 0;
        std::int32_t vy_milli = 0;
        std::uint64_t last_seq = 0;
        std::uint64_t boss_current_hp = 0;
        std::uint64_t damage_dealt = 0;
        bool connected = true;
        bool alive = true;
        // 受击判定状态（只在 kRulesetDanmakuMechanics 下被写；默认 ruleset 保持占位值）。
        std::uint32_t lives = kDanmakuDefaultLives;
        std::uint32_t bombs = kDanmakuDefaultBombs;
        std::uint32_t graze = 0;
        std::uint64_t invulnerable_until_tick = 0;
        // 自机弹：上次齐射 tick、待上报伤害（每 8 tick 聚合成一条 boss_damage）。
        std::uint64_t last_shot_tick = 0;
        std::uint64_t pending_boss_damage = 0;
        // 符卡阶段（0 起；跨阈值 +1，清屏 + spell_phase_start/end）。
        std::uint32_t spell_phase = 0;
        BattleInput last_input;
    };

    // 带完整运动学状态的实时子弹（真实弹幕）。
    struct LiveBullet {
        std::string bullet_id;
        std::string owner_player_id;
        std::string pattern_id;
        std::uint32_t pattern_index = 0;
        // 链 id（§2.8 快照 bullets[].chain_id）：默认/10 模式/合成路径 =
        // pattern_index（P1 占位语义，逐字节不变）；ChainNode 路径 = 触发的
        // 节点序号（chain_id，见 danmaku::ChainNodeEmission）。
        std::uint32_t chain_id = 0;
        std::uint32_t spawn_index = 0;
        // 链式分裂代数：0 = pattern 库直生（宿主），子子弹 = 宿主 +1，
        // 上限 danmaku::kChainSplitMaxGeneration（叶子代不再分裂）。
        // 仅服务端内部使用，不进快照/hash（客户端本地重推自行推导）。
        std::uint32_t generation = 0;
        // 擦弹去重：同一颗弹对同一玩家只计一次（§2.4 擦弹计数）。
        bool grazed = false;
        // 消弹标记：阶段切换清屏时置为该 tick，客户端播消弹动画；下一 tick 移除。
        std::uint64_t vanish_tick = 0;
        danmaku::BulletMotion motion;
    };

    // 自机弹（§2.4 第 1 条）：只在 kRulesetDanmakuMechanics 下产生与判定。
    // 位置/速度用居中 milli（与玩家同坐标系），Boss 命中盒 = 圆心 (kBossOrigin) + r_boss。
    struct MyBullet {
        std::string owner_player_id;
        std::int32_t x_milli = 0;
        std::int32_t y_milli = 0;
        std::int32_t vx_milli = 0;
        std::int32_t vy_milli = 0;
        std::uint64_t damage = 0;
        std::int32_t penetrate = 0;  // 剩余穿透次数（>0 命中后继续，递减）
    };

    void ResolveBossAndLoadouts();
    void StartIfReady();
    void ApplyPlayerTick(std::uint64_t tick);
    void SpawnBulletsForTick(std::uint64_t tick);
    // ChainNode 表路径（tna-1.40-danmaku + 库内 boss_id）：EmitChainBossTickDetailed
    // 的产出取代 10 模式轮转；同时填 tick_chains_（快照 chains[] 真实值）。
    void SpawnChainBulletsForTick(std::uint64_t tick);
    void SpawnRealBulletsForTick(std::uint64_t tick);
    void SpawnSyntheticBulletsForTick(std::uint64_t tick);  // mvp-boss-race-s0 兼容
    void AdvanceBullets(std::uint64_t tick);
    // 自机弹（§2.4 第 1 条）：发射 / 推进 / vs Boss 命中盒 / 符卡阶段。
    void SpawnMyBulletsForPlayer(PlayerState& player, std::uint64_t tick);
    void AdvanceMyBullets(std::uint64_t tick);
    void JudgeMyBullets(std::uint64_t tick);
    void ApplyBossDamage(PlayerState& player, std::uint64_t damage, std::uint64_t tick);
    void CheckSpellPhase(PlayerState& player, std::uint64_t tick);
    // 特殊机制（铁拳/黑色帷幕）状态推进；disabled 时是 no-op（现有模式零变化）。
    void AdvanceSpecialMechanics(std::uint64_t tick);
    // 玩家受击判定（§2.4 第 2 条）：boss 弹 vs 自机判定点/擦弹圈；disabled 时 no-op。
    void JudgePlayerContacts(std::uint64_t tick);
    void CheckRaceWinner(std::uint64_t tick);
    [[nodiscard]] const PlayerState* FirstOpponentOf(const std::string& player_id) const;
    [[nodiscard]] std::size_t EmittedBulletsFor(const std::string& player_id) const;
    // 外层波次调度周期：默认 ruleset / 难度 1 = config_.pattern_period_ticks
    // 原值；kRulesetDanmakuMechanics 且难度 != 1 时 llround×interval_mul。
    [[nodiscard]] std::uint32_t SchedulePeriodTicks() const;

    BossRaceConfig config_;
    std::uint64_t current_tick_ = 0;
    std::uint64_t next_bullet_id_ = 1;
    BossRaceState state_ = BossRaceState::Waiting;
    std::string winner_player_id_;
    std::uint64_t winner_tick_ = 0;
    std::map<std::string, PlayerState> players_;
    std::map<std::uint64_t, std::map<std::string, BattleInput>> pending_inputs_by_tick_;
    std::vector<LiveBullet> live_bullets_;
    std::vector<MyBullet> my_bullets_;
    // 解析后的 boss / 自机（构造期固定，运行期不可变 → 确定性）。
    const danmaku::BossCatalogEntry* boss_entry_ = nullptr;
    // ChainNode 表启用判定（构造期定；真 = SpawnChainBulletsForTick 取代 10 模式轮转）。
    bool chain_tables_active_ = false;
    // 难度缩放（构造期定；非 kRulesetDanmakuMechanics 或难度 1 = 恒等）。
    danmaku::DifficultyScale difficulty_scale_;
    std::map<std::string, PlayerLoadoutSpec> loadout_by_player_;
    std::map<std::string, const danmaku::PlayerShotSpec*> shot_by_player_;
    std::map<std::string, const danmaku::PlayerBombSpec*> bomb_by_player_;
    // F3 事件收集器（见 PushEvent 注释）。
    std::vector<CardCastEvent> events_;
    // 特殊机制状态（enabled 才推进；快照 effects[] 从这里读）。
    BossSpecialMechanicsSchedule mechanics_;
    danmaku::IronFistState iron_fist_;
    danmaku::BlackCurtainState black_curtain_;
    // 本 tick 产生的判定/机制事件（每 Tick 清空，快照 events[] 从这里读）。
    std::vector<BossEventWire> tick_events_;
    // 本 tick 触发的链节点（ChainNode 路径；每 Tick 清空，快照 chains[] 真实值）。
    // 非 ChainNode 路径恒空（chains[] 走 P1 按 pattern_index 聚合占位）。
    std::vector<BossChainWire> tick_chains_;
    std::uint64_t event_cursor_ = 0;
};

}  // namespace phk::battle
