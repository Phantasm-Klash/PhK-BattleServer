// 合作模式（coop_boss）模拟测试——契约 COOP_MODE_CONTRACT.md §2 / §6.3。
//
// 覆盖：座位确定性、2/4 向多向齐射、方向格挡（1 或 3 向、伤害归零）、
// 巨炮封锁（位移与射击冻结）、全屏旋转弹幕（角度随 tick 旋转）、
// 固定种子逐字节确定性、Boss 阵亡终局、输入校验拒绝路径。
//
// 版权：东方同人逆向成果，仅用于自研玩法。

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "phk/battle/coop_boss.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& label) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << label << "\n";
    }
}

using phk::battle::BattleInput;
using phk::battle::CoopBossConfig;
using phk::battle::CoopBossSimulation;
using phk::battle::CoopBossSnapshot;
using phk::battle::CoopBossState;
using phk::battle::CoopSeatFacingMilliDeg;
using phk::battle::CoopSeatSpawnMilli;

BattleInput MakeInput(
    const std::string& match_id,
    const std::string& player_id,
    std::uint64_t tick,
    std::uint64_t seq,
    bool shoot,
    std::uint32_t direction_bits = 0
) {
    BattleInput input;
    input.match_id = match_id;
    input.player_id = player_id;
    input.tick = tick;
    input.seq = seq;
    input.shoot = shoot;
    input.direction_bits = direction_bits;
    return input;
}

CoopBossConfig MakeConfig(std::int64_t seed) {
    CoopBossConfig config;
    config.match_id = "match_coop_000001";
    config.match_seed = seed;
    return config;
}

std::size_t CountKind(const CoopBossSnapshot& snapshot, std::uint32_t kind) {
    std::size_t count = 0;
    for (const auto& bullet : snapshot.bullets) {
        if (bullet.kind == kind) count += 1;
    }
    return count;
}

// 座位：sorted(player_ids) 下标 → 出生点与朝向（2 人上下 / 4 人四向）。
void TestSeatAssignment() {
    std::int32_t x = 0;
    std::int32_t y = 0;
    CoopSeatSpawnMilli(2, 0, &x, &y);
    Check(x == 0 && y == 70200, "2p seat0 must spawn at bottom");
    Check(CoopSeatFacingMilliDeg(2, 0) == 0, "2p seat0 must face up");
    CoopSeatSpawnMilli(2, 1, &x, &y);
    Check(x == 0 && y == -70200, "2p seat1 must spawn at top");
    Check(CoopSeatFacingMilliDeg(2, 1) == 18000, "2p seat1 must face down");

    CoopSeatSpawnMilli(4, 0, &x, &y);
    Check(x == 0 && y == 70200, "4p seat0 must spawn at bottom");
    CoopSeatSpawnMilli(4, 1, &x, &y);
    Check(x == 70200 && y == 0, "4p seat1 must spawn at right");
    CoopSeatSpawnMilli(4, 2, &x, &y);
    Check(x == 0 && y == -70200, "4p seat2 must spawn at top");
    CoopSeatSpawnMilli(4, 3, &x, &y);
    Check(x == -70200 && y == 0, "4p seat3 must spawn at left");
    Check(CoopSeatFacingMilliDeg(4, 1) == 27000, "4p seat1 must face left");
    Check(CoopSeatFacingMilliDeg(4, 2) == 18000, "4p seat2 must face down");
    Check(CoopSeatFacingMilliDeg(4, 3) == 9000, "4p seat3 must face right");

    // 模拟内的座位序 = map key 序（sorted），快照 seat 字段必须一致。
    CoopBossSimulation sim(MakeConfig(1));
    sim.AddPlayer("p-b", 0, 0);
    sim.AddPlayer("p-a", 0, 0);
    const CoopBossSnapshot snapshot = sim.Snapshot();
    Check(snapshot.players.size() == 2, "snapshot must carry both players");
    Check(snapshot.players[0].player_id == "p-a" && snapshot.players[0].seat == 0, "seat0 = sorted first id");
    Check(snapshot.players[1].player_id == "p-b" && snapshot.players[1].seat == 1, "seat1 = sorted second id");
}

// 多向齐射：2 人 = 2 方向 × 7 发；4 人 = 4 方向 × 7 发（同 tick）。
void TestFanAttackCoversEveryDirection() {
    {
        CoopBossSimulation sim(MakeConfig(42));
        sim.AddPlayer("p-a", 0, 70200);
        sim.AddPlayer("p-b", 0, -70200);
        for (std::uint64_t tick = 1; tick <= 90; ++tick) {
            sim.Tick();
        }
        const CoopBossSnapshot snapshot = sim.Snapshot();
        Check(CountKind(snapshot, 0) == 14, "2p fan must emit 7 bullets per direction (14)");
    }
    {
        CoopBossSimulation sim(MakeConfig(42));
        sim.AddPlayer("p-a", 0, 70200);
        sim.AddPlayer("p-b", 70200, 0);
        sim.AddPlayer("p-c", 0, -70200);
        sim.AddPlayer("p-d", -70200, 0);
        for (std::uint64_t tick = 1; tick <= 90; ++tick) {
            sim.Tick();
        }
        const CoopBossSnapshot snapshot = sim.Snapshot();
        Check(CountKind(snapshot, 0) == 28, "4p fan must emit 7 bullets per direction (28)");
    }
}

