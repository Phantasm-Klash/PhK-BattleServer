// Tests for the H-family chain split (bulletFunc31 Boss 分裂/召唤):
//   * 纯函数层：子子弹参数由种子推导（同种子逐位一致、换种子必变）、
//     几何/数量/spawn_index 派生公式、max_children 钳制、请求判据只认 func31。
//   * 集成层（BossRaceSimulation + InjectLiveBulletForTest）：同种子两次
//     运行 state hash 一致、1→8→64 链式结构真实发生、单 tick 每玩家分裂
//     宿主预算、叶子代不再分裂、每玩家 384 总量上限仍然生效、
//     默认 pattern 路径（无 func31）逐字节不受影响。

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "phk/battle/boss_race.hpp"
#include "phk/battle/danmaku/bullet_motion.hpp"
#include "phk/battle/danmaku/chain_split.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& label) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << label << "\n";
    }
}

using phk::battle::BossRaceConfig;
using phk::battle::BossRaceSimulation;
using phk::battle::BattleInput;
using phk::battle::danmaku::BulletMotion;
using phk::battle::danmaku::ChainSplitChild;
using phk::battle::danmaku::ComputeChainSplitChildren;
using phk::battle::danmaku::DeterministicRng;
using phk::battle::danmaku::MakeChainSplitChildMotion;

// 一颗 func31 宿主：p1 = 触发帧；位置屏幕中心；速度 0.5 px/tick（40 tick
// 内飞不出界，保证计数稳定）。
BulletMotion MakeSplitHost(double p1, double angle_deg = 0.0) {
    BulletMotion b;
    b.x = 320.0;
    b.y = 480.0;
    b.angle = angle_deg;
    b.speed = 0.5;
    b.bullet_kind = 45;   // kome_01，任意真实弹种
    b.bullet_func_no = 31;
    b.p1 = static_cast<int>(p1);
    b.p2 = 0;
    b.hit_dis = 5.0;
    b.IntegrateVelocity();
    return b;
}

BossRaceConfig MakeConfig(std::uint64_t seed, std::uint64_t boss_hp = 1000000) {
    BossRaceConfig config;
    config.match_id = "match_chain_000001";
    config.match_seed = seed;
    config.boss_max_hp = boss_hp;
    return config;
}

BattleInput MakeInput(
    const std::string& match_id,
    const std::string& player_id,
    std::uint64_t tick,
    std::uint64_t seq
) {
    BattleInput input;
    input.match_id = match_id;
    input.player_id = player_id;
    input.tick = tick;
    input.seq = seq;
    input.shoot = false;
    input.direction_bits = 0;
    return input;
}

// ---------------------------------------------------------------------------
// 纯函数层
// ---------------------------------------------------------------------------

void TestChildrenAreSeedDerived() {
    const BulletMotion host = MakeSplitHost(1.0);
    const std::vector<ChainSplitChild> a =
        ComputeChainSplitChildren(host, 111111u, 2u, 0u, 0u, 8);
    const std::vector<ChainSplitChild> again =
        ComputeChainSplitChildren(host, 111111u, 2u, 0u, 0u, 8);
    Check(a.size() == again.size() && a.size() == 8, "same seed yields eight children");
    bool identical = a.size() == again.size();
    for (std::size_t i = 0; i < a.size() && i < again.size(); ++i) {
        if (a[i].angle_deg != again[i].angle_deg || a[i].speed != again[i].speed ||
            a[i].spawn_index != again[i].spawn_index) {
            identical = false;
        }
    }
    Check(identical, "same (seed,tick,pattern,spawn) reproduces children bit-for-bit");

    // 换种子必须改变参数集合（否则种子没进入推导）。
    const std::vector<ChainSplitChild> other =
        ComputeChainSplitChildren(host, 222222u, 2u, 0u, 0u, 8);
    bool differs = other.size() != a.size();
    for (std::size_t i = 0; i < a.size() && i < other.size() && !differs; ++i) {
        if (a[i].angle_deg != other[i].angle_deg || a[i].speed != other[i].speed) {
            differs = true;
        }
    }
    Check(differs, "different seed changes derived child parameters");

    // 换 tick / pattern / spawn_index 同样改变序列（RNG 四元组全部入料）。
    const std::vector<ChainSplitChild> other_tick =
        ComputeChainSplitChildren(host, 111111u, 3u, 0u, 0u, 8);
    Check(other_tick[0].angle_deg != a[0].angle_deg, "tick participates in the derivation");
}

