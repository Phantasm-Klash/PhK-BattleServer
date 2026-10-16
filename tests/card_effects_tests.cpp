// 卡牌效果执行层测试——镜像 Gensoulkyo/runtime/core/card_effects_test.go
// 并扩展（五人局确定性对手、半径外子弹、子弹封顶）。
//
// 金标准来源：
//   - purge 选取集与 fan 速度由 Python 复刻 Go 算法（sha256 deterministicUnit
//     + atan2 几何 + Go round）在服务器上生成，与本文件内嵌常量逐项比对。
//   - invulnerable 窗口语义：simulation.go:203 守卫 tick > InvulnerableUntil
//     才受伤，即 InvulnerableUntil = tick+180 恰好在第 181 tick 失效。
//
// 版权：东方同人逆向成果，仅用于自研玩法。

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "phk/battle/card_effects.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& label) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << label << "\n";
    }
}

using phk::battle::ApplyBulletPurge;
using phk::battle::ApplyFinisher;
using phk::battle::ApplyInvulnerable;
using phk::battle::ApplyOpponentPattern;
using phk::battle::CardActivation;
using phk::battle::CardBulletDelta;
using phk::battle::CardBulletSnapshot;
using phk::battle::CardPlayerSnapshot;
using phk::battle::OpponentTargetId;

bool Near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

// 环状布种（对应 Go 测试 seedBulletsAroundPlayer：角度 2πi/count，半径 60）。
std::vector<CardBulletSnapshot> RingBullets(
    const std::string& id_prefix,
    double center_x,
    double center_y,
    int count,
    double ring_radius
) {
    std::vector<CardBulletSnapshot> bullets;
    bullets.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double angle = (2.0 * 3.14159265358979323846 * static_cast<double>(i)) / static_cast<double>(count);
        CardBulletSnapshot bullet;
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), "%03d", i);
        bullet.bullet_id = id_prefix + suffix;
        bullet.pattern_id = "test_seed";
        bullet.kind = "ring";
        bullet.x = center_x + std::cos(angle) * ring_radius;
        bullet.y = center_y + std::sin(angle) * ring_radius;
        bullet.radius = 4.5;
        bullet.damage = 1;
        bullet.color = "white";
        bullets.push_back(bullet);
    }
    return bullets;
}

// ---- opponent_pattern ----

// 镜像 TestCardEffectOpponentPatternTargetsOpponent（扩为五人局，验证
// opponentTargetID 取「除自己外的最低 user id」）。速度金标准 = Python。
void TestOpponentPatternTargetsLowestOpponent() {
    const std::vector<CardPlayerSnapshot> players = {
        {"u-alice", 480.0, 600.0},  // 施法者
        {"u-bob", 600.0, 500.0},    // 目标（除 alice 外最低 id）
        {"u-carol", 300.0, 400.0},
        {"u-dave", 100.0, 200.0},
        {"u-eve", 500.0, 300.0},
    };
    CardActivation activation;
    activation.card_id = "mirror_lance";
    activation.effect_kind = "opponent_pattern";
    activation.pattern_id = "mirror_lance";

    const auto result = ApplyOpponentPattern(players, "u-alice", activation, 1, "m1", 1, 0);

    Check(result.has_target, "fan: expected a target");
    Check(result.target_user_id == "u-bob", "fan: target must be lowest id u-bob, got " + result.target_user_id);
    Check(result.event_value == 7, "fan: event_value must be 7");
    Check(result.bullet_deltas.size() == 7u, "fan: expected 7 spawn deltas");

    // Python 金标准（seed 场景：目标 (600,500)，boss 原点 = 竞技场中心 (0,0)）。
    // 效果层按 Go 的 round(v, 4) 把速度四舍五入到 4 位小数（探针实测值）。
    const double expected[7][2] = {
        {4.9270, 0.8515}, {4.7042, 1.6941}, {4.3386, 2.4853}, {3.8411, 3.2009},
        {3.2269, 3.8193}, {2.5147, 4.3216}, {1.7260, 4.6926},
    };
    for (int i = 0; i < 7; ++i) {
        const CardBulletDelta& delta = result.bullet_deltas[static_cast<std::size_t>(i)];
        const std::string label = "fan[" + std::to_string(i) + "]";
        Check(delta.op == "spawn", label + ": op spawn");
        Check(delta.bullet_id == "m1_b00000" + std::to_string(i + 1), label + ": bullet id, got " + delta.bullet_id);
        Check(delta.pattern_id == "mirror_lance", label + ": pattern");
        Check(delta.kind == "nway", label + ": kind");
        Check(Near(delta.x, phk::battle::kBossOriginX) && Near(delta.y, phk::battle::kBossOriginY), label + ": arena boss origin");
        Check(Near(delta.vx, expected[i][0], 1e-9), label + ": vx");
        Check(Near(delta.vy, expected[i][1], 1e-9), label + ": vy");
        Check(Near(delta.radius, 4.5), label + ": radius");
        Check(delta.damage == 1, label + ": damage");
        Check(delta.color == "violet", label + ": color");
        Check(delta.tick == 1, label + ": tick");
    }
}

