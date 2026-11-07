// 弹幕差量 wire 测试（docs/danmaku-delta-wire.md §4 / §10）。
//
//   1) delta == nullptr（默认关闭）时快照 JSON 与旧版逐字节相同；
//   2) delta != nullptr（§10.2）时省略全量 bullets[]，只发 wire_cap +
//      四类数组字段，其余顶层键顺序与关闭时一致；
//   3) bullet_spawns[].template 与 LiveBullet::motion 同型并逐字段镜像；
//   4) ★ 真实 300 tick 弹幕对局（§10.4）：逐 tick 事件流与全量增删一致、
//      spawn 模板可经一帧递推重建出生 tick 的 x_milli/y_milli、两次运行
//      事件流 sha256 相同、delta/全量 JSON 键集合正确。不使用空快照或
//      构造的假数据。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "phk/battle/boss_race.hpp"
#include "phk/battle/dem.hpp"       // DemSha256Hex
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

// ② 打开时（§10.2）：省略全量 bullets[]，只发 wire_cap + 四类数组字段；
//    其余顶层键顺序与关闭时一致，delta 字段只在收尾 '}' 之前追加。
void TestDeltaFieldsOmitFullBullets() {
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
    Check(augJson.front() == '{' && augJson.back() == '}', "delta JSON is well-formed");
    // wire_cap = SPAWN(1) | DEVIATION(2) | GROUP(4) = 7（despawn 规格未分配位）。
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

    // §10.2：delta 模式不含 "bullets" 键；全量模式反之且不含 delta 字段。
    Check(!Contains(augJson, "\"bullets\""), "delta JSON omits the bullets key");
    Check(Contains(baseJson, "\"bullets\":[{\"bullet_id\":\"b1\""),
          "full JSON keeps the full bullets[]");
    Check(!Contains(baseJson, "\"wire_cap\""), "full JSON has no delta fields");
    Check(!Contains(augJson, "\"x_milli\":1000"),
          "full bullet coordinates are gone in delta mode");

    // 顶层键顺序：delta 输出 == 「bullets 为空的全量输出」删去 ,"bullets":[]
    // 后在收尾 '}' 之前追加 §4 空字段。逐字节钉住「仅省略 bullets、其余顺序不变」。
    phk::battle::BossRaceSnapshot empty_snap;
    const std::string empty_full = JsonOf(phk::battle::EncodeBossRaceSnapshotPayload(empty_snap));
    const std::string bullets_key = ",\"bullets\":[]";
    std::string body = empty_full;
    const std::size_t bp = body.find(bullets_key);
    Check(bp != std::string::npos, "empty full JSON has an empty bullets[]");
    if (bp != std::string::npos) {
        body.erase(bp, bullets_key.size());
    }
    const std::string delta_fields =
        ",\"wire_cap\":7,\"bullet_spawns\":[],\"bullet_deviations\":[],"
        "\"bullet_despawns\":[],\"group_motion\":[]";
    const std::string expected = body.substr(0, body.size() - 1) + delta_fields + "}";
    const phk::battle::BossRaceDeltaWire empty_delta_obj;
    const std::string empty_delta =
        JsonOf(phk::battle::EncodeBossRaceSnapshotPayload(empty_snap, &empty_delta_obj));
    Check(empty_delta == expected,
          "delta JSON = full JSON minus bullets[] plus §4 fields, key order preserved");
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

// ---- §10.4 真实 300 tick 弹幕对局验收 --------------------------------------
namespace pb = phk::battle;

// 不用空快照、不构造假数据：直接跑 BossRaceSimulation，两名脚本化玩家按
// apps/phk_danmaku_trace 的输入脚本射击/走位，逐 tick 取权威快照。
// 配置：match_seed=1、ruleset-local-s0、all_features_enabled=false（避免
// Boss 击破/玩家死亡导致提前 Finished——Finished 分支不清空 tick 容器、
// 会重复带上一 tick 事件，且保证严格 300 tick）。boss_id 为空 →
// chain_tables_active_=false → SpawnRealBulletsForTick（真实 tna-1.40 pattern 库）。
pb::BattleInput MakeScriptedInput(
    const std::string& player_id, std::uint64_t tick, bool shoot, bool bomb) {
    pb::BattleInput input;
    input.match_id = "match_delta_real_0001";
    input.player_id = player_id;
    input.tick = tick;
    input.seq = tick;
    input.shoot = shoot;
    input.bomb = bomb;
    // 确定性走位：每 32 tick 换一个方向（右→下→左→上循环），与 trace 工具一致。
    const std::uint32_t phase = static_cast<std::uint32_t>((tick / 32) % 4);
    switch (phase) {
        case 0: input.direction_bits = 0x2; break;
        case 1: input.direction_bits = 0x4; break;
        case 2: input.direction_bits = 0x8; break;
        default: input.direction_bits = 0x1; break;
    }
    return input;
}

std::vector<pb::BossRaceSnapshot> RunRealMatch() {
    pb::BossRaceConfig config;
    config.match_id = "match_delta_real_0001";
    config.match_seed = 1;
    config.ruleset_version = "ruleset-local-s0";
    config.all_features_enabled = false;
    pb::BossRaceSimulation simulation(config);
    simulation.AddPlayer("p-alpha", 0, 240000);
    simulation.AddPlayer("p-beta", 0, -240000);
    simulation.SetPlayerConnected("p-alpha", true);
    simulation.SetPlayerConnected("p-beta", true);
    std::vector<pb::BossRaceSnapshot> snapshots;
    snapshots.reserve(300);
    for (std::uint64_t tick = 1; tick <= 300; ++tick) {
        simulation.SubmitInput(MakeScriptedInput("p-alpha", tick, true, tick == 100));
        simulation.SubmitInput(MakeScriptedInput("p-beta", tick, true, false));
        snapshots.push_back(simulation.Tick());
    }
    return snapshots;
}

// 测试侧把快照事件流转换成编码器 delta（与生产 MatchServer::AdvanceTick 同构）。
pb::BossRaceDeltaWire MakeDeltaFromSnapshot(const pb::BossRaceSnapshot& snap) {
    pb::BossRaceDeltaWire delta;
    delta.spawns.reserve(snap.tick_spawns.size());
    for (const pb::BossRaceSpawnEvent& e : snap.tick_spawns) {
        pb::BulletSpawnWire w;
        w.bullet_id = e.bullet_id;
        w.owner_player_id = e.owner_player_id;
        w.t0 = e.t0;
        w.p = e.p;
        w.q = e.q;
        w.group_id = e.group_id;
        w.tmpl = e.tmpl;
        delta.spawns.push_back(std::move(w));
    }
    delta.despawns.reserve(snap.tick_despawns.size());
    for (const pb::BossRaceDespawnEvent& e : snap.tick_despawns) {
        pb::BulletDespawnWire w;
        w.bullet_id = e.bullet_id;
        w.reason = e.reason;
        delta.despawns.push_back(std::move(w));
    }
    return delta;
}

// ---- 事件流序列化（逐字段，含 spawn 模板），用于确定性 sha256 --------------
void AppendDouble17(std::string& s, double v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    s += buf;
    s += ',';
}

void AppendMotion(std::string& s, const pb::danmaku::BulletMotion& m) {
    AppendDouble17(s, m.x);
    AppendDouble17(s, m.y);
    AppendDouble17(s, m.vx);
    AppendDouble17(s, m.vy);
    AppendDouble17(s, m.angle);
    AppendDouble17(s, m.speed);
    AppendDouble17(s, m.accel);
    AppendDouble17(s, m.t);
    s += std::to_string(m.life) + ',';
    AppendDouble17(s, m.angle_spd);
    AppendDouble17(s, m.angle_acc);
    AppendDouble17(s, m.f56c);
    AppendDouble17(s, m.speed_cap);
    AppendDouble17(s, m.angle_init);
    AppendDouble17(s, m.speed_init);
    AppendDouble17(s, m.f598);
    AppendDouble17(s, m.prev_angle);
    AppendDouble17(s, m.spd_init2);
    AppendDouble17(s, m.xacc);
    AppendDouble17(s, m.xacc_cap);
    AppendDouble17(s, m.pf1);
    AppendDouble17(s, m.pf2);
    s += std::to_string(m.bullet_kind) + ',';
    s += std::to_string(m.rotate_flg) + ',';
    s += std::to_string(m.bullet_life) + ',';
    s += std::to_string(m.bullet_loop) + ',';
    s += std::to_string(m.delay) + ',';
    s += std::to_string(m.vanish_delay) + ',';
    s += std::to_string(m.p1) + ',';
    s += std::to_string(m.p2) + ',';
    s += std::to_string(m.bullet_func_no) + ',';
    AppendDouble17(s, m.hit_dis);
    s += m.hit_hantei ? "true," : "false,";
}

std::string SerializeEventStream(const std::vector<pb::BossRaceSnapshot>& snaps) {
    std::string s;
    for (const pb::BossRaceSnapshot& snap : snaps) {
        s += "T" + std::to_string(snap.tick) + "|";
        for (const pb::BossRaceSpawnEvent& e : snap.tick_spawns) {
            s += "S:" + e.bullet_id + ":" + e.owner_player_id + ":" +
                 std::to_string(e.t0) + ":" + std::to_string(e.p) + ":" +
                 std::to_string(e.q) + ":" + std::to_string(e.group_id) + ":";
            AppendMotion(s, e.tmpl);
            s += ';';
        }
        for (const pb::BossRaceDespawnEvent& e : snap.tick_despawns) {
            s += "D:" + e.bullet_id + ":" + e.reason + ";";
        }
    }
    return s;
}

// ---- spawn 模板重建出生 tick 的 x_milli/y_milli ----------------------------
//
// 复刻 src/boss_race.cpp:1104-1126（AdvanceBullets 第一趟）：spawn 发生时
// 模板是「未推进」状态，服务端在同 tick 的 AdvanceBullets 里对这颗新弹
// 也做了一次 UpdateBulletMotion + pos += v + t += 1，故出生 tick 的快照
// 位置 = 模板递推一帧后的位置（与 §10.3 客户端重建器一致）。
// 常量与 src/boss_race.cpp:22-33 同值（其常量在匿名 namespace，测试不可见）。
constexpr double kCenterXPx = 320.0;
constexpr double kCenterYPx = 480.0;
constexpr double kMilliPerPx = 1000.0;

std::int32_t ToMilliPx(double pixels) {
    return static_cast<std::int32_t>(std::llround(pixels * kMilliPerPx));
}

void ReconstructBirthPosition(
    const pb::BossRaceSpawnEvent& spawn,
    const pb::BossRaceSnapshot& snap,
    std::int32_t* out_x_milli,
    std::int32_t* out_y_milli) {
    pb::danmaku::BulletMotion m = spawn.tmpl;
    pb::danmaku::MotionTargets targets;
    for (const pb::BossRacePlayerSnapshot& p : snap.players) {
        if (p.player_id == spawn.owner_player_id) {
            targets.has_my_ship = true;
            targets.my_ship_x = kCenterXPx + static_cast<double>(p.x_milli) / kMilliPerPx;
            targets.my_ship_y = kCenterYPx + static_cast<double>(p.y_milli) / kMilliPerPx;
        }
    }
    targets.has_enemy = true;
    targets.enemy_x = kCenterXPx + static_cast<double>(snap.boss.x_milli) / kMilliPerPx;
    targets.enemy_y = kCenterYPx + static_cast<double>(snap.boss.y_milli) / kMilliPerPx;
    // rng 输入与服务端完全一致：(match_seed, tick, pattern_index, spawn_index)。
    pb::danmaku::DeterministicRng rng(1, spawn.t0, spawn.p, spawn.q);
    pb::danmaku::UpdateBulletMotion(m, targets, rng);
    m.x += m.vx;
    m.y += m.vy;
    m.t += 1.0;
    *out_x_milli = ToMilliPx(m.x - kCenterXPx);
    *out_y_milli = ToMilliPx(m.y - kCenterYPx);
}

// ★ 真实 300 tick 弹幕对局：逐 tick 断言事件流与全量增删一致、模板重建、
//   确定性 sha256、delta/全量 JSON 键集合。
void TestRealDanmakuDeltaWire() {
    const std::vector<pb::BossRaceSnapshot> snaps = RunRealMatch();
    Check(snaps.size() == 300, "real match ran exactly 300 ticks");
    if (snaps.size() != 300) {
        return;
    }

    std::size_t total_spawns = 0;
    std::size_t total_despawns = 0;
    std::size_t max_bullets = 0;
    std::size_t reconstruct_checks = 0;
    std::set<std::string> prev_ids;

    for (const pb::BossRaceSnapshot& snap : snaps) {
        total_spawns += snap.tick_spawns.size();
        total_despawns += snap.tick_despawns.size();
        max_bullets = std::max(max_bullets, snap.bullets.size());

        std::set<std::string> curr_ids;
        std::map<std::string, std::pair<std::int32_t, std::int32_t>> pos;
        for (const pb::BossRaceBullet& b : snap.bullets) {
            curr_ids.insert(b.bullet_id);
            pos[b.bullet_id] = {b.x_milli, b.y_milli};
        }
        std::set<std::string> spawn_ids;
        std::set<std::string> despawn_ids;
        for (const pb::BossRaceSpawnEvent& e : snap.tick_spawns) {
            spawn_ids.insert(e.bullet_id);
        }
        for (const pb::BossRaceDespawnEvent& e : snap.tick_despawns) {
            despawn_ids.insert(e.bullet_id);
        }

        std::set<std::string> added;
        std::set<std::string> removed;
        std::set_difference(curr_ids.begin(), curr_ids.end(),
                            prev_ids.begin(), prev_ids.end(),
                            std::inserter(added, added.end()));
        std::set_difference(prev_ids.begin(), prev_ids.end(),
                            curr_ids.begin(), curr_ids.end(),
                            std::inserter(removed, removed.end()));
        const std::string tag = "tick " + std::to_string(snap.tick) + ": ";
        Check(spawn_ids == added,
              tag + "spawn id set == full bullets[] added id set");
        Check(despawn_ids == removed,
              tag + "despawn id set == full bullets[] removed id set");

        // 重建自洽：(prev - despawn) ∪ spawn == curr。
        std::set<std::string> rebuilt = prev_ids;
        for (const std::string& id : despawn_ids) {
            rebuilt.erase(id);
        }
        for (const std::string& id : spawn_ids) {
            rebuilt.insert(id);
        }
        Check(rebuilt == curr_ids,
              tag + "applying events reconstructs current bullet id set");

        // spawn 模板按 Snapshot() 同一换算重建出生 tick 的 x_milli/y_milli。
        for (const pb::BossRaceSpawnEvent& e : snap.tick_spawns) {
            const auto it = pos.find(e.bullet_id);
            if (it == pos.end()) {
                continue;  // 同 tick 又 despawn：真实对局不触发，跳过位置比较
            }
            std::int32_t x = 0;
            std::int32_t y = 0;
            ReconstructBirthPosition(e, snap, &x, &y);
            Check(x == it->second.first && y == it->second.second,
                  tag + "spawn " + e.bullet_id +
                  " template reconstructs birth-tick x_milli/y_milli");
            ++reconstruct_checks;
        }
        prev_ids = curr_ids;
    }

    Check(total_spawns > 0, "real match produced spawn events (not empty snapshots)");
    Check(total_despawns > 0, "real match produced despawn events");
    Check(max_bullets > 0, "real match produced live bullets");
    Check(reconstruct_checks > 0, "spawn template reconstruction was exercised");
    std::cout << "real match: ticks=" << snaps.size()
              << " spawns=" << total_spawns
              << " despawns=" << total_despawns
              << " max_bullets=" << max_bullets
              << " template_reconstructions=" << reconstruct_checks << "\n";

    // 确定性：同参两次运行，事件流逐字节 + sha256 相同。
    const std::vector<pb::BossRaceSnapshot> snaps2 = RunRealMatch();
    const std::string stream1 = SerializeEventStream(snaps);
    const std::string stream2 = SerializeEventStream(snaps2);
    Check(stream1 == stream2, "two runs produce byte-identical event stream");
    const std::string digest1 = pb::DemSha256Hex(stream1);
    const std::string digest2 = pb::DemSha256Hex(stream2);
    Check(digest1 == digest2, "two runs produce identical event stream sha256");
    Check(digest1.size() == 64, "event stream sha256 is 64 hex chars");

    // delta / 全量 JSON 键集合：取一个既有真实 bullets 又有 spawn 的 tick。
    std::size_t pick = 0;
    for (std::size_t i = 0; i < snaps.size(); ++i) {
        if (!snaps[i].bullets.empty() && !snaps[i].tick_spawns.empty()) {
            pick = i;
            break;
        }
    }
    const pb::BossRaceSnapshot& sample = snaps[pick];
    const pb::BossRaceDeltaWire delta = MakeDeltaFromSnapshot(sample);
    const std::string delta_json =
        JsonOf(pb::EncodeBossRaceSnapshotPayload(sample, &delta));
    const std::string full_json =
        JsonOf(pb::EncodeBossRaceSnapshotPayload(sample));
    Check(!Contains(delta_json, "\"bullets\""), "delta JSON omits bullets key");
    Check(Contains(delta_json, "\"wire_cap\":7"), "delta JSON has wire_cap");
    Check(Contains(delta_json, "\"bullet_spawns\""), "delta JSON has bullet_spawns");
    Check(Contains(delta_json, "\"bullet_despawns\""), "delta JSON has bullet_despawns");
    Check(Contains(full_json, "\"bullets\""), "full JSON has bullets key");
    Check(!Contains(full_json, "\"wire_cap\""), "full JSON has no delta fields");
    Check(Contains(delta_json, "],\"chains\":"),
          "delta JSON keeps players[] -> chains key order (bullets omitted)");
}

}  // namespace

int main() {
    TestLegacyByteForByte();
    TestDeltaFieldsOmitFullBullets();
    TestSpawnTemplateMirrorsBulletMotion();
    TestRealDanmakuDeltaWire();

    if (g_failures != 0) {
        std::cerr << g_failures << " danmaku delta test(s) failed\n";
        return 1;
    }
    std::cout << "danmaku delta tests passed\n";
    return 0;
}