void TestChildGeometryAndSpawnIndex() {
    const BulletMotion host = MakeSplitHost(1.0, 30.0);
    const std::vector<ChainSplitChild> children =
        ComputeChainSplitChildren(host, 987654321u, 2u, 0u, 0u, 8);
    Check(children.size() == 8, "eight children per host by default");

    constexpr double kStep = phk::battle::danmaku::kChainSplitAngleStepDeg;
    constexpr double kJitter = phk::battle::danmaku::kChainSplitAngleJitterDeg;
    constexpr double kLo = phk::battle::danmaku::kChainSplitChildSpeedLo;
    constexpr double kHi = phk::battle::danmaku::kChainSplitChildSpeedHi;
    for (int i = 0; i < 8; ++i) {
        const ChainSplitChild& child = children[static_cast<std::size_t>(i)];
        // 角度 = host + i×45° + (u-0.5)×8°：相邻差落在 45±8 内。
        const double prev = children[static_cast<std::size_t>((i + 7) % 8)].angle_deg;
        double delta = child.angle_deg - prev;
        while (delta < 0.0) delta += 360.0;
        while (delta >= 360.0) delta -= 360.0;
        // 相邻差 = 45 + (jitter_i - jitter_{i-1})，每侧 ±jitter/2 → 界 ±jitter。
        Check(std::fabs(delta - kStep) <= kJitter + 1e-9,
              "child " + std::to_string(i) + " keeps the 45-degree ring cadence");
        // 速度 ∈ [0.6,1.0] × host.speed。
        Check(child.speed >= kLo * host.speed - 1e-12 && child.speed <= kHi * host.speed + 1e-12,
              "child " + std::to_string(i) + " speed inside the factor band");
        // spawn_index = host_spawn_index + base + ordinal。
        Check(child.spawn_index == phk::battle::danmaku::kChainSplitSpawnIndexBase +
                  static_cast<std::uint32_t>(i),
              "child " + std::to_string(i) + " spawn_index derived from host + base + ordinal");
    }

    // 子子弹 motion：位置=宿主、func/p1 继承、t/life/p2 归零、速度入快照。
    const BulletMotion motion = MakeChainSplitChildMotion(host, children[0]);
    Check(motion.x == host.x && motion.y == host.y, "child spawns at the host position");
    Check(motion.bullet_func_no == 31 && motion.p1 == host.p1, "child inherits func31 and the trigger tick");
    Check(motion.bullet_kind == host.bullet_kind, "child inherits the bullet kind");
    Check(motion.t == 0.0 && motion.life == 0 && motion.p2 == 0, "child timers start clean");
    Check(motion.speed == children[0].speed && motion.spd_init2 == children[0].speed,
          "child carries the derived speed into vx/vy");
    Check(std::fabs(motion.vx - children[0].speed * std::cos(children[0].angle_deg * M_PI / 180.0)) < 1e-9,
          "child vx matches angle/speed");
}