// 无对手（单人局）：Go 提前返回——无子弹、无 target、value=0。
void TestOpponentPatternNoOpponent() {
    const std::vector<CardPlayerSnapshot> players = {{"solo", 480.0, 600.0}};
    CardActivation activation;
    activation.card_id = "mirror_lance";
    activation.effect_kind = "opponent_pattern";
    activation.pattern_id = "mirror_lance";

    const auto result = ApplyOpponentPattern(players, "solo", activation, 5, "m1", 1, 0);

    Check(result.bullet_deltas.empty(), "solo fan: no deltas");
    Check(!result.has_target, "solo fan: no target");
    Check(result.event_value == 0, "solo fan: value 0");
}

// 子弹封顶（simulation.go:150 maxBullets=384）：满场时一弹不生。
void TestOpponentPatternBulletCap() {
    const std::vector<CardPlayerSnapshot> players = {
        {"u-alice", 480.0, 600.0},
        {"u-bob", 600.0, 500.0},
    };
    CardActivation activation;
    activation.card_id = "mirror_lance";
    activation.effect_kind = "opponent_pattern";
    activation.pattern_id = "mirror_lance";

    const auto result = ApplyOpponentPattern(players, "u-alice", activation, 1, "m1", 100, 384);

    Check(result.bullet_deltas.empty(), "cap: no spawns at maxBullets");
    Check(result.event_value == 0, "cap: value 0");
}

// ---- bullet_purge ----

// 镜像 TestCardEffectBulletPurgeRemovesDeterministicSelection：30 颗环布
// 子弹（半径 60 < 120 全在范围内），seed=424242/tick=1/purge_bell，
// 恰好清除 24 颗；选取集金标准 = Python 复刻 deterministicUnit 选取。
void TestBulletPurgeRemovesDeterministicSelection() {
    const CardPlayerSnapshot caster{"u-alice", 480.0, 600.0};
    const std::vector<CardBulletSnapshot> bullets = RingBullets("m_b", 480.0, 600.0, 30, 60.0);

    CardActivation activation;
    activation.card_id = "purge_bell";
    activation.effect_kind = "bullet_purge";
    activation.purge_radius = 120.0;
    activation.purge_count = 24;

    const auto result = ApplyBulletPurge(bullets, caster, activation, 424242, 1);

    Check(result.event_value == 24, "purge: value must be 24, got " + std::to_string(result.event_value));
    Check(result.bullet_deltas.size() == 24u, "purge: expected 24 despawn deltas");

    // Python 金标准：按 (unit 升序, bullet id 升序) 的清除顺序。
    const char* expected_order[24] = {
        "m_b018", "m_b001", "m_b002", "m_b026", "m_b013", "m_b005", "m_b006", "m_b003",
        "m_b004", "m_b012", "m_b019", "m_b022", "m_b021", "m_b009", "m_b029", "m_b028",
        "m_b008", "m_b015", "m_b016", "m_b011", "m_b017", "m_b010", "m_b027", "m_b020",
    };
    for (int i = 0; i < 24; ++i) {
        const CardBulletDelta& delta = result.bullet_deltas[static_cast<std::size_t>(i)];
        Check(
            delta.op == "despawn" && delta.bullet_id == expected_order[i],
            "purge[" + std::to_string(i) + "]: expected " + expected_order[i] + ", got " + delta.op + " " + delta.bullet_id
        );
    }

    // 存活集（金标准）。
    const char* expected_remain[6] = {"m_b000", "m_b007", "m_b014", "m_b023", "m_b024", "m_b025"};
    for (const char* id : expected_remain) {
        bool purged = false;
        for (const CardBulletDelta& delta : result.bullet_deltas) {
            if (delta.bullet_id == id) {
                purged = true;
            }
        }
        Check(!purged, std::string("purge: ") + id + " must survive");
    }
}

// 空场 / 半径为 0 / 计数为 0：全部无操作（Go radius>0 && count>0 守卫）。
void TestBulletPurgeNoOp() {
    const CardPlayerSnapshot caster{"u-alice", 480.0, 600.0};
    CardActivation activation;
    activation.card_id = "purge_bell";
    activation.effect_kind = "bullet_purge";
    activation.purge_radius = 120.0;
    activation.purge_count = 24;

    const auto empty = ApplyBulletPurge({}, caster, activation, 424242, 1);
    Check(empty.bullet_deltas.empty() && empty.event_value == 0, "purge empty: no-op");

    activation.purge_radius = 0.0;
    const auto no_radius = ApplyBulletPurge(RingBullets("m_b", 480.0, 600.0, 5, 10.0), caster, activation, 424242, 1);
    Check(no_radius.bullet_deltas.empty() && no_radius.event_value == 0, "purge radius 0: no-op");

    activation.purge_radius = 120.0;
    activation.purge_count = 0;
    const auto no_count = ApplyBulletPurge(RingBullets("m_b", 480.0, 600.0, 5, 10.0), caster, activation, 424242, 1);
    Check(no_count.bullet_deltas.empty() && no_count.event_value == 0, "purge count 0: no-op");
}

