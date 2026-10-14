// 卡牌效果执行层实现。逐函数对照 Gensoulkyo/runtime/core/card_effects.go。
//
// 版权：东方同人逆向成果，仅用于自研玩法。

#include "phk/battle/card_effects.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "phk/battle/lobby_rng.hpp"

namespace phk::battle {
namespace {

// bulletDelta 的四舍五入（Go round，service.go:7618）。
double RoundTo(double value, int precision) {
    const double scale = std::pow(10.0, precision);
    return std::round(value * scale) / scale;
}

// Go bulletState → CardBulletDelta（模拟 Go bulletDelta()）。
CardBulletDelta MakeDelta(
    const std::string& op,
    const std::string& bullet_id,
    const std::string& pattern_id,
    const std::string& kind,
    std::int64_t tick,
    double x,
    double y,
    double vx,
    double vy,
    double radius,
    std::int32_t damage,
    const std::string& color
) {
    CardBulletDelta delta;
    delta.op = op;
    delta.bullet_id = bullet_id;
    delta.pattern_id = pattern_id;
    delta.kind = kind;
    delta.tick = tick;
    delta.x = RoundTo(x, 3);
    delta.y = RoundTo(y, 3);
    delta.vx = RoundTo(vx, 4);
    delta.vy = RoundTo(vy, 4);
    delta.radius = RoundTo(radius, 3);
    delta.damage = damage;
    delta.color = color;
    return delta;
}

// Go addBulletLocked（simulation.go:149）：id = "<matchID>_b%06d"。
std::string MakeBulletId(const std::string& match_id, std::uint64_t seq) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "_b%06llu", static_cast<unsigned long long>(seq));
    return match_id + buffer;
}

}  // namespace

std::vector<std::string> SortedBulletIds(const std::vector<CardBulletSnapshot>& bullets) {
    std::vector<std::string> ids;
    ids.reserve(bullets.size());
    for (const CardBulletSnapshot& bullet : bullets) {
        ids.push_back(bullet.bullet_id);
    }
    std::sort(ids.begin(), ids.end());  // 字节序字典序（std::string 比较即 UTF-8 字节序）
    return ids;
}

std::string OpponentTargetId(const std::vector<std::string>& player_ids, const std::string& self_id) {
    std::vector<std::string> candidates;
    candidates.reserve(player_ids.size());
    for (const std::string& id : player_ids) {
        if (id != self_id) {
            candidates.push_back(id);
        }
    }
    if (candidates.empty()) {
        return "";
    }
    std::sort(candidates.begin(), candidates.end());
    return candidates.front();
}

double RoundTo(double value, int precision) {
    const double scale = std::pow(10.0, precision);
    return std::round(value * scale) / scale;
}

CardEffectResult ApplyOpponentPattern(
    const std::vector<CardPlayerSnapshot>& players,
    const std::string& caster_id,
    const CardActivation& activation,
    std::int64_t tick,
    const std::string& match_id,
    std::uint64_t next_bullet_seq,
    std::size_t existing_bullet_count
) {
    CardEffectResult result;

    std::vector<std::string> player_ids;
    player_ids.reserve(players.size());
    for (const CardPlayerSnapshot& player : players) {
        player_ids.push_back(player.player_id);
    }
    const std::string target_id = OpponentTargetId(player_ids, caster_id);
    if (target_id.empty()) {
        // 无对手：只发 value=0 的事件，不产子弹、不写 target（Go 提前返回）。
        return result;
    }

    const CardPlayerSnapshot* target = nullptr;
    for (const CardPlayerSnapshot& player : players) {
        if (player.player_id == target_id) {
            target = &player;
            break;
        }
    }
    if (target == nullptr) {
        return result;
    }

    std::string pattern_id = activation.pattern_id;
    if (pattern_id.empty()) {
        pattern_id = activation.card_id;
    }

    // 从 boss 原点瞄准对手（Go math.Atan2(dy, dx)）。
    const double base = std::atan2(target->y - kBossOriginY, target->x - kBossOriginX);
    const double start = base - kCardOpponentPatternSpread * 0.5;
    double step = 0.0;
    if (kCardOpponentPatternCount > 1) {
        step = kCardOpponentPatternSpread / static_cast<double>(kCardOpponentPatternCount - 1);
    }

    std::uint64_t seq = next_bullet_seq;
    std::size_t field_count = existing_bullet_count;
    for (int i = 0; i < kCardOpponentPatternCount; ++i) {
        if (field_count >= kMaxBullets) {
            break;  // addBulletLocked 封顶：不再生成
        }
        const double angle = start + step * static_cast<double>(i);
        const std::string bullet_id = MakeBulletId(match_id, seq);
        result.bullet_deltas.push_back(MakeDelta(
            "spawn",
            bullet_id,
            pattern_id,
            "nway",
            tick,
            kBossOriginX,
            kBossOriginY,
            std::cos(angle) * kCardOpponentPatternSpeed,
            std::sin(angle) * kCardOpponentPatternSpeed,
            kCardOpponentPatternRadius,
            1,
            kCardOpponentPatternColor
        ));
        ++seq;
        ++field_count;
    }

    result.target_user_id = target_id;
    result.has_target = true;
    result.event_value = static_cast<std::int64_t>(result.bullet_deltas.size());
    return result;
}