void TestRequestPredicateAndCaps() {
    // 只认 func31：Func42/Func55 形态（p2=1 但 func_no≠31）不得触发。
    BulletMotion other_family = MakeSplitHost(1.0);
    other_family.bullet_func_no = 42;
    other_family.p2 = 1;
    Check(!phk::battle::danmaku::ChainSplitRequested(other_family),
          "func42 p2 flag does not request a split");
    other_family.bullet_func_no = 55;
    Check(!phk::battle::danmaku::ChainSplitRequested(other_family),
          "func55 p2 flag does not request a split");
    BulletMotion real = MakeSplitHost(1.0);
    real.p2 = 1;
    Check(phk::battle::danmaku::ChainSplitRequested(real), "func31 p2 flag requests a split");
    real.p2 = 0;
    Check(!phk::battle::danmaku::ChainSplitRequested(real), "no flag, no split");

    // max_children 钳制：0/3 颗；RNG 按颗粒序号取用，前 3 颗与不钳制时一致。
    const BulletMotion host = MakeSplitHost(1.0);
    Check(ComputeChainSplitChildren(host, 42u, 2u, 0u, 0u, 0).empty(), "zero budget yields no child");
    const std::vector<ChainSplitChild> full =
        ComputeChainSplitChildren(host, 42u, 2u, 0u, 0u, 8);
    const std::vector<ChainSplitChild> clipped =
        ComputeChainSplitChildren(host, 42u, 2u, 0u, 0u, 3);
    Check(clipped.size() == 3, "max_children clamps the host count");
    bool prefix_matches = clipped.size() <= full.size();
    for (std::size_t i = 0; i < clipped.size() && prefix_matches; ++i) {
        if (clipped[i].angle_deg != full[i].angle_deg || clipped[i].speed != full[i].speed) {
            prefix_matches = false;
        }
    }
    Check(prefix_matches, "clipped children are the unclipped prefix (RNG is per-ordinal)");

    // 宿主未置请求标记时上层不得放行（判定在 AdvanceBullets 第一趟，
    // 由 ChainSplitRequested 把关；纯函数Compute 只对已判定的宿主调用）。
    const BulletMotion idle = MakeSplitHost(1.0);
    Check(!phk::battle::danmaku::ChainSplitRequested(idle),
          "host without the func31 flag never reaches the split pass");
}

// ---------------------------------------------------------------------------
// 集成层（BossRaceSimulation）
// ---------------------------------------------------------------------------

// 两个同 seed 对局注入同构宿主后逐 tick 推进，state hash 必须一致。
void TestSimulationDeterministic() {
    const std::uint64_t seed = 20261022u;
    std::vector<std::string> hashes_a;
    std::vector<std::string> hashes_b;
    for (int run = 0; run < 2; ++run) {
        BossRaceSimulation sim(MakeConfig(seed));
        Check(sim.AddPlayer("player_a", -20000, 60000), "add player_a");
        Check(sim.AddPlayer("player_b", 20000, 60000), "add player_b");
        // 每玩家注入 3 颗 gen0 宿主（p1=2：tick 2 触发第一代）。
        for (int i = 0; i < 3; ++i) {
            BulletMotion host = MakeSplitHost(2.0, static_cast<double>(i) * 30.0);
            Check(sim.InjectLiveBulletForTest("player_a", host, 0), "inject host a");
            Check(sim.InjectLiveBulletForTest("player_b", host, 0), "inject host b");
        }
        std::vector<std::string>& hashes = run == 0 ? hashes_a : hashes_b;
        std::uint64_t seq = 0;
        for (std::uint64_t tick = 1; tick <= 40; ++tick) {
            Check(sim.SubmitInput(MakeInput(sim.Config().match_id, "player_a", tick, ++seq)), "input a");
            Check(sim.SubmitInput(MakeInput(sim.Config().match_id, "player_b", tick, ++seq)), "input b");
            hashes.push_back(sim.Tick().state_hash);
        }
    }
    Check(hashes_a.size() == hashes_b.size(), "both runs produced a hash per tick");
    bool equal = hashes_a.size() == hashes_b.size();
    for (std::size_t i = 0; i < hashes_a.size() && i < hashes_b.size() && equal; ++i) {
        if (hashes_a[i] != hashes_b[i]) {
            equal = false;
            std::cerr << "FAIL: hash divergence at tick " << (i + 1)
                      << " (" << hashes_a[i] << " vs " << hashes_b[i] << ")\n";
        }
    }
    Check(equal, "identical seeds reproduce the chain-split state hash for all 40 ticks");
}

