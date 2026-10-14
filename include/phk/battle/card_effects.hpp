// 卡牌效果执行层（4 个 EffectKind 的纯函数移植）。
//
// 唯一真值来源：Gensoulkyo/runtime/core/card_effects.go（2026-10-14 逐行对照）：
//   - applyOpponentPatternLocked  → ApplyOpponentPattern   （mirror_lance）
//   - applyBulletPurgeLocked      → ApplyBulletPurge       （purge_bell）
//   - applyInvulnerableLocked     → ApplyInvulnerable      （aegis_ward）
//   - applyFinisherLocked         → ApplyFinisher          （finale_arc / last_arc）
//
// 设计：全部为纯函数——输入状态快照 + 激活参数，输出 bullet delta 列表与
// 状态变更（target / invulnerable_until / 事件 value），不触碰
// BattleSimulation 内部容器；tick 集成层负责把 delta 落到 std::vector 与
// 事件流。确定性选取使用 phk::battle::LobbyDeterministicUnit（与大厅同源）。
//
// 注意：boss 伤害由通用激活路径施加（applyCardRequestLocked），本层不重复。
//
// 版权：东方同人逆向成果，仅用于自研玩法。
#ifndef PHK_BATTLE_CARD_EFFECTS_HPP
#define PHK_BATTLE_CARD_EFFECTS_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace phk::battle {

// ---- 调参常量（card_effects.go 头部，逐项照抄） ----
constexpr int kCardOpponentPatternCount = 7;
constexpr double kCardOpponentPatternSpread = 3.14159265358979323846 / 3.0;  // π/3
constexpr double kCardOpponentPatternSpeed = 5.0;
constexpr double kCardOpponentPatternRadius = 4.5;
constexpr const char* kCardOpponentPatternColor = "violet";

// 场地常量（simulation.go:10-11）。
constexpr double kBossOriginX = 480.0;
constexpr double kBossOriginY = 112.0;

// 全场子弹上限（simulation.go:18 maxBullets）——addBulletLocked 的封顶。
constexpr std::size_t kMaxBullets = 384;

// ---- 输入快照 ----

// 一颗子弹的只读快照（对应 Go bulletState 的渲染相关字段）。
struct CardBulletSnapshot {
    std::string bullet_id;
    std::string pattern_id;
    std::string kind;
    std::int64_t spawn_tick = 0;
    double x = 0.0;
    double y = 0.0;
    double vx = 0.0;
    double vy = 0.0;
    double radius = 0.0;
    std::int32_t damage = 0;
    std::string color;
};

// 一名玩家的只读快照（效果执行只关心位置）。
struct CardPlayerSnapshot {
    std::string player_id;
    double x = 0.0;
    double y = 0.0;
};

// ---- 激活参数（由卡池目录 + 通用激活路径填充，见 service.go:405-420） ----
struct CardActivation {
    std::string card_id;
    std::string effect_kind;  // "opponent_pattern" | "bullet_purge" | "invulnerable" | "finisher"
    std::string pattern_id;   // 仅 opponent_pattern 使用；空则回退 card_id
    std::int64_t expires_tick = 0;  // 通用路径已算好 = tick + DurationTicks
    double purge_radius = 0.0;      // 仅 bullet_purge
    std::int64_t purge_count = 0;   // 仅 bullet_purge
};

// ---- 输出 ----

// 对应 Go BulletDelta（含 bulletDelta() 的四舍五入：x/y/radius 3 位、
// vx/vy 4 位，Go math.Round 与 C++ std::round 均为 half-away-from-zero）。
struct CardBulletDelta {
    std::string op;  // "spawn" | "despawn"
    std::string bullet_id;
    std::string pattern_id;
    std::string kind;
    std::int64_t tick = 0;
    double x = 0.0;
    double y = 0.0;
    double vx = 0.0;
    double vy = 0.0;
    double radius = 0.0;
    std::int32_t damage = 0;
    std::string color;
};

struct CardEffectResult {
    std::vector<CardBulletDelta> bullet_deltas;
    // opponent_pattern：命中的对手（opponentTargetID 的最低 user id）。
    std::string target_user_id;
    bool has_target = false;
    // invulnerable：新的 InvulnerableUntil（= activation.expires_tick）。
    std::int64_t invulnerable_until = 0;
    // MatchEvent.Value 语义 = 受影响的子弹计数（spawned / removed / cleared）。
    std::int64_t event_value = 0;
};

// ---- 4 个 EffectKind ----

// opponent_pattern（card_effects.go:43-72）：从 boss 原点朝对手当前位射
// kCardOpponentPatternCount 发扇形；±spread/2 等分；写入 target_user_id。
// bullet id 形如 "<match_id>_b%06d"（Go addBulletLocked），从
// next_bullet_seq 起递增；existing_bullet_count 参与 maxBullets 封顶判定。
CardEffectResult ApplyOpponentPattern(
    const std::vector<CardPlayerSnapshot>& players,
    const std::string& caster_id,
    const CardActivation& activation,
    std::int64_t tick,
    const std::string& match_id,
    std::uint64_t next_bullet_seq,
    std::size_t existing_bullet_count
);

// bullet_purge（card_effects.go:77-110）：无状态打分选取。全场子弹按
// bullet id 字典序给规范下标 p（下标在整场排序序列上，含半径外子弹），
// 用 LobbyDeterministicUnit(seed, tick, card_id, p) 打分，筛
// hypot(b-caster) <= purge_radius，按 (unit 升序, bullet id 升序) 取前
// purge_count 颗，逐颗发 despawn delta。
CardEffectResult ApplyBulletPurge(
    const std::vector<CardBulletSnapshot>& bullets,
    const CardPlayerSnapshot& caster,
    const CardActivation& activation,
    std::int64_t seed,
    std::int64_t tick
);

// invulnerable（card_effects.go:112-115）：InvulnerableUntil = expires_tick。
// 守卫语义（simulation.go:203）：tick > InvulnerableUntil 才受伤，即免疫
// 窗口恰好覆盖 [tick, expires_tick]，expires_tick 后一跳失效。
CardEffectResult ApplyInvulnerable(const CardActivation& activation, std::int64_t tick);

// finisher（card_effects.go:120-142）：清空全场子弹（按 bullet id 字典序
// 逐颗 despawn）。boss 伤害由通用激活路径施加，此处不重复。
CardEffectResult ApplyFinisher(const std::vector<CardBulletSnapshot>& bullets, std::int64_t tick);

// ---- 辅助（与 Go 同名函数语义一致） ----

// 按 bullet id 字典序（UTF-8 字节序）返回全部 id。
[[nodiscard]] std::vector<std::string> SortedBulletIds(const std::vector<CardBulletSnapshot>& bullets);

// opponentTargetID（card_effects.go:150+）：取除自己外的最低 user id；
// 无对手返回空串。
[[nodiscard]] std::string OpponentTargetId(
    const std::vector<std::string>& player_ids,
    const std::string& self_id
);

// Go round(value, precision)（service.go:7618）：half-away-from-zero。
[[nodiscard]] double RoundTo(double value, int precision);

}  // namespace phk::battle

#endif  // PHK_BATTLE_CARD_EFFECTS_HPP
