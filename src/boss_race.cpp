#include "phk/battle/boss_race.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace phk::battle {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::int32_t kArenaHalfWidthMilli = 120000;
constexpr std::int32_t kArenaHalfHeightMilli = 90000;
constexpr std::int32_t kBossOriginXMilli = 0;
constexpr std::int32_t kBossOriginYMilli = -60000;

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

std::pair<std::int32_t, std::int32_t> VelocityFromAngle(double angle_rad, std::int32_t speed) {
    return {
        static_cast<std::int32_t>(std::llround(std::cos(angle_rad) * speed)),
        static_cast<std::int32_t>(std::llround(std::sin(angle_rad) * speed)),
    };
}

}  // namespace

const std::vector<std::string>& BossRacePatternIds() {
    static const std::vector<std::string> ids = {
        "ring",
        "gap_ring",
        "n_way",
        "aimed",
        "seeded_arc",
        "spiral",
        "laser_curtain",
        "homing",
        "sine_stream",
        "blossom",
    };
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
    if (config_.boss_max_hp == 0) {
        config_.boss_max_hp = kBossRaceDefaultBossHp;
    }
    if (config_.pattern_period_ticks == 0) {
        config_.pattern_period_ticks = kBossRacePatternPeriodTicks;
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

std::string BossRaceSimulation::PatternIdForTick(std::uint64_t tick) const {
    const std::uint32_t period = config_.pattern_period_ticks;
    const std::size_t index = static_cast<std::size_t>((tick / period) % kBossRacePatternCount);
    return BossRacePatternIds()[index];
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
        if ((input.direction_bits & 0x1u) != 0) { player.y_milli -= kMoveMilli; }
        if ((input.direction_bits & 0x2u) != 0) { player.x_milli += kMoveMilli; }
        if ((input.direction_bits & 0x4u) != 0) { player.y_milli += kMoveMilli; }
        if ((input.direction_bits & 0x8u) != 0) { player.x_milli -= kMoveMilli; }
        player.x_milli = ClampMilli(player.x_milli, -kArenaHalfWidthMilli, kArenaHalfWidthMilli);
        player.y_milli = ClampMilli(player.y_milli, -kArenaHalfHeightMilli, kArenaHalfHeightMilli);

        // Shooting damages this player's own Boss copy.
        if (input.shoot && player.boss_current_hp > 0) {
            const std::uint64_t damage = std::min(kBossRaceDamagePerShotTick, player.boss_current_hp);
            player.boss_current_hp -= damage;
            player.damage_dealt += damage;
        }
    }
    if (tick_it != pending_inputs_by_tick_.end()) {
        pending_inputs_by_tick_.erase(tick_it);
    }
}

void BossRaceSimulation::SpawnBulletsForTick(std::uint64_t tick) {
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

        std::size_t emitted_for_player = 0;
        for (const auto& existing : bullets_) {
            if (existing.owner_player_id == player.player_id) {
                ++emitted_for_player;
            }
        }
        if (emitted_for_player >= kBossRaceMaxBulletsPerPlayer) {
            continue;
        }

        const double aim = std::atan2(
            static_cast<double>(player.y_milli - kBossOriginYMilli),
            static_cast<double>(player.x_milli - kBossOriginXMilli)
        );

        auto push = [&](double angle, std::int32_t speed, std::uint32_t radius, std::int32_t offset_x = 0, std::int32_t offset_y = 0) {
            if (emitted_for_player >= kBossRaceMaxBulletsPerPlayer) {
                return;
            }
            const auto velocity = VelocityFromAngle(angle, speed);
            BossRaceBullet bullet;
            bullet.bullet_id = "b" + std::to_string(next_bullet_id_++);
            bullet.owner_player_id = player.player_id;
            bullet.x_milli = kBossOriginXMilli + offset_x;
            bullet.y_milli = kBossOriginYMilli + offset_y;
            bullet.vx_milli = velocity.first;
            bullet.vy_milli = velocity.second;
            bullet.radius_milli = radius;
            bullet.pattern_id = pattern_id;
            bullets_.push_back(bullet);
            ++emitted_for_player;
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

void BossRaceSimulation::AdvanceBullets() {
    for (auto& bullet : bullets_) {
        bullet.x_milli += bullet.vx_milli;
        bullet.y_milli += bullet.vy_milli;
    }
    bullets_.erase(
        std::remove_if(
            bullets_.begin(),
            bullets_.end(),
            [](const BossRaceBullet& bullet) {
                return std::abs(bullet.x_milli) > kArenaHalfWidthMilli + 20000 ||
                    std::abs(bullet.y_milli) > kArenaHalfHeightMilli + 20000;
            }
        ),
        bullets_.end()
    );
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
    StartIfReady();
    if (state_ == BossRaceState::Running) {
        ApplyPlayerTick(tick_to_apply);
        SpawnBulletsForTick(tick_to_apply);
        AdvanceBullets();
        CheckRaceWinner(tick_to_apply);
    }
    current_tick_ = tick_to_apply;
    return Snapshot();
}

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
    material += "|b:" + std::to_string(bullets_.size());
    for (const auto& bullet : bullets_) {
        material += "|" + bullet.bullet_id + ":" + std::to_string(bullet.x_milli) + ":" + std::to_string(bullet.y_milli);
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
    for (const auto& item : players_) {
        BossRacePlayerSnapshot player;
        player.player_id = item.first;
        player.x_milli = item.second.x_milli;
        player.y_milli = item.second.y_milli;
        player.boss_current_hp = item.second.boss_current_hp;
        player.damage_dealt = item.second.damage_dealt;
        player.connected = item.second.connected;
        snapshot.players.push_back(player);
    }
    snapshot.bullets = bullets_;
    snapshot.state_hash = CanonicalStateHash();
    return snapshot;
}

}  // namespace phk::battle