// 方向格挡：窗口内被格挡方向的玩家伤害归零，其余方向照常。
void TestShieldBlocksOneOrThreeDirections() {
    for (std::int64_t seed = 1; seed <= 12; ++seed) {
        CoopBossSimulation sim(MakeConfig(seed));
        sim.AddPlayer("p-a", 0, 70200);
        sim.AddPlayer("p-b", 70200, 0);
        sim.AddPlayer("p-c", 0, -70200);
        sim.AddPlayer("p-d", -70200, 0);
        // 推进到格挡窗口（t % 600 ∈ [300,480)）。
        for (std::uint64_t tick = 1; tick <= 305; ++tick) {
            sim.Tick();
        }
        const std::uint32_t mask = sim.ShieldDirMask();
        Check(mask != 0, "shield window must block at least one direction");
        const std::size_t blocked = static_cast<std::size_t>(__builtin_popcount(mask));
        Check(blocked == 1 || blocked == 3, "shield must block exactly 1 or 3 directions");
        for (std::uint64_t tick = 306; tick <= 320; ++tick) {
            sim.SubmitInput(MakeInput("match_coop_000001", "p-a", tick, tick, true));
            sim.SubmitInput(MakeInput("match_coop_000001", "p-b", tick, tick, true));
            sim.SubmitInput(MakeInput("match_coop_000001", "p-c", tick, tick, true));
            sim.SubmitInput(MakeInput("match_coop_000001", "p-d", tick, tick, true));
            sim.Tick();
        }
        const CoopBossSnapshot snapshot = sim.Snapshot();
        for (const auto& player : snapshot.players) {
            if (player.shielded) {
                Check(player.damage_dealt == 0, "shielded direction must deal zero damage");
            } else {
                Check(player.damage_dealt > 0, "unshielded direction must deal damage");
            }
        }
    }
}

// 巨炮封锁：目标座位在封锁窗口内位移与射击冻结；他人不受影响。
void TestCannonLocksOneDirection() {
    bool locked_case_seen = false;
    for (std::int64_t seed = 1; seed <= 12 && !locked_case_seen; ++seed) {
        CoopBossSimulation sim(MakeConfig(seed));
        sim.AddPlayer("p-a", 0, 70200);
        sim.AddPlayer("p-b", 70200, 0);
        // 推进到巨炮窗口（t % 600 ∈ [450,600)）。
        for (std::uint64_t tick = 1; tick <= 455; ++tick) {
            sim.Tick();
        }
        const std::int32_t lock_dir = sim.LockDir();
        if (lock_dir < 0) continue;
        locked_case_seen = true;
        const CoopBossSnapshot before = sim.Snapshot();
        const std::string locked_id = before.players[static_cast<std::size_t>(lock_dir)].player_id;
        const std::string other_id = before.players[static_cast<std::size_t>(lock_dir) == 0 ? 1 : 0].player_id;
        const std::int32_t locked_x = before.players[static_cast<std::size_t>(lock_dir)].x_milli;
        const std::int32_t locked_y = before.players[static_cast<std::size_t>(lock_dir)].y_milli;
        for (std::uint64_t tick = 456; tick <= 470; ++tick) {
            sim.SubmitInput(MakeInput("match_coop_000001", locked_id, tick, tick, true, 0x2u));
            sim.SubmitInput(MakeInput("match_coop_000001", other_id, tick, tick, true, 0x2u));
            sim.Tick();
        }
        const CoopBossSnapshot after = sim.Snapshot();
        for (const auto& player : after.players) {
            if (player.player_id == locked_id) {
                Check(player.x_milli == locked_x && player.y_milli == locked_y, "locked player must not move");
                Check(player.damage_dealt == 0, "locked player must not shoot");
                Check(player.locked, "locked flag must be visible in the snapshot");
            } else {
                Check(player.x_milli != locked_x || player.damage_dealt > 0, "other player must stay active");
            }
        }
        Check(CountKind(after, 1) > 0, "cannon window must emit wide bullets");
        bool has_wide = false;
        for (const auto& bullet : after.bullets) {
            if (bullet.kind == 1 && bullet.radius_milli == 12000) has_wide = true;
        }
        Check(has_wide, "cannon bullets must use the wide radius");
    }
    Check(locked_case_seen, "at least one seed must produce a cannon lock");
}

