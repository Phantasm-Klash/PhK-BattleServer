#include "phk/battle/coop_boss.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "phk/battle/lobby_rng.hpp"

namespace phk::battle {
namespace {

constexpr double kPi = 3.14159265358979323846;

std::int32_t ClampMilli(std::int32_t value, std::int32_t lo, std::int32_t hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

std::uint32_t Fnv1a32(const std::string& text) {
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

double Unit(std::int64_t seed, std::int64_t tick, const char* stream, std::int64_t index) {
    return LobbyDeterministicUnit(seed, tick, stream, index);
}

const char* StateName(CoopBossState state) {
    switch (state) {
        case CoopBossState::Waiting: return "waiting";
        case CoopBossState::Running: return "running";
        case CoopBossState::Finished: return "finished";
    }
    return "unknown";
}

}  // namespace

bool IsCoopBossMode(const std::string& mode_id) {
    return mode_id == kCoopBossModeId;
}

void CoopSeatSpawnMilli(std::size_t seat_count, std::size_t seat, std::int32_t* x_milli, std::int32_t* y_milli) {
    const std::int32_t r = kCoopPlayerSpawnMilli;
    // 2 人：0=下朝上，1=上朝下。4 人：0=下、1=右、2=上、3=左。
    if (seat_count <= 2) {
        *x_milli = 0;
        *y_milli = seat == 0 ? r : -r;
        return;
    }
    switch (seat % 4) {
        case 0: *x_milli = 0; *y_milli = r; break;
        case 1: *x_milli = r; *y_milli = 0; break;
        case 2: *x_milli = 0; *y_milli = -r; break;
        default: *x_milli = -r; *y_milli = 0; break;
    }
}

std::int32_t CoopSeatFacingMilliDeg(std::size_t seat_count, std::size_t seat) {
    if (seat_count <= 2) {
        return seat == 0 ? 0 : 18000;  // 0=上，18000=下
    }
    switch (seat % 4) {
        case 0: return 0;      // 下方位 → 朝上
        case 1: return 27000;  // 右方位 → 朝左
        case 2: return 18000;  // 上方位 → 朝下
        default: return 9000;  // 左方位 → 朝右
    }
}

CoopBossSimulation::CoopBossSimulation(CoopBossConfig config) : config_(std::move(config)) {
    if (config_.mode_id.empty()) {
        config_.mode_id = kCoopBossModeId;
    }
    if (config_.tick_rate_hz == 0) {
        config_.tick_rate_hz = kCoopBossTickRateHz;
    }
    ResolveLoadouts();
}

void CoopBossSimulation::ResolveLoadouts() {
    shot_by_player_.clear();
    for (const PlayerLoadoutSpec& loadout : config_.loadouts) {
        if (loadout.player_id.empty() || loadout.shot_type.empty()) {
            continue;
        }
        for (const danmaku::PlayerShotSpec& spec : danmaku::PlayerShotTable()) {
            if (loadout.shot_type == spec.name) {
                shot_by_player_[loadout.player_id] = &spec;
                break;
            }
        }
    }
}

const CoopBossConfig& CoopBossSimulation::Config() const { return config_; }
CoopBossState CoopBossSimulation::State() const { return state_; }
std::uint64_t CoopBossSimulation::CurrentTick() const { return current_tick_; }
std::size_t CoopBossSimulation::PlayerCount() const { return players_.size(); }
bool CoopBossSimulation::BossDefeated() const { return state_ == CoopBossState::Finished && boss_hp_ == 0; }

bool CoopBossSimulation::AddPlayer(const std::string& player_id, std::int32_t x_milli, std::int32_t y_milli) {
    if (player_id.empty() || players_.size() >= kCoopMaxPlayers) {
        return false;
    }
    if (players_.find(player_id) != players_.end()) {
        return false;
    }
    PlayerState player;
    player.player_id = player_id;
    player.x_milli = ClampMilli(x_milli, -kCoopArenaHalfMilli, kCoopArenaHalfMilli);
    player.y_milli = ClampMilli(y_milli, -kCoopArenaHalfMilli, kCoopArenaHalfMilli);
    player.last_input.match_id = config_.match_id;
    player.last_input.player_id = player_id;
    players_[player_id] = player;
    return true;
}

bool CoopBossSimulation::SetPlayerConnected(const std::string& player_id, bool connected) {
    const auto it = players_.find(player_id);
    if (it == players_.end()) return false;
    it->second.connected = connected;
    return true;
}

void CoopBossSimulation::PushEvent(CardCastEvent row) {
    if (events_.size() >= kCardCastEventRowLimit) return;
    events_.push_back(std::move(row));
}

std::vector<CardCastEvent> CoopBossSimulation::TakeEvents(std::size_t limit) {
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

void CoopBossSimulation::RestoreEvents(const std::vector<CardCastEvent>& rows) {
    events_.insert(events_.begin(), rows.begin(), rows.end());
}

bool CoopBossSimulation::SubmitInput(const BattleInput& input) {
    if (state_ == CoopBossState::Finished) return false;
    if (!input.version.IsCompatible() || input.match_id != config_.match_id) return false;
    const auto it = players_.find(input.player_id);
    if (it == players_.end() || !it->second.connected) return false;
    if (input.seq == 0 || input.seq <= it->second.last_seq) return false;
    if (input.tick <= current_tick_) return false;
    if (input.tick - current_tick_ > 8) return false;
    if ((input.direction_bits & ~0x0fu) != 0) return false;
    it->second.last_seq = input.seq;
    pending_inputs_by_tick_[input.tick][input.player_id] = input;
    return true;
}

void CoopBossSimulation::StartIfReady() {
    if (state_ != CoopBossState::Waiting) return;
    if (players_.size() >= kCoopMinPlayers) {
        state_ = CoopBossState::Running;
        if (config_.boss_max_hp == 0) {
            config_.boss_max_hp = kCoopBossHpPerPlayer * players_.size();
        }
        boss_hp_ = config_.boss_max_hp;
    }
}

// 机制时间表（契约 §2.3.5，600 tick 大循环）：
//   t % 600 ∈ [0,300)   只有 fan_attack（首个窗口无 spiral）
//   t % 600 ∈ [300,480) shield 格挡窗口（1 或 3 向，窗口起点定）
//   t % 600 ∈ [450,600) cannon 巨炮封锁（150 tick）
//   t % 90 == 0         fan 多向齐射（恒开）
//   spiral 窗口：[600,900)、[1200,1500)…（即 w>=1 且 phase<300）
void CoopBossSimulation::RecomputeMechanics(std::uint64_t tick) {
    shield_dir_mask_ = 0;
    lock_dir_ = -1;
    lock_until_tick_ = 0;
    spiral_active_ = false;
    const std::size_t seats = players_.size();
    if (seats == 0 || tick == 0) return;
    const std::int64_t seed = config_.match_seed;
    const std::int64_t w = static_cast<std::int64_t>(tick / kCoopCannonIntervalTicks);
    const std::uint32_t phase = static_cast<std::uint32_t>(tick % kCoopCannonIntervalTicks);

    if (phase >= 300 && phase < 300 + kCoopShieldWindowTicks) {
        const double count_roll = Unit(seed, w, "coop_shield_count", 0);
        const std::size_t want = count_roll < 0.5 ? 1 : 3;
        // 绝不全封：2p 最多封 1 向（3 向在只有 2 座位时退化为封死双方，
        // 会让合作模式在窗口期完全打不动 Boss）；4p 可封 1 或 3 向。
        const std::size_t max_block = seats > 1 ? seats - 1 : 1;
        const std::size_t count = std::min(want, max_block);
        // 候选方向按 (unit, seat) 排序取前 count：确定性且天然去重。
        std::vector<std::pair<double, std::size_t>> ranked;
        ranked.reserve(seats);
        for (std::size_t seat = 0; seat < seats; ++seat) {
            ranked.emplace_back(Unit(seed, w, "coop_shield_rank", static_cast<std::int64_t>(seat)), seat);
        }
        std::sort(ranked.begin(), ranked.end());
        for (std::size_t i = 0; i < count; ++i) {
            shield_dir_mask_ |= (1u << ranked[i].second);
        }
    }
    // 巨炮段 [450,600)：起点 450 + 时长 150 = 间隔 600，在循环边界收口（契约 §2.3.5）。
    if (phase >= kCoopCannonStartOffset && phase < kCoopCannonStartOffset + kCoopCannonDurationTicks) {
        const double roll = Unit(seed, w, "coop_cannon_dir", 0);
        std::int32_t dir = static_cast<std::int32_t>(roll * static_cast<double>(seats));
        if (dir < 0) dir = 0;
        if (static_cast<std::size_t>(dir) >= seats) dir = static_cast<std::int32_t>(seats) - 1;
        lock_dir_ = dir;
        lock_until_tick_ = (static_cast<std::uint64_t>(w) + 1) * kCoopCannonIntervalTicks;
    }
    // spiral 窗口：[600,900)、[1200,1500)…（契约 §2.3.5；[0,300) 只有 fan）。
    if (w >= 1 && phase < kCoopSpiralWindowTicks) {
        spiral_active_ = true;
    }
}

void CoopBossSimulation::ApplyPlayerTick(std::uint64_t tick) {
    const auto tick_it = pending_inputs_by_tick_.find(tick);
    std::uint32_t seat = 0;
    for (auto& item : players_) {
        PlayerState& player = item.second;
        if (!player.connected) {
            ++seat;
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
        // 巨炮封锁：该座位位移与射击冻结（契约 §2.3.3）。
        const bool locked = (lock_dir_ >= 0 && static_cast<std::uint32_t>(lock_dir_) == seat);
        if (!locked) {
            std::int32_t vx = 0;
            std::int32_t vy = 0;
            if ((input.direction_bits & 0x1u) != 0) vy -= kCoopMoveMilli;
            if ((input.direction_bits & 0x2u) != 0) vx += kCoopMoveMilli;
            if ((input.direction_bits & 0x4u) != 0) vy += kCoopMoveMilli;
            if ((input.direction_bits & 0x8u) != 0) vx -= kCoopMoveMilli;
            player.vx_milli = vx;
            player.vy_milli = vy;
            player.x_milli += vx;
            player.y_milli += vy;
            player.x_milli = ClampMilli(player.x_milli, -kCoopArenaHalfMilli, kCoopArenaHalfMilli);
            player.y_milli = ClampMilli(player.y_milli, -kCoopArenaHalfMilli, kCoopArenaHalfMilli);
        } else {
            player.vx_milli = 0;
            player.vy_milli = 0;
        }
        // 射击：对共享 Boss 造成伤害；本方向被格挡时归零（契约 §2.3.2）。
        const bool shielded = (shield_dir_mask_ & (1u << seat)) != 0;
        if (!locked && input.shoot && boss_hp_ > 0 && !shielded) {
            std::uint64_t tick_damage = kCoopDamagePerShotTick;
            if (const danmaku::PlayerShotSpec* spec = ShotSpecFor(player.player_id);
                spec != nullptr && spec->damage >= 0) {
                tick_damage = static_cast<std::uint64_t>(spec->damage);
            }
            if (input.bomb) {
                for (const danmaku::PlayerBombSpec& bomb : danmaku::PlayerBombTable()) {
                    const auto loadout_it = std::find_if(
                        config_.loadouts.begin(), config_.loadouts.end(),
                        [&player](const PlayerLoadoutSpec& spec) { return spec.player_id == player.player_id; });
                    if (loadout_it != config_.loadouts.end() && loadout_it->bomb_type == bomb.name && bomb.damage >= 0) {
                        tick_damage += static_cast<std::uint64_t>(bomb.damage);
                        break;
                    }
                }
            }
            const std::uint64_t damage = std::min(tick_damage, boss_hp_);
            boss_hp_ -= damage;
            player.damage_dealt += damage;
        }
        ++seat;
    }
    if (tick_it != pending_inputs_by_tick_.end()) {
        pending_inputs_by_tick_.erase(tick_it);
    }
}

std::uint32_t CoopBossSimulation::SeatOf(const std::string& player_id) const {
    std::uint32_t seat = 0;
    for (const auto& item : players_) {
        if (item.first == player_id) return seat;
        ++seat;
    }
    return 0;
}

const danmaku::PlayerShotSpec* CoopBossSimulation::ShotSpecFor(const std::string& player_id) const {
    const auto it = shot_by_player_.find(player_id);
    return it == shot_by_player_.end() ? nullptr : it->second;
}

void CoopBossSimulation::SpawnBulletsForTick(std::uint64_t tick) {
    if (tick == 0) return;
    const std::size_t seats = players_.size();
    if (seats == 0) return;
    const std::int64_t seed = config_.match_seed;
    std::uint32_t seat = 0;
    for (const auto& item : players_) {
        const PlayerState& player = item.second;
        if (!player.connected) {
            ++seat;
            continue;
        }
        // fan_attack：每 kCoopFanIntervalTicks，从 Boss 中心向本座位 7 发扇形。
        if (tick % kCoopFanIntervalTicks == 0 && live_bullets_.size() < kCoopMaxBullets) {
            const double dx = static_cast<double>(player.x_milli);
            const double dy = static_cast<double>(player.y_milli);
            const double aim = std::atan2(dy, dx);
            constexpr double kFanSpeedMilli = 3600.0;
            for (std::uint32_t i = 0; i < kCoopFanBulletsPerDir; ++i) {
                const double spread = (static_cast<double>(i) - 3.0) * (kPi / 18.0);  // 10° 步进，总 60°
                const double angle = aim + spread;
                LiveBullet live;
                live.bullet_id = "c" + std::to_string(next_bullet_id_++);
                live.owner_player_id = player.player_id;
                live.x_milli = 0;
                live.y_milli = 0;
                live.vx_milli = static_cast<std::int32_t>(std::llround(kFanSpeedMilli * std::cos(angle)));
                live.vy_milli = static_cast<std::int32_t>(std::llround(kFanSpeedMilli * std::sin(angle)));
                live.radius_milli = 4000;
                live.kind = static_cast<std::uint32_t>(CoopBulletKind::Fan);
                live.pattern_id = "coop_fan";
                live_bullets_.push_back(std::move(live));
            }
        }
        // cannon_lock：巨炮窗口内每 tick 一发巨宽弹，瞄准被封锁座位当前位置。
        if (lock_dir_ >= 0 && static_cast<std::uint32_t>(lock_dir_) == seat &&
            live_bullets_.size() < kCoopMaxBullets) {
            const double dx = static_cast<double>(player.x_milli);
            const double dy = static_cast<double>(player.y_milli);
            const double len = std::sqrt(dx * dx + dy * dy);
            const double ux = len > 0.0 ? dx / len : 0.0;
            const double uy = len > 0.0 ? dy / len : -1.0;
            constexpr double kCannonSpeedMilli = 3000.0;
            LiveBullet live;
            live.bullet_id = "c" + std::to_string(next_bullet_id_++);
            live.owner_player_id = player.player_id;
            live.x_milli = 0;
            live.y_milli = 0;
            live.vx_milli = static_cast<std::int32_t>(std::llround(kCannonSpeedMilli * ux));
            live.vy_milli = static_cast<std::int32_t>(std::llround(kCannonSpeedMilli * uy));
            live.radius_milli = kCoopCannonBulletRadiusMilli;
            live.kind = static_cast<std::uint32_t>(CoopBulletKind::Cannon);
            live.pattern_id = "coop_cannon";
            live_bullets_.push_back(std::move(live));
        }
        ++seat;
    }
    // spiral：全屏旋转。每 kCoopSpiralEveryTicks 四向对称一发，角度随 tick 旋转；
    // 半径随窗口推进外扩至铺满全场（契约 §2.3.4 的实现解释：一发 = 四向对称组）。
    if (spiral_active_ && tick % kCoopSpiralEveryTicks == 0 && live_bullets_.size() < kCoopMaxBullets) {
        const double angle = static_cast<double>(tick % 360000u) * 0.9 * (kPi / 180.0);
        const double radius = 12000.0 + static_cast<double>(tick % kCoopSpiralWindowTicks) * 260.0;
        constexpr double kSpiralOutMilli = 2600.0;
        constexpr double kSpiralTanMilli = 800.0;
        for (std::uint32_t i = 0; i < 4; ++i) {
            const double theta = angle + static_cast<double>(i) * (kPi / 2.0);
            const double cos_t = std::cos(theta);
            const double sin_t = std::sin(theta);
            LiveBullet live;
            live.bullet_id = "c" + std::to_string(next_bullet_id_++);
            live.owner_player_id = "boss";  // 全场共有，观察方渲染为公共弹幕
            live.x_milli = static_cast<std::int32_t>(std::llround(radius * cos_t));
            live.y_milli = static_cast<std::int32_t>(std::llround(radius * sin_t));
            // 外向 + 切向（ swirl 旋转感）。
            live.vx_milli = static_cast<std::int32_t>(std::llround(kSpiralOutMilli * cos_t - kSpiralTanMilli * sin_t));
            live.vy_milli = static_cast<std::int32_t>(std::llround(kSpiralOutMilli * sin_t + kSpiralTanMilli * cos_t));
            live.radius_milli = 4200;
            live.kind = static_cast<std::uint32_t>(CoopBulletKind::Spiral);
            live.pattern_id = "coop_spiral";
            live_bullets_.push_back(std::move(live));
        }
    }
}

void CoopBossSimulation::AdvanceBullets() {
    const std::int32_t cull = kCoopArenaHalfMilli + 20000;
    for (LiveBullet& live : live_bullets_) {
        live.x_milli += live.vx_milli;
        live.y_milli += live.vy_milli;
    }
    live_bullets_.erase(
        std::remove_if(
            live_bullets_.begin(), live_bullets_.end(),
            [cull](const LiveBullet& live) {
                return live.x_milli < -cull || live.x_milli > cull ||
                    live.y_milli < -cull || live.y_milli > cull;
            }),
        live_bullets_.end());
}

void CoopBossSimulation::CheckBossDefeated(std::uint64_t tick) {
    if (state_ == CoopBossState::Finished) return;
    if (boss_hp_ == 0 && config_.boss_max_hp > 0) {
        state_ = CoopBossState::Finished;
        // coop 无单人 winner：全部存活玩家共同胜利（winner 字段留空）。
    }
    (void)tick;
}

CoopBossSnapshot CoopBossSimulation::Tick() {
    if (state_ == CoopBossState::Finished) {
        return Snapshot();
    }
    const std::uint64_t tick_to_apply = current_tick_ + 1;
    StartIfReady();
    if (state_ == CoopBossState::Running) {
        RecomputeMechanics(tick_to_apply);
        ApplyPlayerTick(tick_to_apply);
        SpawnBulletsForTick(tick_to_apply);
        AdvanceBullets();
        CheckBossDefeated(tick_to_apply);
    }
    current_tick_ = tick_to_apply;
    return Snapshot();
}

std::string CoopBossSimulation::CanonicalStateHash() const {
    std::string material = config_.match_id + "|" + std::to_string(config_.match_seed) + "|" +
        std::to_string(current_tick_) + "|" + std::to_string(static_cast<int>(state_)) + "|" +
        std::to_string(boss_hp_) + "|" + std::to_string(config_.boss_max_hp) + "|" +
        std::to_string(shield_dir_mask_) + "|" + std::to_string(lock_dir_) + "|" +
        std::to_string(lock_until_tick_) + "|" + (spiral_active_ ? "1" : "0");
    for (const auto& item : players_) {
        material += "|p:" + item.first + ":" +
            std::to_string(item.second.x_milli) + ":" +
            std::to_string(item.second.y_milli) + ":" +
            std::to_string(item.second.damage_dealt);
    }
    material += "|b:" + std::to_string(live_bullets_.size());
    for (const LiveBullet& live : live_bullets_) {
        material += "|" + live.bullet_id + ":" +
            std::to_string(live.x_milli) + ":" +
            std::to_string(live.y_milli) + ":" +
            std::to_string(live.vx_milli) + ":" +
            std::to_string(live.vy_milli) + ":" +
            std::to_string(live.kind) + ":" +
            std::to_string(live.radius_milli);
    }
    const std::uint32_t a = Fnv1a32(material);
    const std::uint32_t b = Mix32(a ^ 0x9e3779b9u);
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%08x%08x", a, b);
    return std::string(buffer);
}

CoopBossSnapshot CoopBossSimulation::Snapshot() const {
    CoopBossSnapshot snapshot;
    snapshot.tick = current_tick_;
    snapshot.state = state_;
    snapshot.boss_hp = boss_hp_;
    snapshot.boss_max_hp = config_.boss_max_hp;
    snapshot.seat_count = static_cast<std::uint32_t>(players_.size());
    snapshot.shield_dir_mask = shield_dir_mask_;
    snapshot.lock_dir = lock_dir_;
    snapshot.lock_until_tick = lock_until_tick_;
    snapshot.spiral_active = spiral_active_;
    std::uint32_t seat = 0;
    for (const auto& item : players_) {
        CoopBossPlayerSnapshot player;
        player.player_id = item.first;
        player.seat = seat;
        player.x_milli = item.second.x_milli;
        player.y_milli = item.second.y_milli;
        player.facing_millideg = CoopSeatFacingMilliDeg(players_.size(), seat);
        player.damage_dealt = item.second.damage_dealt;
        player.connected = item.second.connected;
        player.locked = (lock_dir_ >= 0 && static_cast<std::uint32_t>(lock_dir_) == seat);
        player.shielded = (shield_dir_mask_ & (1u << seat)) != 0;
        player.vx_milli = item.second.vx_milli;
        player.vy_milli = item.second.vy_milli;
        player.lives = kSnapshotDefaultLives;
        player.bombs = kSnapshotDefaultBombs;
        player.invulnerable_until_tick = 0;  // 判定系统 P3
        player.graze = 0;                    // 擦弹计数 P3
        snapshot.players.push_back(std::move(player));
        ++seat;
    }
    snapshot.bullets.reserve(live_bullets_.size());
    for (const LiveBullet& live : live_bullets_) {
        CoopBossBullet bullet;
        bullet.bullet_id = live.bullet_id;
        bullet.owner_player_id = live.owner_player_id;
        bullet.x_milli = live.x_milli;
        bullet.y_milli = live.y_milli;
        bullet.vx_milli = live.vx_milli;
        bullet.vy_milli = live.vy_milli;
        bullet.radius_milli = live.radius_milli;
        bullet.kind = live.kind;
        bullet.pattern_id = live.pattern_id;
        bullet.chain_id = 0;    // coop 无链式分裂
        bullet.generation = 0;
        bullet.hidden = false;  // 黑色帷幕 P4
        snapshot.bullets.push_back(std::move(bullet));
    }
    // §2.8 共享 Boss 视角：coop 的 Boss 固定在池中心（无移动状态机）。
    snapshot.boss.x_milli = 0;
    snapshot.boss.y_milli = 0;
    snapshot.boss.hp = boss_hp_;
    snapshot.boss.max_hp = config_.boss_max_hp;
    snapshot.boss.phase = 0;
    snapshot.boss.movement.type = "static";
    snapshot.boss.movement.phase_t = 0;
    // chains/hand/events 在 coop 无对应机制，保持空数组与 0 默认值。
    snapshot.state_hash = CanonicalStateHash();
    return snapshot;
}

}  // namespace phk::battle