// 1 → 8 → 64 链式结构真实发生；叶子代不再产弹。
void TestSimulationChainShape() {
    BossRaceSimulation sim(MakeConfig(777u));
    Check(sim.AddPlayer("player_a", 0, 60000), "add player_a");
    Check(sim.AddPlayer("player_b", 0, 60000), "add player_b");
    // 单宿主：tick 2 分裂 8 颗一代，tick 4 每颗再分裂 8 颗二代 → 1+8+64=73。
    Check(sim.InjectLiveBulletForTest("player_a", MakeSplitHost(1.0), 0), "inject single host");

    std::size_t ticks_with_growth_after_gen2 = 0;
    std::size_t last_size = 0;
    std::uint64_t seq = 0;
    for (std::uint64_t tick = 1; tick <= 40; ++tick) {
        Check(sim.SubmitInput(MakeInput(sim.Config().match_id, "player_a", tick, ++seq)), "input a");
        Check(sim.SubmitInput(MakeInput(sim.Config().match_id, "player_b", tick, ++seq)), "input b");
        const auto snapshot = sim.Tick();
        if (tick >= 5) {
            if (snapshot.bullets.size() > last_size) {
                ++ticks_with_growth_after_gen2;
            }
        }
        last_size = snapshot.bullets.size();
    }
    // player_b 无注入、45 tick 前无主发射；player_a = 1+8+64 = 73。
    Check(last_size == 73u, "single host produced the full 1+8+64 chain, got " + std::to_string(last_size));
    Check(ticks_with_growth_after_gen2 == 0, "leaf generation stops the chain (no growth after tick 4)");
}

// 单 tick 每玩家分裂宿主预算（8）：32 颗同时到点的宿主只分裂前 8 个；
// 且每玩家 384 总量上限与注入拒绝路径仍然生效。
void TestSimulationDensityCaps() {
    BossRaceSimulation sim(MakeConfig(31337u));
    Check(sim.AddPlayer("player_a", 0, 60000), "add player_a");
    Check(sim.AddPlayer("player_b", 0, 60000), "add player_b");
    for (int i = 0; i < 32; ++i) {
        BulletMotion host = MakeSplitHost(1.0, static_cast<double>(i) * 11.0);
        Check(sim.InjectLiveBulletForTest("player_a", host, 0), "inject host batch");
    }
    // tick 2：32 个宿主全部到点，但每玩家预算 8 → 只产 8×8 = 64 颗一代。
    // tick 4：8 个一代宿主再分裂 → 64 颗二代。总量 32+64+64 = 160。
    std::size_t max_single_tick_growth = 0;
    std::size_t previous = 32;
    std::uint64_t seq = 0;
    for (std::uint64_t tick = 1; tick <= 40; ++tick) {
        Check(sim.SubmitInput(MakeInput(sim.Config().match_id, "player_a", tick, ++seq)), "input a");
        Check(sim.SubmitInput(MakeInput(sim.Config().match_id, "player_b", tick, ++seq)), "input b");
        const std::size_t size = sim.Tick().bullets.size();
        if (size > previous) {
            max_single_tick_growth = std::max(max_single_tick_growth, size - previous);
        }
        previous = size;
    }
    Check(previous == 160u, "per-tick host budget caps the batch at 160 total, got " + std::to_string(previous));
    Check(max_single_tick_growth <= 64u,
          "single-tick growth stayed within 8 hosts x 8 children, got " + std::to_string(max_single_tick_growth));

    // 每玩家 384 上限：注入至拒绝为止，总数恰为 384（分裂与注入共享同一上限）。
    BossRaceSimulation cap_sim(MakeConfig(4242u));
    Check(cap_sim.AddPlayer("player_a", 0, 60000), "add player_a for cap");
    Check(cap_sim.AddPlayer("player_b", 0, 60000), "add player_b for cap");
    int injected = 0;
    while (cap_sim.InjectLiveBulletForTest("player_a", MakeSplitHost(999.0), 0)) {
        ++injected;
    }
    Check(injected == 384, "per-player cap admits exactly 384 bullets, got " + std::to_string(injected));
    Check(!cap_sim.InjectLiveBulletForTest("player_a", MakeSplitHost(999.0), 0),
          "injection beyond the per-player cap is refused");
}