CardEffectResult ApplyBulletPurge(
    const std::vector<CardBulletSnapshot>& bullets,
    const CardPlayerSnapshot& caster,
    const CardActivation& activation,
    std::int64_t seed,
    std::int64_t tick
) {
    CardEffectResult result;
    const double radius = activation.purge_radius;
    const std::int64_t count = activation.purge_count;
    if (radius > 0.0 && count > 0) {
        struct PurgeCandidate {
            std::string bullet_id;
            double unit;
        };
        std::vector<PurgeCandidate> candidates;

        // 规范下标 p 在「全场 bullet id 排序序列」上（含半径外子弹）。
        const std::vector<std::string> sorted_ids = SortedBulletIds(bullets);
        for (std::size_t index = 0; index < sorted_ids.size(); ++index) {
            const CardBulletSnapshot* bullet = nullptr;
            for (const CardBulletSnapshot& candidate : bullets) {
                if (candidate.bullet_id == sorted_ids[index]) {
                    bullet = &candidate;
                    break;
                }
            }
            if (bullet == nullptr) {
                continue;
            }
            if (std::hypot(bullet->x - caster.x, bullet->y - caster.y) > radius) {
                continue;
            }
            candidates.push_back(PurgeCandidate{
                bullet->bullet_id,
                LobbyDeterministicUnit(seed, tick, activation.card_id, static_cast<std::int64_t>(index))
            });
        }
        std::sort(candidates.begin(), candidates.end(), [](const PurgeCandidate& a, const PurgeCandidate& b) {
            if (a.unit != b.unit) {
                return a.unit < b.unit;
            }
            return a.bullet_id < b.bullet_id;
        });
        std::size_t limit = static_cast<std::size_t>(count);
        if (limit > candidates.size()) {
            limit = candidates.size();
        }
        for (std::size_t i = 0; i < limit; ++i) {
            const CardBulletSnapshot* bullet = nullptr;
            for (const CardBulletSnapshot& candidate : bullets) {
                if (candidate.bullet_id == candidates[i].bullet_id) {
                    bullet = &candidate;
                    break;
                }
            }
            if (bullet == nullptr) {
                continue;
            }
            result.bullet_deltas.push_back(MakeDelta(
                "despawn",
                bullet->bullet_id,
                bullet->pattern_id,
                bullet->kind,
                tick,
                bullet->x,
                bullet->y,
                bullet->vx,
                bullet->vy,
                bullet->radius,
                bullet->damage,
                bullet->color
            ));
        }
    }
    result.event_value = static_cast<std::int64_t>(result.bullet_deltas.size());
    return result;
}

CardEffectResult ApplyInvulnerable(const CardActivation& activation, std::int64_t tick) {
    (void)tick;  // Go 直接采用通用路径算好的 ExpiresTick
    CardEffectResult result;
    result.invulnerable_until = activation.expires_tick;
    result.event_value = 0;
    return result;
}

CardEffectResult ApplyFinisher(const std::vector<CardBulletSnapshot>& bullets, std::int64_t tick) {
    CardEffectResult result;
    const std::vector<std::string> sorted_ids = SortedBulletIds(bullets);
    for (const std::string& id : sorted_ids) {
        const CardBulletSnapshot* bullet = nullptr;
        for (const CardBulletSnapshot& candidate : bullets) {
            if (candidate.bullet_id == id) {
                bullet = &candidate;
                break;
            }
        }
        if (bullet == nullptr) {
            continue;
        }
        result.bullet_deltas.push_back(MakeDelta(
            "despawn",
            bullet->bullet_id,
            bullet->pattern_id,
            bullet->kind,
            tick,
            bullet->x,
            bullet->y,
            bullet->vx,
            bullet->vy,
            bullet->radius,
            bullet->damage,
            bullet->color
        ));
    }
    result.event_value = static_cast<std::int64_t>(result.bullet_deltas.size());
    return result;
}

}  // namespace phk::battle