// 半径外子弹不参选（注意：规范下标仍跨全场排序序列计数）。
void TestBulletPurgeOutsideRadius() {
    const CardPlayerSnapshot caster{"u-alice", 480.0, 600.0};
    std::vector<CardBulletSnapshot> bullets;
    // 一颗贴脸（应被清）+ 一颗 300 单位外（不应被清）。
    CardBulletSnapshot near;
    near.bullet_id = "a-near";
    near.x = 480.0;
    near.y = 640.0;  // 距施法者 40 < 120
    bullets.push_back(near);
    CardBulletSnapshot far;
    far.bullet_id = "b-far";
    far.x = 480.0;
    far.y = 600.0 + 300.0;  // 距施法者 300 > 120
    bullets.push_back(far);

    CardActivation activation;
    activation.card_id = "purge_bell";
    activation.effect_kind = "bullet_purge";
    activation.purge_radius = 120.0;
    activation.purge_count = 24;

    const auto result = ApplyBulletPurge(bullets, caster, activation, 424242, 1);

    Check(result.event_value == 1, "purge outside: exactly 1 removed, got " + std::to_string(result.event_value));
    Check(result.bullet_deltas.size() == 1u && result.bullet_deltas[0].bullet_id == "a-near", "purge outside: only a-near");
}

// ---- invulnerable ----

// 镜像 TestCardEffectInvulnerableGrantsDamageImmunity 的窗口断言：
// tick=1 出牌 → InvulnerableUntil = 1+180 = 181；守卫 tick > 181 才受伤，
// 即免疫覆盖 tick 1..181，第 182 tick 失效（窗口后恰好一跳）。
void TestInvulnerableWindow() {
    CardActivation activation;
    activation.card_id = "aegis_ward";
    activation.effect_kind = "invulnerable";
    activation.expires_tick = 1 + 180;

    const auto result = ApplyInvulnerable(activation, 1);

    Check(result.invulnerable_until == 181, "invuln: InvulnerableUntil must be 181");
    Check(result.event_value == 0, "invuln: event value 0");
    Check(result.bullet_deltas.empty(), "invuln: no bullet deltas");
}

// ---- finisher ----

// 镜像 TestCardEffectFinisherClearsFieldAndDamagesBoss 的清屏部分：
// 12 颗种子子弹全部清除（despawn 顺序 = bullet id 字典序），value=12；
// boss 伤害由通用激活路径施加，本层结果不含任何伤害语义（不重复施加）。
void TestFinisherClearsField() {
    const std::vector<CardBulletSnapshot> bullets = RingBullets("m_b", 480.0, 352.0, 12, 80.0);
    const auto result = ApplyFinisher(bullets, 1);

    Check(result.event_value == 12, "finisher: value must be 12");
    Check(result.bullet_deltas.size() == 12u, "finisher: expected 12 despawn deltas");
    for (std::size_t i = 0; i < result.bullet_deltas.size(); ++i) {
        const CardBulletDelta& delta = result.bullet_deltas[i];
        const std::string expected_id = std::string("m_b") + (i < 10 ? "00" : "0") + std::to_string(i);
        Check(delta.op == "despawn", "finisher: op despawn");
        Check(delta.bullet_id == expected_id, "finisher: id order, expected " + expected_id + " got " + delta.bullet_id);
    }
}

// 空场清屏：无操作。
void TestFinisherEmptyField() {
    const auto result = ApplyFinisher({}, 7);
    Check(result.bullet_deltas.empty() && result.event_value == 0, "finisher empty: no-op");
}

// ---- 辅助函数 ----

void TestOpponentTargetIdHelper() {
    Check(OpponentTargetId({"a", "b", "c"}, "b") == "a", "target: lowest other");
    Check(OpponentTargetId({"b", "a", "c"}, "b") == "a", "target: unordered input");
    Check(OpponentTargetId({"solo"}, "solo").empty(), "target: no opponents");
}

}  // namespace

int main() {
    TestOpponentPatternTargetsLowestOpponent();
    TestOpponentPatternNoOpponent();
    TestOpponentPatternBulletCap();
    TestBulletPurgeRemovesDeterministicSelection();
    TestBulletPurgeNoOp();
    TestBulletPurgeOutsideRadius();
    TestInvulnerableWindow();
    TestFinisherClearsField();
    TestFinisherEmptyField();
    TestOpponentTargetIdHelper();

    if (g_failures != 0) {
        std::cerr << g_failures << " card_effects test(s) failed\n";
        return 1;
    }
    std::cout << "card_effects tests passed\n";
    return 0;
}