// 全屏旋转：窗口内旋转弹持续发射，角度随 tick 推进。
void TestSpiralRotatesAcrossScreen() {
    CoopBossSimulation sim(MakeConfig(7));
    sim.AddPlayer("p-a", 0, 70200);
    sim.AddPlayer("p-b", 0, -70200);
    for (std::uint64_t tick = 1; tick <= 12; ++tick) {
        sim.Tick();
    }
    const CoopBossSnapshot early = sim.Snapshot();
    Check(sim.SpiralActive(), "spiral must be active inside its window");
    Check(CountKind(early, 2) > 0, "spiral must emit bullets");
    double first_angle = 0.0;
    bool have_first = false;
    for (const auto& bullet : early.bullets) {
        if (bullet.kind != 2) continue;
        if (!have_first) {
            first_angle = std::atan2(static_cast<double>(bullet.vy_milli), static_cast<double>(bullet.vx_milli));
            have_first = true;
        }
    }
    for (std::uint64_t tick = 13; tick <= 24; ++tick) {
        sim.Tick();
    }
    const CoopBossSnapshot late = sim.Snapshot();
    bool rotated = false;
    for (const auto& bullet : late.bullets) {
        if (bullet.kind != 2) continue;
        const double angle = std::atan2(static_cast<double>(bullet.vy_milli), static_cast<double>(bullet.vx_milli));
        if (have_first && std::abs(angle - first_angle) > 1e-6) rotated = true;
    }
    Check(rotated, "spiral direction must rotate over time");
}

// 确定性：同种子 + 同输入 → 逐字节相同的 state hash。
void TestDeterminismIsByteIdentical() {
    auto run = [](std::int64_t seed) {
        CoopBossSimulation sim(MakeConfig(seed));
        sim.AddPlayer("p-a", 0, 70200);
        sim.AddPlayer("p-b", 70200, 0);
        sim.AddPlayer("p-c", 0, -70200);
        for (std::uint64_t tick = 1; tick <= 240; ++tick) {
            sim.SubmitInput(MakeInput("match_coop_000001", "p-a", tick, tick, true, 0x1u));
            sim.SubmitInput(MakeInput("match_coop_000001", "p-b", tick, tick, true, 0x2u));
            sim.SubmitInput(MakeInput("match_coop_000001", "p-c", tick, tick, false, 0x8u));
            sim.Tick();
        }
        return sim.CanonicalStateHash();
    };
    Check(run(99) == run(99), "same seed + same inputs must produce the same state hash");
    Check(run(99) != run(100), "different seeds must diverge");
}

// Boss 阵亡 → 终局；合作共赢（无单人 winner）。
void TestBossDefeatFinishesMatch() {
    CoopBossSimulation sim(MakeConfig(5));
    sim.AddPlayer("p-a", 0, 70200);
    sim.AddPlayer("p-b", 0, -70200);
    // 2p：Boss HP = 10000；格挡窗口期只有 1 个方向能输出 ⇒ 需要跨过窗口才打完。
    for (std::uint64_t tick = 1; tick <= 1500; ++tick) {
        sim.SubmitInput(MakeInput("match_coop_000001", "p-a", tick, tick, true));
        sim.SubmitInput(MakeInput("match_coop_000001", "p-b", tick, tick, true));
        sim.Tick();
        if (sim.State() == CoopBossState::Finished) break;
    }
    const CoopBossSnapshot snapshot = sim.Snapshot();
    Check(sim.State() == CoopBossState::Finished, "boss defeat must finish the match");
    Check(snapshot.boss_hp == 0, "boss hp must reach zero");
    Check(snapshot.boss_max_hp == 10000, "boss hp must be 5000 per player");
}

// 输入校验拒绝路径。
void TestInputValidation() {
    CoopBossSimulation sim(MakeConfig(3));
    sim.AddPlayer("p-a", 0, 70200);
    sim.AddPlayer("p-b", 0, -70200);
    BattleInput zero_seq = MakeInput("match_coop_000001", "p-a", 1, 0, true);
    Check(!sim.SubmitInput(zero_seq), "seq 0 must be rejected");
    BattleInput unknown = MakeInput("match_coop_000001", "p-zzz", 1, 1, true);
    Check(!sim.SubmitInput(unknown), "unknown player must be rejected");
    BattleInput bad_bits = MakeInput("match_coop_000001", "p-a", 1, 1, true, 0x10u);
    Check(!sim.SubmitInput(bad_bits), "direction bits above 0x0f must be rejected");
    BattleInput wrong_match = MakeInput("match_other", "p-a", 1, 1, true);
    Check(!sim.SubmitInput(wrong_match), "foreign match id must be rejected");
    BattleInput good = MakeInput("match_coop_000001", "p-a", 1, 1, true);
    Check(sim.SubmitInput(good), "valid input must be accepted");
    Check(!sim.SubmitInput(good), "replayed seq must be rejected");
    sim.SetPlayerConnected("p-a", false);
    BattleInput after_disconnect = MakeInput("match_coop_000001", "p-a", 2, 2, true);
    Check(!sim.SubmitInput(after_disconnect), "disconnected player must be rejected");
}

}  // namespace

int main() {
    TestSeatAssignment();
    TestFanAttackCoversEveryDirection();
    TestShieldBlocksOneOrThreeDirections();
    TestCannonLocksOneDirection();
    TestSpiralRotatesAcrossScreen();
    TestDeterminismIsByteIdentical();
    TestBossDefeatFinishesMatch();
    TestInputValidation();

    if (g_failures != 0) {
        std::cerr << g_failures << " coop_boss test(s) failed\n";
        return 1;
    }
    std::cout << "coop_boss tests passed\n";
    return 0;
}
