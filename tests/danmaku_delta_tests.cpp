// 弹幕差量 wire 骨架测试（docs/danmaku-delta-wire.md §4 / §7 / §8）。
//
// 本轮是「能力位 + 可选字段 + 双端编解码」骨架：
//   1) delta_wire_enabled=false（默认）时快照 JSON 与旧版逐字节相同；
//   2) 打开时 wire_cap 与 §4 四类数组字段出现，且全量 bullets[] 仍完整；
//   3) bullet_spawns[].template 与 LiveBullet::motion 同型（danmaku::BulletMotion）
//      并逐字段镜像其值。
//
// ★ 本轮仍保留全量 bullets[] 兜底：去掉它需要 §7 的 golden/parity 全量回归
//   全部通过后才能做，故这里也钉住「delta 字段只在 JSON 收尾 '}' 之前追加」。
#include <cstdint>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

#include "phk/battle/boss_race.hpp"
#include "phk/battle/match_lifecycle.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (condition) {
        return;
    }
    std::cerr << "FAIL: " << message << "\n";
    ++g_failures;
}

std::string JsonOf(const std::vector<std::uint8_t>& payload) {
    // 首字节是 payload_type(0x04)，其后是手写 JSON。
    return std::string(payload.begin() + 1, payload.end());
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// ① 默认关闭：逐字节等于改动前的全量快照，且五个新字段一个都不出现。
void TestLegacyByteForByte() {
    phk::battle::BossRaceSnapshot snap;  // 全默认
    const std::vector<std::uint8_t> payload =
        phk::battle::EncodeBossRaceSnapshotPayload(snap);
    Check(!payload.empty(), "legacy payload is non-empty");
    Check(payload[0] == 4, "legacy payload keeps the 0x04 snapshot type byte");

    const std::string json = JsonOf(payload);
    const std::string golden =
        "{\"tick\":0,\"state\":\"waiting\",\"winner_player_id\":\"\",\"winner_tick\":0,"
        "\"difficulty\":1,\"state_hash\":\"\","
        "\"boss\":{\"x_milli\":0,\"y_milli\":0,\"hp\":0,\"max_hp\":0,\"phase\":0,"
        "\"movement\":{\"type\":\"static\",\"phase_t\":0},\"effects\":[]},"
        "\"players\":[],\"bullets\":[],\"chains\":[],\"hand\":[],"
        "\"deck_remaining\":0,\"draw_cooldown_ticks\":0,\"events\":[],\"event_cursor\":0}";
    Check(json == golden, "default (delta=nullptr) JSON is byte-for-byte identical to legacy");

    // 旧调用点（隐式默认参数）与显式 nullptr 必须等价。
    Check(payload == phk::battle::EncodeBossRaceSnapshotPayload(snap, nullptr),
          "implicit default arg equals explicit nullptr");

    for (const char* key : {"\"wire_cap\"", "\"bullet_spawns\"",
                            "\"bullet_deviations\"", "\"bullet_despawns\"",
                            "\"group_motion\""}) {
        Check(!Contains(json, key),
              std::string("legacy JSON must not carry delta field ") + key);
    }
}

// ② 打开时：wire_cap + 四类数组字段出现，全量 bullets[] 仍完整，且 delta
//    字段只在收尾 '}' 之前追加（主体逐字节不变）。
void TestDeltaFieldsAndFullBullets() {
    phk::battle::BossRaceSnapshot snap;
    snap.tick = 45;
    phk::battle::BossRaceBullet bullet;
    bullet.bullet_id = "b1";
    bullet.owner_player_id = "p1";
    bullet.x_milli = 1000;
    bullet.y_milli = -2000;
    bullet.vx_milli = 300;
    bullet.vy_milli = -400;
    bullet.radius_milli = 4000;
    bullet.pattern_id = "gap_ring";
    bullet.bullet_kind = 9;
    bullet.motion_func = 1;
    snap.bullets.push_back(bullet);

    phk::battle::BossRaceDeltaWire delta;
    phk::battle::BulletSpawnWire spawn;
    spawn.bullet_id = "b2";
    spawn.owner_player_id = "p1";
    spawn.t0 = 45;
    spawn.p = 3;
    spawn.q = 7;
    spawn.group_id = 0;
    delta.spawns.push_back(spawn);

    phk::battle::BulletDeviationWire deviation;
    deviation.bullet_id = "b1";
    deviation.dx = 120;
    deviation.dy = -40;
    deviation.dvx = 0;
    deviation.dvy = 0;
    delta.deviations.push_back(deviation);

    phk::battle::BulletDespawnWire despawn;
    despawn.bullet_id = "b9";
    despawn.reason = "spell_clear";
    delta.despawns.push_back(despawn);

    // G_g ≡ I：恒等值，不编造编队变换（规格 §2 / §8）。
    phk::battle::GroupMotionWire group;
    delta.group_motion.push_back(group);

    const std::vector<std::uint8_t> base =
        phk::battle::EncodeBossRaceSnapshotPayload(snap);
    const std::vector<std::uint8_t> aug =
        phk::battle::EncodeBossRaceSnapshotPayload(snap, &delta);
    const std::string baseJson = JsonOf(base);
    const std::string augJson = JsonOf(aug);

    Check(aug[0] == 4, "delta payload keeps the 0x04 snapshot type byte");
    // wire_cap = SPAWN(1) | DEVIATION(2) | GROUP(4) = 7，字段级真实反映三个
    // 已写出的数组字段（despawn 规格未分配位）。
    Check(Contains(augJson, "\"wire_cap\":7"), "wire_cap reflects encoded field classes");
    Check((phk::battle::kDeltaWireCapSpawn | phk::battle::kDeltaWireCapDeviation |
           phk::battle::kDeltaWireCapGroup) == 7u,
          "cap bit meanings are 1=SPAWN 2=DEVIATION 4=GROUP");
    Check(Contains(augJson, "\"bullet_spawns\":[{\"bullet_id\":\"b2\""),
          "bullet_spawns field present with its entry");
    Check(Contains(augJson, "\"template\":{"),
          "spawn carries the full BulletMotion template");
    Check(Contains(augJson,
                   "\"bullet_deviations\":[{\"bullet_id\":\"b1\",\"dx\":120,\"dy\":-40,"
                   "\"dvx\":0,\"dvy\":0}"),
          "bullet_deviations carries milli-integer deltas");
    Check(Contains(augJson,
                   "\"bullet_despawns\":[{\"bullet_id\":\"b9\",\"reason\":\"spell_clear\"}"),
          "bullet_despawns carries id + reason");
    Check(Contains(augJson,
                   "\"group_motion\":[{\"group_id\":0,\"tx\":0,\"ty\":0,\"phi\":0}"),
          "group_motion is the identity transform (G_g = I)");

    // delta 字段只在收尾 '}' 之前追加：去掉 base 的最后一个 '}' 后，其全部
    // 字节（含完整 bullets[]）必须是 aug 的前缀。
    const std::string prefix = baseJson.substr(0, baseJson.size() - 1);
    Check(augJson.size() > prefix.size() + 1, "delta payload is strictly longer");
    Check(augJson.compare(0, prefix.size(), prefix) == 0,
          "delta fields are appended only before the closing brace; body unchanged");
    Check(augJson.back() == '}', "delta payload still closes with '}'");

    Check(Contains(augJson,
                   "\"bullets\":[{\"bullet_id\":\"b1\",\"owner_player_id\":\"p1\""),
          "full bullets[] is still present as the fallback");
    Check(Contains(augJson, "\"x_milli\":1000"), "full bullet coordinates survive");
    Check(Contains(augJson, "\"pattern_id\":\"gap_ring\""),
          "full bullet pattern_id survives");
}

// ③ spawn.template 与 LiveBullet::motion 同型（danmaku::BulletMotion）并逐字段
//    镜像其值。LiveBullet 是 BossRaceSimulation 的私有内嵌结构，但其 motion
//    成员就是 danmaku::BulletMotion，故这里以类型 + 值双重断言钉住一致性。
void TestSpawnTemplateMirrorsBulletMotion() {
    static_assert(
        std::is_same_v<decltype(phk::battle::BulletSpawnWire::tmpl),
                       phk::battle::danmaku::BulletMotion>,
        "spawn.template must have the exact type of LiveBullet::motion");

    phk::battle::BossRaceSnapshot snap;
    phk::battle::BossRaceDeltaWire delta;
    phk::battle::BulletSpawnWire spawn;
    spawn.bullet_id = "b42";
    spawn.owner_player_id = "p1";
    spawn.t0 = 45;
    spawn.p = 3;
    spawn.q = 7;
    phk::battle::danmaku::BulletMotion& m = spawn.tmpl;
    // 用能被 %.17g 精确写成短串的值，避免浮点格式噪声干扰逐字段断言。
    m.x = 320;
    m.y = 120;
    m.angle = 90;
    m.speed = 3;
    m.angle_init = -1;
    m.spd_init2 = 2.5;
    m.xacc = 0.5;
    m.bullet_kind = 12;
    m.bullet_func_no = 7;
    m.hit_dis = 4;
    m.p1 = 3;
    m.p2 = 4;
    m.delay = 5;
    delta.spawns.push_back(spawn);

    const std::string json =
        JsonOf(phk::battle::EncodeBossRaceSnapshotPayload(snap, &delta));
    for (const char* field : {"\"x\":320", "\"y\":120", "\"angle\":90",
                              "\"speed\":3", "\"angle_init\":-1",
                              "\"spd_init2\":2.5", "\"xacc\":0.5",
                              "\"bullet_kind\":12", "\"bullet_func_no\":7",
                              "\"hit_dis\":4", "\"p1\":3", "\"p2\":4",
                              "\"delay\":5", "\"hit_hantei\":true"}) {
        Check(Contains(json, field),
              std::string("spawn template mirrors BulletMotion field ") + field);
    }
    Check(Contains(json, "\"t0\":45,\"p\":3,\"q\":7,\"group_id\":0"),
          "spawn four-tuple + group_id are encoded");
}

}  // namespace

int main() {
    TestLegacyByteForByte();
    TestDeltaFieldsAndFullBullets();
    TestSpawnTemplateMirrorsBulletMotion();

    if (g_failures != 0) {
        std::cerr << g_failures << " danmaku delta test(s) failed\n";
        return 1;
    }
    std::cout << "danmaku delta tests passed\n";
    return 0;
}