// 叶子代宿主（generation == kChainSplitMaxGeneration）不得再分裂。
void TestSimulationLeafGeneration() {
    BossRaceSimulation sim(MakeConfig(5150u));
    Check(sim.AddPlayer("player_a", 0, 60000), "add player_a");
    Check(sim.AddPlayer("player_b", 0, 60000), "add player_b");
    Check(sim.InjectLiveBulletForTest(
              "player_a", MakeSplitHost(1.0),
              phk::battle::danmaku::kChainSplitMaxGeneration),
          "inject leaf-generation host");
    std::uint64_t seq = 0;
    std::size_t final_size = 0;
    for (std::uint64_t tick = 1; tick <= 20; ++tick) {
        Check(sim.SubmitInput(MakeInput(sim.Config().match_id, "player_a", tick, ++seq)), "input a");
        Check(sim.SubmitInput(MakeInput(sim.Config().match_id, "player_b", tick, ++seq)), "input b");
        final_size = sim.Tick().bullets.size();
    }
    Check(final_size == 1u, "leaf host never spawns children, got " + std::to_string(final_size));
}

// 回归：默认 pattern 路径（无 func31 注入）行为与链式分裂落地前一致
// ——两个同 seed 对局 hash 一致，且子弹全部来自 pattern 库。
void TestSimulationDefaultPathUnchanged() {
    const std::uint64_t seed = 60606u;
    std::string hash_a;
    std::string hash_b;
    std::size_t bullets_seen = 0;
    for (int run = 0; run < 2; ++run) {
        BossRaceSimulation sim(MakeConfig(seed));
        Check(sim.AddPlayer("player_a", -20000, 60000), "add player_a");
        Check(sim.AddPlayer("player_b", 20000, 60000), "add player_b");
        std::uint64_t seq = 0;
        for (std::uint64_t tick = 1; tick <= 90; ++tick) {
            Check(sim.SubmitInput(MakeInput(sim.Config().match_id, "player_a", tick, ++seq)), "input a");
            Check(sim.SubmitInput(MakeInput(sim.Config().match_id, "player_b", tick, ++seq)), "input b");
            const auto snapshot = sim.Tick();
            if (run == 1) {
                bullets_seen += snapshot.bullets.size();
            }
        }
        if (run == 0) {
            hash_a = sim.CanonicalStateHash();
        } else {
            hash_b = sim.CanonicalStateHash();
        }
    }
    Check(hash_a == hash_b, "default path stays deterministic without any injected host");
    Check(bullets_seen > 0, "default path still emits pattern bullets");
}

}  // namespace

int main() {
    TestChildrenAreSeedDerived();
    TestChildGeometryAndSpawnIndex();
    TestRequestPredicateAndCaps();
    TestSimulationDeterministic();
    TestSimulationChainShape();
    TestSimulationDensityCaps();
    TestSimulationLeafGeneration();
    TestSimulationDefaultPathUnchanged();

    if (g_failures != 0) {
        std::cerr << g_failures << " danmaku chain test(s) failed\n";
        return 1;
    }
    std::cout << "danmaku chain tests passed\n";
    return 0;
}
