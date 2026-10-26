#include "phk/battle/boss_race.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "phk/battle/danmaku/chain_node.hpp"

namespace phk::battle {
namespace {

constexpr double kPi = 3.14159265358979323846;
// 场地常量 kArenaHalfWidthMilli / kArenaHalfHeightMilli 复用 simulation.hpp
// 的 phk::battle 定义（值相同）；此处不得重复定义，否则产生二义性。
constexpr std::int32_t kBossOriginXMilli = 0;
constexpr std::int32_t kBossOriginYMilli = -60000;

// 权威坐标单位：整数 milli（1/1000 px）。内部运动学用像素浮点，边界处 llround。
constexpr double kMilliPerPixel = 1000.0;

// 原版场地 640×960（半宽 320 / 半高 480）。danmaku 模块的反弹/边界常量按此场地，
// 因此发射/更新在「屏幕坐标」(0..640, 0..960) 下进行，快照再转回居中 milli。
constexpr double kScreenW = 640.0;
constexpr double kScreenH = 960.0;
constexpr double kScreenCenterX = 320.0;
constexpr double kScreenCenterY = 480.0;

// 真实弹幕 ruleset 版本；旧合成弹幕保留在 mvp-boss-race-s0 下。
inline constexpr const char* kRulesetRealDanmaku = "tna-1.40";

std::int32_t ToMilli(double pixels) {
    return static_cast<std::int32_t>(std::llround(pixels * kMilliPerPixel));
}

double ToPixels(std::int32_t milli) {
    return static_cast<double>(milli) / kMilliPerPixel;
}

std::uint32_t Fnv1a32(std::string_view text) {
    std::uint32_t hash = 2166136261u;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

std::uint32_t Mix32(std::uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

std::int32_t ClampMilli(std::int32_t value, std::int32_t lo, std::int32_t hi) {
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

// §2.8 wire：boss 移动方式名。P1 把既有整数三角波（SwayX/BobY）统一映射成
// "sine"；真实 move_to/move_by/angle_speed 状态机 P4 再引入。
const char* BossMovementWireType(danmaku::BossMovement kind) {
    switch (kind) {
        case danmaku::BossMovement::Static: return "static";
        case danmaku::BossMovement::SwayX:
        case danmaku::BossMovement::BobY: return "sine";
    }
    return "static";
}

// §2.8 wire：自机朝向（0=上，9000=右，18000=下，27000=左；0.01°）。
// 原版自机只做横向倾斜，故只按左右输入给朝向，其余保持朝上。
std::int32_t FacingMilliDegFromDirection(std::uint32_t direction_bits) {
    const bool left = (direction_bits & 0x8u) != 0;
    const bool right = (direction_bits & 0x2u) != 0;
    if (left && !right) return 27000;
    if (right && !left) return 9000;
    return 0;
}

// §2.8 wire：特殊机制阶段名（与 judgment.hpp 的枚举一一对应）。
const char* IronFistStateName(danmaku::IronFistPhase phase) {
    switch (phase) {
        case danmaku::IronFistPhase::Windup: return "windup";
        case danmaku::IronFistPhase::Strike: return "strike";
        case danmaku::IronFistPhase::Recover: return "recover";
        case danmaku::IronFistPhase::Idle: return "idle";
    }
    return "idle";
}

const char* BlackCurtainStateName(danmaku::BlackCurtainPhase phase) {
    switch (phase) {
        case danmaku::BlackCurtainPhase::Fall: return "fall";
        case danmaku::BlackCurtainPhase::Hold: return "hold";
        case danmaku::BlackCurtainPhase::Sweep: return "sweep";
        case danmaku::BlackCurtainPhase::Idle: return "idle";
    }
    return "idle";
}

}  // namespace

const std::vector<std::string>& BossRacePatternIds() {
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> out;
        for (const danmaku::PatternDef& def : danmaku::BossRacePatternLibrary()) {
            out.emplace_back(def.id);
        }
        return out;
    }();
    return ids;
}

std::uint32_t BossRaceDeterministicU32(
    std::uint64_t seed,
    std::uint64_t tick,
    std::uint32_t pattern_index,
    std::uint32_t spawn_index
) {
    const std::string material =
        std::to_string(seed) + ":" +
        std::to_string(tick) + ":" +
        std::to_string(pattern_index) + ":" +
        std::to_string(spawn_index);
    return Mix32(Fnv1a32(material) ^ static_cast<std::uint32_t>(seed));
}

double BossRaceDeterministicUnit(
    std::uint64_t seed,
    std::uint64_t tick,
    std::uint32_t pattern_index,
    std::uint32_t spawn_index
) {
    return static_cast<double>(BossRaceDeterministicU32(seed, tick, pattern_index, spawn_index) % 10000u) / 10000.0;
}

BossRaceSimulation::BossRaceSimulation(BossRaceConfig config) : config_(std::move(config)) {
    if (config_.mode_id.empty()) {
        config_.mode_id = kMvpBossRaceModeId;
    }
    if (config_.tick_rate_hz == 0) {
        config_.tick_rate_hz = kBossRaceTickRateHz;
    }
    if (config_.pattern_period_ticks == 0) {
        config_.pattern_period_ticks = kBossRacePatternPeriodTicks;
    }
    ResolveBossAndLoadouts();
    // 特殊机制：只有显式 ruleset 才启用（默认关闭 → 现有模式逐字节不变）。
    mechanics_ = config_.special_mechanics;
    if (config_.ruleset_version == kRulesetDanmakuMechanics) {
        mechanics_.enabled = true;
    }
    // ChainNode 表路径：tna-1.40-danmaku + 库内 boss_id 时取代 10 模式轮转
    // （默认 ruleset 与未知 boss_id 逐字节不变）。
    chain_tables_active_ = danmaku::ChainTablesActive(config_.ruleset_version, config_.boss_id);
    // 难度缩放：只在 kRulesetDanmakuMechanics 下生效；其余（含难度 1）恒等。
    // 注意 DifficultyScaleFor(1) 全 1.0，且倍率 1.0 的乘法/llround 均 IEEE 精确，
    // 故恒等缩放下所有发射参数逐字节不变。
    difficulty_scale_ = config_.ruleset_version == kRulesetDanmakuMechanics
        ? danmaku::DifficultyScaleFor(config_.difficulty)
        : danmaku::DifficultyScale{};
    if (config_.boss_max_hp == 0) {
        // boss 目录存在时以目录 HP 为准，否则回退默认（默认路径逐字节不变）。
        config_.boss_max_hp = boss_entry_ != nullptr ? boss_entry_->boss_max_hp : kBossRaceDefaultBossHp;
    }
}

// 解析 boss 目录项与每玩家自机 loadout。构造函数里调用一次，运行期只读。
void BossRaceSimulation::ResolveBossAndLoadouts() {
    boss_entry_ = danmaku::FindBossCatalogEntry(config_.boss_id);
    loadout_by_player_.clear();
    shot_by_player_.clear();
    bomb_by_player_.clear();
    for (const PlayerLoadoutSpec& loadout : config_.loadouts) {
        if (loadout.player_id.empty()) {
            continue;
        }
        loadout_by_player_[loadout.player_id] = loadout;
        if (!loadout.shot_type.empty()) {
            for (const danmaku::PlayerShotSpec& spec : danmaku::PlayerShotTable()) {
                if (loadout.shot_type == spec.name) {
                    shot_by_player_[loadout.player_id] = &spec;
                    break;
                }
            }
        }
        if (!loadout.bomb_type.empty()) {
            for (const danmaku::PlayerBombSpec& spec : danmaku::PlayerBombTable()) {
                if (loadout.bomb_type == spec.name) {
                    bomb_by_player_[loadout.player_id] = &spec;
                    break;
                }
            }
        }
    }
}

const BossRaceConfig& BossRaceSimulation::Config() const {
    return config_;
}

BossRaceState BossRaceSimulation::State() const {
    return state_;
}

std::uint64_t BossRaceSimulation::CurrentTick() const {
    return current_tick_;
}

std::size_t BossRaceSimulation::PlayerCount() const {
    return players_.size();
}

const std::string& BossRaceSimulation::WinnerPlayerId() const {
    return winner_player_id_;
}

// 外层波次调度周期（难度 interval 缩放的唯一入口；默认/难度 1 = 原值）。
std::uint32_t BossRaceSimulation::SchedulePeriodTicks() const {
    const std::uint32_t base = config_.pattern_period_ticks == 0 ? kBossRacePatternPeriodTicks : config_.pattern_period_ticks;
    return danmaku::ScalePeriod(base, difficulty_scale_.interval_mul);
}

std::size_t BossRaceSimulation::PatternIndexForTick(std::uint64_t tick) const {
    const std::uint32_t period = SchedulePeriodTicks();
    const std::size_t wave_index = static_cast<std::size_t>(tick / period);
    if (boss_entry_ != nullptr) {
        // boss 目录：按阶段组展开，循环整条 timeline。
        return danmaku::BossTimelinePatternIndexAt(*boss_entry_, wave_index);
    }
    // 默认路径：原 10 个真实模式轮转（逐字节保持现状）。
    return wave_index % kBossRacePatternCount;
}

std::string BossRaceSimulation::PatternIdForTick(std::uint64_t tick) const {
    return BossRacePatternIds()[PatternIndexForTick(tick)];
}

const danmaku::BossCatalogEntry* BossRaceSimulation::BossEntry() const {
    return boss_entry_;
}

const danmaku::PlayerShotSpec* BossRaceSimulation::PlayerShotSpecFor(const std::string& player_id) const {
    const auto it = shot_by_player_.find(player_id);
    return it == shot_by_player_.end() ? nullptr : it->second;
}

const danmaku::PlayerBombSpec* BossRaceSimulation::PlayerBombSpecFor(const std::string& player_id) const {
    const auto it = bomb_by_player_.find(player_id);
    return it == bomb_by_player_.end() ? nullptr : it->second;
}

std::string_view BossRaceSimulation::ShotTypeFor(const std::string& player_id) const {
    const auto it = loadout_by_player_.find(player_id);
    if (it == loadout_by_player_.end()) {
        return {};
    }
    return it->second.shot_type;
}

bool BossRaceSimulation::AddPlayer(const std::string& player_id, std::int32_t x_milli, std::int32_t y_milli) {
    if (player_id.empty() || players_.size() >= kBossRaceMaxPlayers) {
        return false;
    }
    if (players_.find(player_id) != players_.end()) {
        return false;
    }

    PlayerState player;
    player.player_id = player_id;
    player.x_milli = ClampMilli(x_milli, -kArenaHalfWidthMilli, kArenaHalfWidthMilli);
    player.y_milli = ClampMilli(y_milli, -kArenaHalfHeightMilli, kArenaHalfHeightMilli);
    player.boss_current_hp = config_.boss_max_hp;
    player.last_input.match_id = config_.match_id;
    player.last_input.player_id = player_id;
    if (mechanics_.enabled) {
        // 符卡阶段 0 在开局即生效（没有对应的 spell_phase_start 事件），
        // 故总数先记 1，之后每进一个新阶段再 +1（见 CheckSpellPhase）。
        player.spell_total = 1;
    }
    players_[player_id] = player;
    return true;
}

bool BossRaceSimulation::SetPlayerConnected(const std::string& player_id, bool connected) {
    const auto player_it = players_.find(player_id);
    if (player_it == players_.end()) {
        return false;
    }
    player_it->second.connected = connected;
    return true;
}

void BossRaceSimulation::PushEvent(CardCastEvent row) {
    if (events_.size() >= kCardCastEventRowLimit) {
        return;  // 宿主长期不上报时丢弃新行，防无界增长。
    }
    events_.push_back(std::move(row));
}

std::vector<CardCastEvent> BossRaceSimulation::TakeEvents(std::size_t limit) {
    if (limit == 0 || limit >= events_.size()) {
        std::vector<CardCastEvent> taken = std::move(events_);
        events_.clear();
        return taken;
    }
    std::vector<CardCastEvent> taken(
        std::make_move_iterator(events_.begin()),
        std::make_move_iterator(events_.begin() + static_cast<std::ptrdiff_t>(limit)));
    events_.erase(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(limit));
    return taken;
}

void BossRaceSimulation::RestoreEvents(const std::vector<CardCastEvent>& rows) {
    events_.insert(events_.begin(), rows.begin(), rows.end());
}

bool BossRaceSimulation::SubmitInput(const BattleInput& input) {
    if (state_ == BossRaceState::Finished) {
        return false;
    }
    if (!input.version.IsCompatible() || input.match_id != config_.match_id) {
        return false;
    }
    const auto player_it = players_.find(input.player_id);
    if (player_it == players_.end() || !player_it->second.connected) {
        return false;
    }
    if (input.seq == 0 || input.seq <= player_it->second.last_seq) {
        return false;
    }
    if (input.tick <= current_tick_) {
        return false;
    }
    if (input.tick - current_tick_ > 8) {
        return false;
    }
    if ((input.direction_bits & ~0x0fu) != 0) {
        return false;
    }

    player_it->second.last_seq = input.seq;
    pending_inputs_by_tick_[input.tick][input.player_id] = input;
    return true;
}

void BossRaceSimulation::StartIfReady() {
    if (state_ != BossRaceState::Waiting) {
        return;
    }
    if (players_.size() >= kBossRaceMinPlayers) {
        state_ = BossRaceState::Running;
    }
}

void BossRaceSimulation::ApplyPlayerTick(std::uint64_t tick) {
    const auto tick_it = pending_inputs_by_tick_.find(tick);
    for (auto& item : players_) {
        PlayerState& player = item.second;
        if (!player.connected) {
            continue;
        }

        BattleInput input = player.last_input;
        if (tick_it != pending_inputs_by_tick_.end()) {
            const auto input_it = tick_it->second.find(player.player_id);
            if (input_it != tick_it->second.end()) {
                input = input_it->second;
                player.last_input = input;
            }
        }

        // Movement: 8-direction bitmask (0=up,1=right,2=down,3=left), 3000 milli/tick.
        constexpr std::int32_t kMoveMilli = 3000;
        std::int32_t vx = 0;
        std::int32_t vy = 0;
        if ((input.direction_bits & 0x1u) != 0) { vy -= kMoveMilli; }
        if ((input.direction_bits & 0x2u) != 0) { vx += kMoveMilli; }
        if ((input.direction_bits & 0x4u) != 0) { vy += kMoveMilli; }
        if ((input.direction_bits & 0x8u) != 0) { vx -= kMoveMilli; }
        player.vx_milli = vx;
        player.vy_milli = vy;
        player.x_milli += vx;
        player.y_milli += vy;
        player.x_milli = ClampMilli(player.x_milli, -kArenaHalfWidthMilli, kArenaHalfWidthMilli);
        player.y_milli = ClampMilli(player.y_milli, -kArenaHalfHeightMilli, kArenaHalfHeightMilli);

        // 炸弹（只在 kRulesetDanmakuMechanics 下消耗残弹并给无敌帧；默认 ruleset
        // 保持现状：只按自机表加伤害，不消耗炸弹、不加无敌帧）。
        if (mechanics_.enabled && input.bomb && player.bombs > 0 && player.alive) {
            --player.bombs;
            player.invulnerable_until_tick = tick + kDanmakuInvulnerableTicks;
            BossEventWire event;
            event.tick = tick;
            event.type = kBombUsedEvent;
            event.player_id = player.player_id;
            tick_events_.push_back(std::move(event));
            // 真实路径下炸弹伤害直接结算（炸弹没有弹实体，一次性生效）。
            if (const danmaku::PlayerBombSpec* bomb = PlayerBombSpecFor(player.player_id);
                bomb != nullptr && bomb->damage > 0 && player.boss_current_hp > 0) {
                ApplyBossDamage(player, static_cast<std::uint64_t>(bomb->damage), tick);
            }
        }

        // Shooting damages this player's own Boss copy.
        // 默认（未配置自机 loadout）：每 tick 固定 kBossRaceDamagePerShotTick，
        // 与现状逐字节一致。配置了 shot_type/bomb_type 时改用真实自机表：
        //   * shot_type → danmaku::PlayerShotTable() 的 damage（主弹伤害）；
        //   * bomb_type → danmaku::PlayerBombTable() 的 damage（该 tick 触发的炸弹）。
        // kRulesetDanmakuMechanics 下改为「自机弹实体 vs Boss 命中盒」真实判定
        // （§2.4 第 1 条），这里不再叠加合成伤害。
        if (!mechanics_.enabled && player.boss_current_hp > 0 && player.alive) {
            std::uint64_t tick_damage = 0;
            if (input.shoot) {
                tick_damage += kBossRaceDamagePerShotTick;
                if (const danmaku::PlayerShotSpec* spec = PlayerShotSpecFor(player.player_id);
                    spec != nullptr && spec->damage >= 0) {
                    tick_damage = static_cast<std::uint64_t>(spec->damage);
                }
            }
            if (input.bomb) {
                if (const danmaku::PlayerBombSpec* bomb = PlayerBombSpecFor(player.player_id);
                    bomb != nullptr && bomb->damage >= 0) {
                    tick_damage += static_cast<std::uint64_t>(bomb->damage);
                }
            }
            if (tick_damage > 0) {
                const std::uint64_t damage = std::min(tick_damage, player.boss_current_hp);
                player.boss_current_hp -= damage;
                player.damage_dealt += damage;
                if (player.boss_current_hp == 0) {
                    // 默认 ruleset 不走 ApplyBossDamage → 击破奖励在这里补。
                    AwardBossDefeatBonus(player, tick);
                }
            }
        } else if (mechanics_.enabled && input.shoot && player.alive) {
            // 真实路径：按住射击 → 按 loadout 表的冷却节奏发射自机弹实体。
            SpawnMyBulletsForPlayer(player, tick);
        }
    }
    if (tick_it != pending_inputs_by_tick_.end()) {
        pending_inputs_by_tick_.erase(tick_it);
    }
}

std::size_t BossRaceSimulation::EmittedBulletsFor(const std::string& player_id) const {
    std::size_t count = 0;
    for (const LiveBullet& bullet : live_bullets_) {
        if (bullet.owner_player_id == player_id) {
            ++count;
        }
    }
    return count;
}

void BossRaceSimulation::SpawnBulletsForTick(std::uint64_t tick) {
    // 主路径 = 真实弹幕（tna-1.40）；旧合成弹幕仅在 mvp-boss-race-s0 下保留兼容。
    if (config_.ruleset_version == "mvp-boss-race-s0") {
        SpawnSyntheticBulletsForTick(tick);
    } else if (chain_tables_active_) {
        // ChainNode 表路径：EmitChainBossTickDetailed 取代 10 模式轮转
        // （启用条件见 ChainTablesActive：tna-1.40-danmaku + 库内 boss_id）。
        SpawnChainBulletsForTick(tick);
    } else {
        // 其余（含 kRulesetRealDanmaku）一律走真实弹幕。
        (void)kRulesetRealDanmaku;
        SpawnRealBulletsForTick(tick);
    }
}

// ChainNode 表路径：触发周期来自数据表（300/200 等，非 45 格网格），按 tick
// 逐节点展开；难度 interval 缩放作用于各节点 trigger period（chain_node.cpp）。
// 快照 chains[] 的权威来源 = tick_chains_（本 tick 触发的节点，广播级）。
void BossRaceSimulation::SpawnChainBulletsForTick(std::uint64_t tick) {
    // boss 移动：整数三角波发射点偏移（未配置 boss → (0,0)，与现状一致）。
    std::int32_t boss_offset_x = 0;
    std::int32_t boss_offset_y = 0;
    if (boss_entry_ != nullptr) {
        danmaku::BossEmitterOffsetMilli(boss_entry_->movement, tick, &boss_offset_x, &boss_offset_y);
    }

    // chains[] 为广播级视图（每玩家 Boss 副本的同构时间线）：取首个合格玩家的
    // 展开结构记录一次（节点触发与展开数与玩家无关；瞄准只改角度不改计数）。
    bool chains_recorded = false;

    for (auto& item : players_) {
        PlayerState& player = item.second;
        if (!player.connected || player.boss_current_hp == 0) {
            continue;
        }
        if (EmittedBulletsFor(player.player_id) >= kBossRaceMaxBulletsPerPlayer) {
            continue;
        }

        danmaku::EmitContext ctx;
        ctx.emitter_x = kScreenCenterX + ToPixels(kBossOriginXMilli + boss_offset_x);
        ctx.emitter_y = kScreenCenterY + ToPixels(kBossOriginYMilli + boss_offset_y);

        danmaku::MotionTargets targets;
        targets.has_my_ship = true;
        targets.my_ship_x = kScreenCenterX + ToPixels(player.x_milli);
        targets.my_ship_y = kScreenCenterY + ToPixels(player.y_milli);
        targets.has_enemy = true;
        targets.enemy_x = ctx.emitter_x;
        targets.enemy_y = ctx.emitter_y;

        const std::vector<danmaku::ChainNodeEmission> emissions =
            danmaku::EmitChainBossTickDetailed(config_.boss_id, tick, ctx, targets, difficulty_scale_);

        if (!chains_recorded && !emissions.empty()) {
            for (const danmaku::ChainNodeEmission& emission : emissions) {
                BossChainWire chain;
                chain.chain_id = emission.chain_id;              // 节点序号（合并时间线平坦序）
                chain.pattern_index = emission.table_index;      // 触发表索引
                chain.node_id = emission.node_id;                 // 节点在表内下标
                chain.generation = 0;                             // 子代代数（链式子代运行期激活属后续）
                chain.rng_cursor = static_cast<std::uint32_t>(emission.bullets.size());  // 该 tick 该链已展开节点数
                tick_chains_.push_back(chain);
            }
            chains_recorded = true;
        }

        // spawn_index = 该玩家该 tick 展开序（跨节点连续，保证 (pattern_index,
        // spawn_index) 对每颗弹唯一 → 运动学 RNG 输入不撞车）。
        std::uint32_t spawn_index = 0;
        for (const danmaku::ChainNodeEmission& emission : emissions) {
            for (const danmaku::BulletMotion& motion : emission.bullets) {
                if (EmittedBulletsFor(player.player_id) >= kBossRaceMaxBulletsPerPlayer) {
                    break;
                }
                LiveBullet live;
                live.bullet_id = "b" + std::to_string(next_bullet_id_++);
                live.owner_player_id = player.player_id;
                live.pattern_id = "chain:" + config_.boss_id;
                live.pattern_index = emission.table_index;
                live.chain_id = emission.chain_id;
                live.spawn_index = spawn_index++;
                live.motion = motion;
                live_bullets_.push_back(std::move(live));
            }
        }
    }
}

void BossRaceSimulation::SpawnRealBulletsForTick(std::uint64_t tick) {
    const std::uint32_t schedule = SchedulePeriodTicks();
    if (tick == 0 || (tick % schedule) != 0) {
        return;
    }
    const std::size_t pattern_index = PatternIndexForTick(tick);
    const danmaku::PatternDef& def = danmaku::BossRacePatternLibrary()[pattern_index];

    // boss 移动：整数三角波发射点偏移（未配置 boss → (0,0)，与现状一致）。
    std::int32_t boss_offset_x = 0;
    std::int32_t boss_offset_y = 0;
    if (boss_entry_ != nullptr) {
        danmaku::BossEmitterOffsetMilli(boss_entry_->movement, tick, &boss_offset_x, &boss_offset_y);
    }

    for (auto& item : players_) {
        PlayerState& player = item.second;
        if (!player.connected || player.boss_current_hp == 0) {
            continue;
        }
        if (EmittedBulletsFor(player.player_id) >= kBossRaceMaxBulletsPerPlayer) {
            continue;
        }

        danmaku::EmitContext ctx;
        ctx.emitter_x = kScreenCenterX + ToPixels(kBossOriginXMilli + boss_offset_x);
        ctx.emitter_y = kScreenCenterY + ToPixels(kBossOriginYMilli + boss_offset_y);

        danmaku::MotionTargets targets;
        targets.has_my_ship = true;
        targets.my_ship_x = kScreenCenterX + ToPixels(player.x_milli);
        targets.my_ship_y = kScreenCenterY + ToPixels(player.y_milli);
        targets.has_enemy = true;
        targets.enemy_x = ctx.emitter_x;
        targets.enemy_y = ctx.emitter_y;

        // 难度 interval 缩放：库内波次偏移按 45 格网格设计（pattern_timeline.cpp
        // 节奏模型），故把 tick 折算回 45 格再交给模式时间线；恒等缩放下
        // (tick/schedule)*45 == tick（schedule=45 且 tick 为其倍数），逐字节不变。
        const std::uint64_t pattern_tick =
            (tick / schedule) * static_cast<std::uint64_t>(kBossRacePatternPeriodTicks);
        const std::vector<danmaku::BulletMotion> emitted =
            danmaku::EmitPatternTick(def, pattern_tick, ctx, targets, difficulty_scale_);

        std::uint32_t spawn_index = 0;
        for (const danmaku::BulletMotion& motion : emitted) {
            if (EmittedBulletsFor(player.player_id) >= kBossRaceMaxBulletsPerPlayer) {
                break;
            }
            LiveBullet live;
            live.bullet_id = "b" + std::to_string(next_bullet_id_++);
            live.owner_player_id = player.player_id;
            live.pattern_id = std::string(def.id);
            live.pattern_index = static_cast<std::uint32_t>(pattern_index);
            live.chain_id = static_cast<std::uint32_t>(pattern_index);  // P1 占位语义（逐字节不变）
            live.spawn_index = spawn_index++;
            live.motion = motion;
            live_bullets_.push_back(std::move(live));
        }
    }
}

// mvp-boss-race-s0 兼容路径：保留旧 10 个合成模式（非主路径）。
void BossRaceSimulation::SpawnSyntheticBulletsForTick(std::uint64_t tick) {
    if (tick == 0 || (tick % config_.pattern_period_ticks) != 0) {
        return;
    }

    const std::size_t pattern_index = static_cast<std::size_t>((tick / config_.pattern_period_ticks) % kBossRacePatternCount);
    const std::string pattern_id = BossRacePatternIds()[pattern_index];

    for (auto& item : players_) {
        PlayerState& player = item.second;
        if (!player.connected || player.boss_current_hp == 0) {
            continue;
        }
        if (EmittedBulletsFor(player.player_id) >= kBossRaceMaxBulletsPerPlayer) {
            continue;
        }

        const double aim = std::atan2(
            static_cast<double>(player.y_milli - kBossOriginYMilli),
            static_cast<double>(player.x_milli - kBossOriginXMilli)
        );

        std::uint32_t synthetic_spawn_index = 0;
        auto push = [&](double angle_rad, std::int32_t speed_milli, std::uint32_t radius_milli, std::int32_t offset_x = 0, std::int32_t offset_y = 0) {
            if (EmittedBulletsFor(player.player_id) >= kBossRaceMaxBulletsPerPlayer) {
                return;
            }
            LiveBullet live;
            live.bullet_id = "b" + std::to_string(next_bullet_id_++);
            live.owner_player_id = player.player_id;
            live.pattern_id = pattern_id;
            live.pattern_index = static_cast<std::uint32_t>(pattern_index);
            live.chain_id = static_cast<std::uint32_t>(pattern_index);  // P1 占位语义（逐字节不变）
            live.spawn_index = synthetic_spawn_index++;
            danmaku::BulletMotion motion;
            motion.x = kScreenCenterX + ToPixels(kBossOriginXMilli) + ToPixels(offset_x);
            motion.y = kScreenCenterY + ToPixels(kBossOriginYMilli) + ToPixels(offset_y);
            motion.angle = angle_rad * (180.0 / kPi);
            motion.angle_init = -1.0;
            motion.speed = ToPixels(speed_milli);
            motion.spd_init2 = motion.speed;
            motion.bullet_func_no = 0;
            motion.hit_dis = ToPixels(static_cast<std::int32_t>(radius_milli));
            motion.IntegrateVelocity();
            live.motion = motion;
            live_bullets_.push_back(std::move(live));
        };

        switch (pattern_index) {
            case 0: {  // ring
                for (int i = 0; i < 16; ++i) {
                    push(2.0 * kPi * static_cast<double>(i) / 16.0, 3000, 4000);
                }
                break;
            }
            case 1: {  // gap_ring
                for (int i = 0; i < 16; ++i) {
                    if (i % 5 == 0) {
                        continue;
                    }
                    push(2.0 * kPi * static_cast<double>(i) / 16.0, 3000, 4000);
                }
                break;
            }
            case 2: {  // n_way
                for (int i = -2; i <= 2; ++i) {
                    push(aim + static_cast<double>(i) * (kPi / 12.0), 3600, 4200);
                }
                break;
            }
            case 3: {  // aimed
                push(aim, 4400, 4000);
                break;
            }
            case 4: {  // seeded_arc
                const double offset = (BossRaceDeterministicUnit(config_.match_seed, tick, 4, 0) - 0.5) * (kPi / 2.0);
                for (int i = 0; i < 8; ++i) {
                    push(aim + offset + (static_cast<double>(i) - 3.5) * (kPi / 16.0), 3200, 4000);
                }
                break;
            }
            case 5: {  // spiral
                const double base = static_cast<double>(tick % 360) * (kPi / 180.0);
                for (int i = 0; i < 4; ++i) {
                    push(base + static_cast<double>(i) * (kPi / 2.0), 3400, 4200);
                }
                break;
            }
            case 6: {  // laser_curtain
                for (int i = -1; i <= 1; ++i) {
                    push(kPi / 2.0, 1500, 9000, static_cast<std::int32_t>(i) * 40000, 0);
                }
                break;
            }
            case 7: {  // homing (straight approximation at fire time)
                push(aim - 0.12, 2600, 4200);
                push(aim + 0.12, 2600, 4200);
                break;
            }
            case 8: {  // sine_stream
                const double wobble = std::sin(static_cast<double>(tick) * 0.2) * 0.4;
                push(aim + wobble, 3400, 4000);
                break;
            }
            default: {  // blossom
                for (int i = 0; i < 12; ++i) {
                    push(2.0 * kPi * static_cast<double>(i) / 12.0, 2600, 4000);
                }
                for (int i = 0; i < 6; ++i) {
                    push(2.0 * kPi * static_cast<double>(i) / 6.0 + (kPi / 6.0), 4200, 3600);
                }
                break;
            }
        }
    }
}

// 本 tick 内一颗宿主的分裂请求（下标而非指针/引用：第二趟 push_back 会
// 使 live_bullets_ 重新分配，下标语义在只追加不删除的前提下保持稳定）。
struct AdvanceSplitRequest {
    std::size_t index = 0;
    std::string owner_player_id;
    std::uint32_t pattern_index = 0;
    std::uint32_t spawn_index = 0;
    std::uint32_t generation = 0;
};

void BossRaceSimulation::AdvanceBullets(std::uint64_t tick) {
    // 第一趟：运动学更新 + 收集 H 族（bulletFunc31）分裂请求。
    // 不能边遍历边 push 子子弹：vector 增长会失效遍历迭代器，且子子弹必须
    // 晚于全部宿主更新后入场（与主发射一致：子子弹从下一帧才开始运动）。
    // 分裂由服务端在本函数内权威完成；参数全部由
    // DeterministicRng(match_seed, tick, pattern_index, host_spawn_index)
    // 推导（见 danmaku/chain_split.hpp），客户端镜像同算法即可重放。
    std::vector<AdvanceSplitRequest> requests;
    std::map<std::string, std::uint32_t> host_budget;  // 每玩家本 tick 分裂宿主预算

    // 阶段切换清屏的子弹：上一 tick 已广播 vanish_tick，本 tick 正式移除。
    live_bullets_.erase(
        std::remove_if(
            live_bullets_.begin(),
            live_bullets_.end(),
            [tick](const LiveBullet& live) { return live.vanish_tick != 0 && live.vanish_tick < tick; }
        ),
        live_bullets_.end()
    );

    for (std::size_t i = 0; i < live_bullets_.size(); ++i) {
        LiveBullet& live = live_bullets_[i];
        if (live.vanish_tick != 0) {
            continue;  // 消弹中的子弹不再运动/分裂
        }
        danmaku::MotionTargets targets;
        const auto owner_it = players_.find(live.owner_player_id);
        if (owner_it != players_.end()) {
            targets.has_my_ship = true;
            targets.my_ship_x = kScreenCenterX + ToPixels(owner_it->second.x_milli);
            targets.my_ship_y = kScreenCenterY + ToPixels(owner_it->second.y_milli);
        }
        // boss 移动：追踪弹的「敌军」目标随 boss 三角波移动（未配置 boss → 原点）。
        std::int32_t boss_offset_x = 0;
        std::int32_t boss_offset_y = 0;
        if (boss_entry_ != nullptr) {
            danmaku::BossEmitterOffsetMilli(boss_entry_->movement, tick, &boss_offset_x, &boss_offset_y);
        }
        targets.has_enemy = true;
        targets.enemy_x = kScreenCenterX + ToPixels(kBossOriginXMilli + boss_offset_x);
        targets.enemy_y = kScreenCenterY + ToPixels(kBossOriginYMilli + boss_offset_y);

        // 确定性 RNG：输入 (match_seed, tick, pattern_index, spawn_index)。
        danmaku::DeterministicRng rng(config_.match_seed, tick, live.pattern_index, live.spawn_index);
        danmaku::UpdateBulletMotion(live.motion, targets, rng);
        live.motion.x += live.motion.vx;
        live.motion.y += live.motion.vy;
        live.motion.t += 1.0;

        // H 族链式分裂请求：func31 本帧置了 p2 标记、代数未达上限、该玩家
        // 本 tick 宿主预算未用尽 → 记录并消费标记。ChainSplitRequested 只认
        // func_no==31（Func42/Func55 同样写 p2，但不是分裂族，不会误触发）。
        // 叶子代（generation 达上限）不消费标记：Func31 仅在 t==p1 单帧置位，
        // p2 残留无行为影响，故无需清零。
        // p2 是「单帧事件标志」（Func31 仅在 t==p1 置位一次）：本帧一律
        // 消费——被服务（进 requests）或丢弃（代数到顶/预算用尽）都在本帧
        // 决定，绝不推迟到后续帧，否则残留标记会撞上「每 tick 预算重置」把
        // 一次事件摊成多次。
        if (danmaku::ChainSplitRequested(live.motion)) {
            live.motion.p2 = 0;
            if (live.generation < danmaku::kChainSplitMaxGeneration) {
                std::uint32_t& budget = host_budget[live.owner_player_id];
                if (budget < danmaku::kChainSplitHostsPerPlayerPerTick) {
                    ++budget;
                    AdvanceSplitRequest request;
                    request.index = i;
                    request.owner_player_id = live.owner_player_id;
                    request.pattern_index = live.pattern_index;
                    request.spawn_index = live.spawn_index;
                    request.generation = live.generation;
                    requests.push_back(std::move(request));
                }
            }
        }
    }

    // 第二趟：按请求顺序生成子子弹。请求顺序 = 第一趟遍历顺序（live_bullets_
    // 稳定顺序），因此 bullet_id 分配顺序、快照子弹顺序两端一致。
    // 钳制链：每玩家总量 kBossRaceMaxBulletsPerPlayer 决定 max_children
    // （超限时后请求的宿主少生或不生，截断顺序确定）。
    for (const AdvanceSplitRequest& request : requests) {
        const LiveBullet& host = live_bullets_[request.index];
        const std::size_t room = kBossRaceMaxBulletsPerPlayer - EmittedBulletsFor(request.owner_player_id);
        const int max_children = static_cast<int>(
            std::min<std::size_t>(room, static_cast<std::size_t>(danmaku::kChainSplitChildrenPerHost)));
        const std::vector<danmaku::ChainSplitChild> children = danmaku::ComputeChainSplitChildren(
            host.motion,
            config_.match_seed,
            tick,
            request.pattern_index,
            request.spawn_index,
            max_children
        );
        for (const danmaku::ChainSplitChild& child : children) {
            if (EmittedBulletsFor(request.owner_player_id) >= kBossRaceMaxBulletsPerPlayer) {
                break;
            }
            LiveBullet live;
            live.bullet_id = "b" + std::to_string(next_bullet_id_++);
            live.owner_player_id = request.owner_player_id;
            live.pattern_id = host.pattern_id;
            live.pattern_index = request.pattern_index;
            live.chain_id = request.pattern_index;  // P1 占位语义（逐字节不变）
            live.spawn_index = child.spawn_index;
            live.generation = request.generation + 1;
            live.motion = danmaku::MakeChainSplitChildMotion(host.motion, child);
            live_bullets_.push_back(std::move(live));
        }
    }

    // 第三趟：出界移除（与链式分裂落地前完全相同的谓词）。
    live_bullets_.erase(
        std::remove_if(
            live_bullets_.begin(),
            live_bullets_.end(),
            [](const LiveBullet& live) {
                return live.motion.x < -20.0 || live.motion.x > kScreenW + 20.0 ||
                    live.motion.y < -20.0 || live.motion.y > kScreenH + 20.0;
            }
        ),
        live_bullets_.end()
    );
}

// ---------------------------------------------------------------------------
// 自机弹 vs Boss（§2.4 第 1 条，仅 kRulesetDanmakuMechanics）
// ---------------------------------------------------------------------------
void BossRaceSimulation::SpawnMyBulletsForPlayer(PlayerState& player, std::uint64_t tick) {
    const danmaku::PlayerShotSpec* spec = PlayerShotSpecFor(player.player_id);
    const std::uint64_t cooldown = (spec != nullptr && spec->cooldown_frames > 0)
        ? static_cast<std::uint64_t>(spec->cooldown_frames)
        : 8;  // 无 loadout 时的默认齐射间隔（帧→tick 数值原样，不做缩放）
    if (player.last_shot_tick != 0 && tick - player.last_shot_tick < cooldown) {
        return;
    }
    player.last_shot_tick = tick;

    const int volley = (spec != nullptr && spec->bullets_per_volley > 0) ? spec->bullets_per_volley : 1;
    for (int i = 0; i < volley; ++i) {
        if (my_bullets_.size() >= kBossRaceMaxBulletsPerPlayer * kBossRaceMaxPlayers) {
            break;
        }
        double angle_deg = 90.0;  // 90° = 正上方（原版约定）
        double speed = (spec != nullptr && spec->speed > 0.0) ? spec->speed : 12.0;
        double offset_x = 0.0;
        double offset_y = 0.0;
        int damage = (spec != nullptr && spec->damage > 0) ? spec->damage : 1;
        if (spec != nullptr && spec->bullets != nullptr && i < spec->bullet_count) {
            angle_deg = spec->bullets[i].angle_deg;
            if (spec->bullets[i].speed > 0.0) speed = spec->bullets[i].speed;
            offset_x = spec->bullets[i].dx;
            offset_y = spec->bullets[i].dy;
        } else if (spec != nullptr && volley > 1) {
            // 没有逐弹表时，把齐射角度均分到 [angle_min, angle_max]。
            const double span = spec->angle_max_deg - spec->angle_min_deg;
            angle_deg = spec->angle_min_deg + span * static_cast<double>(i) / static_cast<double>(volley - 1);
        }

        MyBullet bullet;
        bullet.owner_player_id = player.player_id;
        bullet.x_milli = player.x_milli + static_cast<std::int32_t>(std::llround(offset_x * kMilliPerPixel));
        bullet.y_milli = player.y_milli + static_cast<std::int32_t>(std::llround(offset_y * kMilliPerPixel));
        const double rad = angle_deg * (kPi / 180.0);
        bullet.vx_milli = static_cast<std::int32_t>(std::llround(speed * kMilliPerPixel * std::cos(rad)));
        bullet.vy_milli = -static_cast<std::int32_t>(std::llround(speed * kMilliPerPixel * std::sin(rad)));
        bullet.damage = static_cast<std::uint64_t>(damage);
        bullet.penetrate = 0;  // 自机射击表没有穿透列 → 默认单段（穿透留给测试注入验证）
        my_bullets_.push_back(std::move(bullet));
    }
}

void BossRaceSimulation::AdvanceMyBullets(std::uint64_t tick) {
    if (!mechanics_.enabled) {
        return;
    }
    (void)tick;
    for (MyBullet& bullet : my_bullets_) {
        bullet.x_milli += bullet.vx_milli;
        bullet.y_milli += bullet.vy_milli;
    }
    my_bullets_.erase(
        std::remove_if(
            my_bullets_.begin(),
            my_bullets_.end(),
            [](const MyBullet& bullet) {
                return bullet.x_milli < -kArenaHalfWidthMilli || bullet.x_milli > kArenaHalfWidthMilli ||
                    bullet.y_milli < -kArenaHalfHeightMilli || bullet.y_milli > kArenaHalfHeightMilli;
            }
        ),
        my_bullets_.end()
    );
}

// ---------------------------------------------------------------------------
// 计分（TNA `GameData` 形状；数值见 danmaku/score_table.hpp 的冻结占位表）
// ---------------------------------------------------------------------------
// 所有加分都经这里 → 保证「本局 score_gain 增量之和 == players[].score」。
// 事件 value 是 int64（wire 既有类型），单次增量远小于 int64 上限。
void BossRaceSimulation::AddScore(PlayerState& player, std::uint64_t delta, std::uint64_t tick) {
    if (delta == 0) {
        return;
    }
    player.score += delta;
    BossEventWire gain;
    gain.tick = tick;
    gain.type = kScoreGainEvent;
    gain.player_id = player.player_id;
    gain.value = static_cast<std::int64_t>(delta);
    tick_events_.push_back(std::move(gain));
}

// 击破奖励 = 原版「实体消灭」分（实测 @0x565724 / 0x56571c）：基础 10000，
// `SelectChara ∈ {15,16}` 时 15000（映射未实测，见 score_table.hpp）。
// 每玩家打掉自己那份 Boss 副本时各算一次（两种 ruleset 都算）。
void BossRaceSimulation::AwardBossDefeatBonus(PlayerState& player, std::uint64_t tick) {
    AddScore(player, danmaku::DestroyScoreForCharacter(ShotTypeFor(player.player_id)), tick);
}

void BossRaceSimulation::ApplyBossDamage(PlayerState& player, std::uint64_t damage, std::uint64_t tick) {
    if (damage == 0 || player.boss_current_hp == 0) {
        return;
    }
    const std::uint64_t applied = std::min(damage, player.boss_current_hp);
    player.boss_current_hp -= applied;
    player.damage_dealt += applied;
    player.pending_boss_damage += applied;
    CheckSpellPhase(player, tick);
    if (player.boss_current_hp == 0) {
        AwardBossDefeatBonus(player, tick);
        // 击破时若符卡阶段仍 active → 捕获 bonus（§2.4 第 3 条）。
        BossEventWire capture;
        capture.tick = tick;
        capture.type = kSpellCaptureEvent;
        capture.player_id = player.player_id;
        capture.value = static_cast<std::int64_t>(player.spell_phase);
        tick_events_.push_back(std::move(capture));
        // 最后一阶段只可能由击破收尾（没有下一个阈值 → 不会发 spell_phase_end），
        // 故在这里补一次「未被弹的收尾阶段」捕获分。已经发过 phase_end 的阶段
        // 早已加过分，且 phase_hit_taken 在每次 phase_start 复位 → 同一阶段
        // 只会被计一次（不双计）。
        if (!player.phase_hit_taken) {
            ++player.spell_get;
            AddScore(
                player,
                danmaku::SpellCapturePoints(
                    danmaku::ScoreRulesFor(),
                    danmaku::BossNumberOf(config_.boss_id),
                    player.graze),
                tick);
        }
    }
}

// 符卡阶段：3 段，阈值 100% / 66% / 33%。跨阈值 → phase+1、清屏、阶段事件。
void BossRaceSimulation::CheckSpellPhase(PlayerState& player, std::uint64_t tick) {
    const std::uint64_t max_hp = config_.boss_max_hp;
    if (max_hp == 0) {
        return;
    }
    while (player.spell_phase + 1 < kSpellPhaseCount) {
        const std::uint64_t remaining_phases = kSpellPhaseCount - 1 - player.spell_phase;
        if (player.boss_current_hp * kSpellPhaseCount > max_hp * remaining_phases) {
            break;
        }
        BossEventWire phase_end;
        phase_end.tick = tick;
        phase_end.type = kSpellPhaseEndEvent;
        phase_end.player_id = player.player_id;
        phase_end.value = static_cast<std::int64_t>(player.spell_phase);
        tick_events_.push_back(std::move(phase_end));

        // 符卡获取：整段没被打过才给分（TNA 的 spell bonus 语义）。
        if (!player.phase_hit_taken) {
            ++player.spell_get;
            AddScore(
                player,
                danmaku::SpellCapturePoints(
                    danmaku::ScoreRulesFor(),
                    danmaku::BossNumberOf(config_.boss_id),
                    player.graze),
                tick);
        }

        player.spell_phase += 1;
        ++player.spell_total;
        player.phase_hit_taken = false;  // 新阶段重新计「是否被打」

        // 清屏：该玩家副本的活跃弹幕标 vanish_tick（客户端播消弹；下一 tick 移除）。
        std::uint64_t cleared = 0;
        for (LiveBullet& live : live_bullets_) {
            if (live.owner_player_id == player.player_id && live.vanish_tick == 0) {
                live.vanish_tick = tick;
                ++cleared;
            }
        }
        // 消弹分（实测 @0x568eb0）：每颗 +100。
        if (cleared > 0) {
            AddScore(player, cleared * danmaku::ScoreRulesFor().bullet_clear_points, tick);
        }
        BossEventWire phase_start;
        phase_start.tick = tick;
        phase_start.type = kSpellPhaseStartEvent;
        phase_start.player_id = player.player_id;
        phase_start.value = static_cast<std::int64_t>(player.spell_phase);
        tick_events_.push_back(std::move(phase_start));
    }
}

void BossRaceSimulation::JudgeMyBullets(std::uint64_t tick) {
    if (!mechanics_.enabled) {
        return;
    }
    std::vector<std::size_t> consumed;
    for (std::size_t i = 0; i < my_bullets_.size(); ++i) {
        MyBullet& bullet = my_bullets_[i];
        const auto player_it = players_.find(bullet.owner_player_id);
        if (player_it == players_.end() || player_it->second.boss_current_hp == 0) {
            consumed.push_back(i);
            continue;
        }
        PlayerState& player = player_it->second;
        if (!danmaku::PointInCircle(
                bullet.x_milli, bullet.y_milli,
                kBossOriginXMilli, kBossOriginYMilli,
                kBossHitRadiusMilli)) {
            continue;
        }
        ApplyBossDamage(player, bullet.damage, tick);
        // 命中分（实测 @0x5662b0）：每次命中 +30。
        AddScore(player, danmaku::ScoreRulesFor().hit_points, tick);
        if (bullet.penetrate > 0) {
            --bullet.penetrate;  // 穿透弹继续前进，下一 tick 可再判定一段
        } else {
            consumed.push_back(i);
        }
    }
    for (auto it = consumed.rbegin(); it != consumed.rend(); ++it) {
        my_bullets_.erase(my_bullets_.begin() + static_cast<std::ptrdiff_t>(*it));
    }
}

bool BossRaceSimulation::InjectMyBulletForTest(
    const std::string& owner_player_id,
    std::int32_t x_milli,
    std::int32_t y_milli,
    std::int32_t vx_milli,
    std::int32_t vy_milli,
    std::uint64_t damage,
    std::int32_t penetrate
) {
    if (players_.find(owner_player_id) == players_.end()) {
        return false;
    }
    MyBullet bullet;
    bullet.owner_player_id = owner_player_id;
    bullet.x_milli = x_milli;
    bullet.y_milli = y_milli;
    bullet.vx_milli = vx_milli;
    bullet.vy_milli = vy_milli;
    bullet.damage = damage;
    bullet.penetrate = penetrate;
    my_bullets_.push_back(std::move(bullet));
    return true;
}

bool BossRaceSimulation::InjectLiveBulletForTest(
    const std::string& owner_player_id,
    const danmaku::BulletMotion& motion,
    std::uint32_t generation
) {    // 测试/调试专用注入（产品路径与 .dem 重放均不调用）：默认 pattern 库
    // 没有 func31 波次，链式分裂只能经此入口在测试中触发。
    const auto it = players_.find(owner_player_id);
    if (it == players_.end()) {
        return false;
    }
    if (EmittedBulletsFor(owner_player_id) >= kBossRaceMaxBulletsPerPlayer) {
        return false;
    }
    LiveBullet live;
    live.bullet_id = "b" + std::to_string(next_bullet_id_++);
    live.owner_player_id = owner_player_id;
    live.pattern_id = "injected";
    live.pattern_index = 0;
    live.spawn_index = 0;
    live.generation = generation;
    live.motion = motion;
    live_bullets_.push_back(std::move(live));
    return true;
}

void BossRaceSimulation::AdvanceSpecialMechanics(std::uint64_t tick) {
    if (!mechanics_.enabled) {
        return;
    }

    // 铁拳：到相位且当前 Idle → 起手（朝 map 序第一个玩家，确定性）；否则推进。
    const bool iron_fist_due = mechanics_.iron_fist_interval_ticks != 0 &&
        (tick % mechanics_.iron_fist_interval_ticks) == mechanics_.iron_fist_offset_ticks;
    if (iron_fist_.phase == danmaku::IronFistPhase::Idle) {
        if (iron_fist_due) {
            std::int32_t dir_x = 1000;
            std::int32_t dir_y = 0;
            if (!players_.empty()) {
                const PlayerState& target = players_.begin()->second;
                const double vx = static_cast<double>(target.x_milli - kBossOriginXMilli);
                const double vy = static_cast<double>(target.y_milli - kBossOriginYMilli);
                const double len = std::sqrt(vx * vx + vy * vy);
                if (len > 0.0) {
                    dir_x = static_cast<std::int32_t>(std::llround(vx / len * 1000.0));
                    dir_y = static_cast<std::int32_t>(std::llround(vy / len * 1000.0));
                }
            }
            danmaku::StartIronFist(
                &iron_fist_,
                danmaku::PointMilli{kBossOriginXMilli, kBossOriginYMilli},
                dir_x,
                dir_y,
                mechanics_.iron_fist_length_milli,
                mechanics_.iron_fist
            );
            BossEventWire wire;
            wire.tick = tick;
            wire.type = danmaku::kIronFistWindupEvent;
            tick_events_.push_back(std::move(wire));
        }
    } else {
        for (const danmaku::JudgeEvent& event : danmaku::AdvanceIronFist(&iron_fist_, tick, mechanics_.iron_fist)) {
            BossEventWire wire;
            wire.tick = event.tick;
            wire.type = event.type;
            tick_events_.push_back(std::move(wire));
        }
    }

    // 黑色帷幕：同理。
    const bool curtain_due = mechanics_.curtain_interval_ticks != 0 &&
        (tick % mechanics_.curtain_interval_ticks) == mechanics_.curtain_offset_ticks;
    if (black_curtain_.phase == danmaku::BlackCurtainPhase::Idle) {
        if (curtain_due) {
            danmaku::StartBlackCurtain(&black_curtain_, mechanics_.curtain);
            BossEventWire wire;
            wire.tick = tick;
            wire.type = danmaku::kBlackCurtainFallEvent;
            tick_events_.push_back(std::move(wire));
        }
    } else {
        for (const danmaku::JudgeEvent& event : danmaku::AdvanceBlackCurtain(&black_curtain_, tick, mechanics_.curtain)) {
            BossEventWire wire;
            wire.tick = event.tick;
            wire.type = event.type;
            tick_events_.push_back(std::move(wire));
        }
    }
}

// 玩家受击判定（§2.4 第 2 条）。只在 kRulesetDanmakuMechanics 下生效：
//   * 命中（dist <= 弹半径 + r_hit）→ lives-1、无敌帧 120 tick、事件 player_hit
//     （lives 归零再发 player_dead 并置 alive=false）；
//   * 擦弹（dist <= 弹半径 + r_graze 且未命中）→ graze+1、事件 player_graze，
//     同一颗弹对同一玩家只计一次（LiveBullet::grazed 去重）；
//   * 无敌帧内两者都不计。
// 子弹按 owner_player_id 归属到「自己那份 Boss 副本」，故只判定该玩家自己的弹。
void BossRaceSimulation::JudgePlayerContacts(std::uint64_t tick) {
    if (!mechanics_.enabled) {
        return;
    }
    for (auto& item : players_) {
        PlayerState& player = item.second;
        if (!player.connected || !player.alive) {
            continue;
        }
        const bool invulnerable = tick <= player.invulnerable_until_tick;
        for (LiveBullet& live : live_bullets_) {
            if (live.owner_player_id != player.player_id || live.vanish_tick != 0) {
                continue;
            }
            const std::int32_t bullet_x = ToMilli(live.motion.x - kScreenCenterX);
            const std::int32_t bullet_y = ToMilli(live.motion.y - kScreenCenterY);
            const std::int32_t bullet_radius = ToMilli(live.motion.hit_dis);
            const std::int64_t distance_squared = danmaku::DistanceSquaredMilli(
                danmaku::PointMilli{bullet_x, bullet_y},
                danmaku::PointMilli{player.x_milli, player.y_milli}
            );
            if (invulnerable) {
                continue;
            }
            const std::int64_t hit_radius = bullet_radius + kDanmakuHitRadiusMilli;
            if (distance_squared <= hit_radius * hit_radius) {
                if (player.lives > 0) {
                    --player.lives;
                }
                // 计分：受击次数 + 本阶段符卡作废（该阶段不给捕获分）。
                ++player.hit_count;
                player.phase_hit_taken = true;
                player.invulnerable_until_tick = tick + kDanmakuInvulnerableTicks;
                BossEventWire hit;
                hit.tick = tick;
                hit.type = kPlayerHitEvent;
                hit.player_id = player.player_id;
                tick_events_.push_back(std::move(hit));
                if (player.lives == 0) {
                    player.alive = false;
                    BossEventWire dead;
                    dead.tick = tick;
                    dead.type = kPlayerDeadEvent;
                    dead.player_id = player.player_id;
                    tick_events_.push_back(std::move(dead));
                }
                break;  // 本 tick 已进入无敌帧，剩余子弹不再判定
            }
            const std::int64_t graze_radius = bullet_radius + kDanmakuGrazeRadiusMilli;
            if (!live.grazed && distance_squared <= graze_radius * graze_radius) {
                live.grazed = true;
                ++player.graze;
                BossEventWire graze;
                graze.tick = tick;
                graze.type = kPlayerGrazeEvent;
                graze.player_id = player.player_id;
                tick_events_.push_back(std::move(graze));
                // 计分：擦弹 +100（实测 @0x564d3c 等三处同值；擦弹路径无按自机分支）。
                AddScore(player, danmaku::ScoreRulesFor().graze_points, tick);
            }
        }
    }
}

void BossRaceSimulation::CheckRaceWinner(std::uint64_t tick) {
    if (state_ == BossRaceState::Finished) {
        return;
    }
    std::string winner;
    for (const auto& item : players_) {  // std::map iterates in key order => deterministic
        if (item.second.boss_current_hp == 0) {
            winner = item.first;
            break;
        }
    }
    if (!winner.empty()) {
        state_ = BossRaceState::Finished;
        winner_player_id_ = winner;
        winner_tick_ = tick;
    }
}

BossRaceSnapshot BossRaceSimulation::Tick() {
    if (state_ == BossRaceState::Finished) {
        return Snapshot();
    }

    const std::uint64_t tick_to_apply = current_tick_ + 1;
    tick_events_.clear();
    tick_chains_.clear();
    StartIfReady();
    if (state_ == BossRaceState::Running) {
        // 计时分（实测 @0x562200）：每 10 tick 每玩家 +110。原版在 update 开头按
        // 帧计数取模判定，这里放在玩家 tick 之前，顺序固定。
        const danmaku::ScoreRules& score_rules = danmaku::ScoreRulesFor();
        if (mechanics_.enabled && score_rules.timer_interval_ticks > 0 &&
            tick_to_apply % score_rules.timer_interval_ticks == 0) {
            for (auto& item : players_) {
                AddScore(item.second, score_rules.timer_points, tick_to_apply);
            }
        }
        ApplyPlayerTick(tick_to_apply);
        SpawnBulletsForTick(tick_to_apply);
        AdvanceBullets(tick_to_apply);
        AdvanceSpecialMechanics(tick_to_apply);
        AdvanceMyBullets(tick_to_apply);
        JudgeMyBullets(tick_to_apply);
        JudgePlayerContacts(tick_to_apply);
        CheckRaceWinner(tick_to_apply);
        // boss_damage 聚合上报：每 8 tick 一条（或击破时立刻发），避免每 tick 刷屏。
        if (mechanics_.enabled) {
            for (auto& item : players_) {
                PlayerState& player = item.second;
                if (player.pending_boss_damage == 0) {
                    continue;
                }
                if (tick_to_apply % kBossDamageEventIntervalTicks == 0 || player.boss_current_hp == 0) {
                    BossEventWire damage_event;
                    damage_event.tick = tick_to_apply;
                    damage_event.type = kBossDamageEvent;
                    damage_event.player_id = player.player_id;
                    damage_event.value = static_cast<std::int64_t>(player.pending_boss_damage);
                    tick_events_.push_back(std::move(damage_event));
                    player.pending_boss_damage = 0;
                }
            }
        }
        event_cursor_ += static_cast<std::uint64_t>(tick_events_.size());
    }
    current_tick_ = tick_to_apply;
    return Snapshot();
}

// 权威状态 hash（客户端镜像重放校验用）。
//
// ★ 刻意**不含** score 及其派生字段（score / hit_count / spell_get / spell_total）
// 与快照里的 vx/vy 同理：它们是「消费态」而非「世界态」，不参与运动学/判定的
// 重放校验；且分值表是冻结占位（待实测），一旦校准就会变动——放进 hash 会让
// 全部历史 replay fixture 失效。同 seed 两次 run 的 hash 必须相等（score 可以不同）。
std::string BossRaceSimulation::CanonicalStateHash() const {
    std::string material = config_.match_id + "|" + std::to_string(config_.match_seed) + "|" +
        std::to_string(current_tick_) + "|" + std::to_string(static_cast<int>(state_)) + "|" +
        winner_player_id_ + "|" + std::to_string(winner_tick_);
    for (const auto& item : players_) {
        material += "|p:" + item.first + ":" +
            std::to_string(item.second.x_milli) + ":" +
            std::to_string(item.second.y_milli) + ":" +
            std::to_string(item.second.boss_current_hp) + ":" +
            std::to_string(item.second.damage_dealt);
    }
    material += "|b:" + std::to_string(live_bullets_.size());
    for (const LiveBullet& live : live_bullets_) {
        material += "|" + live.bullet_id + ":" +
            std::to_string(ToMilli(live.motion.x - kScreenCenterX)) + ":" +
            std::to_string(ToMilli(live.motion.y - kScreenCenterY)) + ":" +
            std::to_string(live.motion.bullet_kind) + ":" +
            std::to_string(live.motion.bullet_func_no);
    }
    const std::uint32_t a = Fnv1a32(material);
    const std::uint32_t b = Mix32(a ^ 0x9e3779b9u);
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%08x%08x", a, b);
    return std::string(buffer);
}

BossRaceSnapshot BossRaceSimulation::Snapshot() const {
    BossRaceSnapshot snapshot;
    snapshot.tick = current_tick_;
    snapshot.state = state_;
    snapshot.winner_player_id = winner_player_id_;
    snapshot.winner_tick = winner_tick_;
    snapshot.difficulty = config_.difficulty;  // P4（客户端只消费）
    for (const auto& item : players_) {
        BossRacePlayerSnapshot player;
        player.player_id = item.first;
        player.x_milli = item.second.x_milli;
        player.y_milli = item.second.y_milli;
        player.boss_current_hp = item.second.boss_current_hp;
        player.damage_dealt = item.second.damage_dealt;
        player.connected = item.second.connected;
        player.vx_milli = item.second.vx_milli;
        player.vy_milli = item.second.vy_milli;
        player.facing_millideg = FacingMilliDegFromDirection(item.second.last_input.direction_bits);
        if (mechanics_.enabled) {
            // 真实受击状态（§2.4 第 2 条）。
            player.lives = item.second.lives;
            player.bombs = item.second.bombs;
            player.invulnerable_until_tick = item.second.invulnerable_until_tick;
            player.graze = item.second.graze;
        } else {
            // 默认 ruleset：判定系统未启用，保持占位值（现有玩法零变化）。
            player.lives = item.second.alive ? kSnapshotDefaultLives : 0u;
            player.bombs = kSnapshotDefaultBombs;
            player.invulnerable_until_tick = 0;
            player.graze = 0;
        }
        // 计分：两种 ruleset 都填（默认 ruleset 只有击破 + 结算两项非零）。
        player.score = item.second.score;
        player.hit_count = item.second.hit_count;
        player.spell_get = item.second.spell_get;
        player.spell_total = item.second.spell_total;
        snapshot.players.push_back(player);
    }
    snapshot.bullets.reserve(live_bullets_.size());
    // 黑色帷幕 hold 期：子弹 hidden=true（客户端不渲染，服务端判定照算）。
    const bool bullets_hidden = mechanics_.enabled &&
        danmaku::BulletsHiddenDuringCurtain(black_curtain_);
    for (const LiveBullet& live : live_bullets_) {
        BossRaceBullet bullet;
        bullet.bullet_id = live.bullet_id;
        bullet.owner_player_id = live.owner_player_id;
        bullet.x_milli = ToMilli(live.motion.x - kScreenCenterX);
        bullet.y_milli = ToMilli(live.motion.y - kScreenCenterY);
        bullet.vx_milli = ToMilli(live.motion.vx);
        bullet.vy_milli = ToMilli(live.motion.vy);
        bullet.radius_milli = static_cast<std::uint32_t>(std::llround(live.motion.hit_dis * kMilliPerPixel));
        bullet.pattern_id = live.pattern_id;
        bullet.bullet_kind = live.motion.bullet_kind;
        bullet.motion_func = live.motion.bullet_func_no;
        bullet.chain_id = live.chain_id;  // 默认路径 = pattern_index（P1 占位）；ChainNode 路径 = 节点序号
        bullet.generation = live.generation;
        bullet.hidden = bullets_hidden;        // 黑色帷幕 hold 期（P4 起有真值）
        bullet.vanish_tick = live.vanish_tick;  // 阶段切换清屏标记（§2.4 第 3 条）
        snapshot.bullets.push_back(std::move(bullet));
    }

    // §2.8 boss 视角：race 每玩家一份 Boss 副本，几何/移动完全相同，故广播负载
    // 共享一份；hp 取各副本剩余最大值（每玩家真实 HP 见 players[].boss_current_hp）。
    std::int32_t boss_offset_x = 0;
    std::int32_t boss_offset_y = 0;
    if (boss_entry_ != nullptr) {
        danmaku::BossEmitterOffsetMilli(boss_entry_->movement, current_tick_, &boss_offset_x, &boss_offset_y);
    }
    snapshot.boss.x_milli = kBossOriginXMilli + boss_offset_x;
    snapshot.boss.y_milli = kBossOriginYMilli + boss_offset_y;
    snapshot.boss.max_hp = config_.boss_max_hp;
    snapshot.boss.hp = players_.empty() ? config_.boss_max_hp : 0;
    for (const auto& item : players_) {
        snapshot.boss.hp = std::max<std::uint64_t>(snapshot.boss.hp, item.second.boss_current_hp);
    }
    // phase = 当前波次使用的 pattern index（默认 ruleset 占位）；判定模式下改为
    // 符卡阶段号（各玩家副本独立推进，广播负载取最大值）。
    if (mechanics_.enabled) {
        std::uint32_t phase = 0;
        for (const auto& item : players_) {
            phase = std::max(phase, item.second.spell_phase);
        }
        snapshot.boss.phase = phase;
    } else {
        snapshot.boss.phase = static_cast<std::uint32_t>(PatternIndexForTick(current_tick_));
    }
    if (boss_entry_ != nullptr) {
        snapshot.boss.movement.type = BossMovementWireType(boss_entry_->movement.kind);
        snapshot.boss.movement.phase_t = boss_entry_->movement.period_ticks == 0
            ? 0
            : static_cast<std::int32_t>(current_tick_ % boss_entry_->movement.period_ticks);
    }

    // §2.8 boss.effects：特殊机制只在启用 ruleset 下出现（默认空数组）。
    if (mechanics_.enabled) {
        if (iron_fist_.phase != danmaku::IronFistPhase::Idle) {
            BossEffectWire effect;
            effect.type = "iron_fist";
            effect.state = IronFistStateName(iron_fist_.phase);
            effect.t_remaining = static_cast<std::int32_t>(iron_fist_.t_remaining);
            effect.x0_milli = iron_fist_.origin.x;
            effect.y0_milli = iron_fist_.origin.y;
            effect.dx_milli = iron_fist_.dir_x_milli;
            effect.dy_milli = iron_fist_.dir_y_milli;
            effect.length_milli = iron_fist_.length_milli;
            snapshot.boss.effects.push_back(std::move(effect));
        }
        if (black_curtain_.phase != danmaku::BlackCurtainPhase::Idle) {
            BossEffectWire effect;
            effect.type = "black_curtain";
            effect.state = BlackCurtainStateName(black_curtain_.phase);
            effect.t_remaining = static_cast<std::int32_t>(black_curtain_.t_remaining);
            effect.edge_y_milli = black_curtain_.edge_y_milli;
            snapshot.boss.effects.push_back(std::move(effect));
        }
    }

    // §2.8 chains：ChainNode 路径 = 本 tick 触发的链节点（真实值，见
    // SpawnChainBulletsForTick）；其余路径 = 按 pattern_index 聚合活跃弹幕
    // （P1 占位，逐字节保持现状）。
    if (chain_tables_active_) {
        snapshot.chains = tick_chains_;
    } else {
        std::map<std::uint32_t, std::uint32_t> chain_generation;
        for (const LiveBullet& live : live_bullets_) {
            std::uint32_t& generation = chain_generation[live.pattern_index];
            generation = std::max(generation, live.generation);
        }
        for (const auto& item : chain_generation) {
            BossChainWire chain;
            chain.chain_id = item.first;
            chain.pattern_index = item.first;
            chain.node_id = 0;
            chain.generation = item.second;
            chain.rng_cursor = 0;
            snapshot.chains.push_back(chain);
        }
    }

    // hand/deck/draw_cooldown：对局内抽卡（P3）才产生，P1 保持空数组与 0 默认值。
    // events：本 tick 的机制/判定事件（§2.4.6），event_cursor = 累计事件数。
    snapshot.events = tick_events_;
    snapshot.event_cursor = event_cursor_;
    snapshot.state_hash = CanonicalStateHash();
    return snapshot;
}

}  // namespace phk::battle
