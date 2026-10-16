// 出牌执行器 tick 集成测试（T3 step4）。
//
// 全部经**真实入口**（AcceptInput / AcceptModeAction + Tick()），不直测
// card_hand/card_effects 纯函数——本文件验的是「校验后的出牌意图在 tick 内
// 被执行」这条接线：
//   - 输入通道（Go 同构）：真实客户端输入的 card_slot >= 0 → 执行；
//     -1 = 不出牌哨兵（无事件行、无状态变更、无子弹）。
//   - 动作通道：cast_card mode action → ApplyModeActionsForTick 内执行。
// 拒绝路径记 card_rejected；通过记 card_accepted + card_effect_applied。
//
// 事件行只进 CardCastEvents() 收集器，不进 event_trace_/event_stream_hash_
// /event_count_（见 task-queue/out/T3_step4_integration.md §5）。
//
// 版权：东方同人逆向成果，仅用于自研玩法。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "phk/battle/card_events_report.hpp"
#include "phk/battle/lobby_rng.hpp"  // HmacSha256Hex (T6 签名)
#include "phk/battle/server.hpp"
#include "phk/battle/simulation.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& label) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << label << "\n";
    }
}

using phk::battle::BattleInput;
using phk::battle::BattleModeAction;
using phk::battle::BattleServer;
using phk::battle::BattleSimulation;
using phk::battle::SimulationConfig;

constexpr const char* kMatchId = "match-cast-001";

SimulationConfig DuelConfig() {
    // 非 boss 模式：battle_input_enabled 恒真，无需 ready 流程。
    SimulationConfig config;
    config.match_id = kMatchId;
    config.mode_id = "duel";
    config.match_seed = 777;
    config.spawn_period_ticks = 5;
    config.max_bullets = 256;
    return config;
}

BattleInput MakeInput(
    const std::string& player_id,
    std::uint64_t tick,
    std::uint64_t seq,
    std::int32_t card_slot
) {
    BattleInput input;
    input.match_id = kMatchId;
    input.player_id = player_id;
    input.tick = tick;
    input.seq = seq;
    input.direction_bits = 0;
    input.slow = false;
    input.shoot = false;
    input.card_slot = card_slot;  // -1 = 不出牌哨兵（struct default）
    return input;
}

BattleModeAction MakeCastAction(
    const std::string& player_id,
    std::uint64_t seq,
    std::uint64_t tick,
    int slot
) {
    BattleModeAction action;
    action.match_id = kMatchId;
    action.player_id = player_id;
    action.tick = tick;
    action.seq = seq;
    action.action_id = "cast-" + player_id + "-" + std::to_string(seq);
    action.action_type = "cast_card";
    action.payload_json = "{\"card_slot\":" + std::to_string(slot) + "}";
    return action;
}

// 行查找助手：返回第一个指定类型的行指针（无则 nullptr）。
const phk::battle::CardCastEvent* FindEvent(
    const BattleSimulation& simulation,
    const std::string& event_type
) {
    for (const auto& row : simulation.CardCastEvents()) {
        if (row.event_type == event_type) {
            return &row;
        }
    }
    return nullptr;
}

std::size_t CountEvents(const BattleSimulation& simulation, const std::string& event_type) {
    std::size_t count = 0;
    for (const auto& row : simulation.CardCastEvents()) {
        if (row.event_type == event_type) {
            ++count;
        }
    }
    return count;
}

// ---- 播种 + -1 哨兵（输入通道） ----

void TestSeedAndSentinel() {
    BattleSimulation simulation(DuelConfig());
    Check(simulation.AddPlayer("p1", -20000, 0), "add p1");
    Check(simulation.AddPlayer("p2", 20000, 0), "add p2");
    Check(
        simulation.SeedPlayerHand(
            "p1",
            {"mirror_lance", "focus_lens", "focus_lens", "focus_lens"},
            10.0,
            "1234567890123456789"
        ),
        "seed p1 hand (seed > 2^53 as decimal text)"
    );
    Check(simulation.HandSize("p1") == 4, "hand size seeded to 4");
    Check(simulation.PlayerEnergy("p1") == 10.0, "energy seeded to 10");
    Check(simulation.LobbySeed() == 1234567890123456789LL, "lobby seed parsed from decimal text");
    Check(simulation.HandSize("p2") == 0, "p2 unseeded hand empty");
    Check(
        !simulation.SeedPlayerHand("p9", {"focus_lens"}, 1.0, "5"),
        "seeding unknown player rejected"
    );
    Check(
        !simulation.SeedPlayerHand("p2", {}, 1.0, "999"),
        "conflicting match seed rejected (first wins)"
    );
    Check(simulation.LobbySeed() == 1234567890123456789LL, "lobby seed unchanged after conflict");

    // -1 哨兵：真实输入、本 tick 不出牌 → 无事件行、无状态变更、无子弹。
    const auto before_bullets = simulation.BulletCount();
    Check(simulation.AcceptInput(MakeInput("p1", 1, 1, -1)).ok, "accept sentinel input");
    simulation.Tick();
    Check(simulation.CardCastEvents().empty(), "sentinel produces no event rows");
    Check(simulation.HandSize("p1") == 4, "sentinel leaves hand untouched");
    Check(simulation.PlayerEnergy("p1") == 10.0, "sentinel leaves energy untouched");
    Check(simulation.BulletCount() == before_bullets, "sentinel spawns no bullets");

    // 显式 cast_card 动作的 -1 在传输层即被拒（到不了执行器）。
    const auto sentinel_action = simulation.AcceptModeAction(MakeCastAction("p1", 2, 2, -1));
    Check(!sentinel_action.ok, "cast_card action with slot -1 rejected at transport");
    Check(sentinel_action.reason == "cast_card_slot_invalid", "reject reason cast_card_slot_invalid");
    Check(simulation.CardCastEvents().empty(), "transport-rejected action produces no rows");
}

// ---- 四张效果卡：动作通道合法 cast ----

void TestMirrorLanceCastViaModeAction() {
    BattleSimulation simulation(DuelConfig());
    Check(simulation.AddPlayer("p1", -20000, 0), "add p1");
    Check(simulation.AddPlayer("p2", 20000, 0), "add p2");
    Check(
        simulation.SeedPlayerHand(
            "p1",
            {"mirror_lance", "focus_lens", "focus_lens", "focus_lens"},
            10.0,
            "42"
        ),
        "seed p1 hand"
    );

    Check(simulation.AcceptModeAction(MakeCastAction("p1", 1, 1, 0)).ok, "accept cast slot 0");
    simulation.Tick();

    const auto accepted = FindEvent(simulation, "card_accepted");
    Check(accepted != nullptr, "card_accepted row emitted");
    if (accepted != nullptr) {
        Check(accepted->player_id == "p1", "card_accepted player");
        Check(accepted->card_id == "mirror_lance", "card_accepted card id");
        Check(accepted->event_value == 26, "card_accepted value = card damage (26)");
        Check(accepted->slot == 0, "card_accepted slot = 0");
        Check(accepted->tick == 1, "card_accepted tick = 1");
    }
    const auto effect = FindEvent(simulation, "card_effect_applied");
    Check(effect != nullptr, "card_effect_applied row emitted");
    if (effect != nullptr) {
        Check(effect->card_id == "mirror_lance", "effect row card id");
        Check(effect->event_value == 7, "opponent_pattern spawned 7 (event_value)");
        Check(effect->target_user_id == "p2", "opponent_pattern targets lowest other id");
    }
    // 坐标回归：效果层必须用竞技场空间的 boss 原点 (0,0)。照搬 Go 的
    // (480,112) 时子弹落在 x_milli=480000，越界被同 tick 剔除，这里会是 0。
    Check(
        simulation.BulletCount() == 7,
        "opponent_pattern bullets survive the tick (arena-space boss origin)"
    );
    Check(simulation.HandSize("p1") == 3, "played slot removed (deck empty: no draw)");
    Check(simulation.PlayerEnergy("p1") == 6.0, "energy 10 - cost 4");
    Check(CountEvents(simulation, "card_rejected") == 0, "no rejection on legal cast");
}

void TestPurgeBellCastViaInputChannel() {
    BattleSimulation simulation(DuelConfig());
    Check(simulation.AddPlayer("p1", 0, 0), "add p1 at origin");
    Check(simulation.AddPlayer("p2", 20000, 0), "add p2");
    Check(
        simulation.SeedPlayerHand("p1", {"purge_bell", "focus_lens", "focus_lens", "focus_lens"}, 10.0, "7"),
        "seed p1 hand"
    );

    // tick 5：SpawnBulletsForTick 在 (0,0) 附近生成 4 颗 duel 子弹。
    simulation.Tick();  // tick 1..4 无生成（spawn_period_ticks=5）
    simulation.Tick();
    simulation.Tick();
    simulation.Tick();
    simulation.Tick();
    const std::size_t spawned = simulation.BulletCount();
    Check(spawned == 4, "4 bullets spawned at tick 5");
    Check(
        simulation.BulletCount() >= 4,
        "purge preconditions: bullets on field"
    );

    // 输入通道：真实输入 card_slot=0 出 purge_bell（caster 在 (0,0)，全在半径内）。
    Check(simulation.AcceptInput(MakeInput("p1", 6, 1, 0)).ok, "accept cast input slot 0");
    simulation.Tick();  // tick 6 执行

    const auto accepted = FindEvent(simulation, "card_accepted");
    Check(accepted != nullptr && accepted->card_id == "purge_bell", "card_accepted purge_bell");
    const auto effect = FindEvent(simulation, "card_effect_applied");
    Check(effect != nullptr, "card_effect_applied row emitted");
    if (effect != nullptr) {
        Check(effect->event_value == static_cast<std::int64_t>(spawned), "purge removed all in-radius bullets");
    }
    Check(simulation.BulletCount() == 0, "field cleared by purge");
    Check(simulation.PlayerEnergy("p1") == 7.0, "energy 10 - cost 3");
    Check(simulation.HandSize("p1") == 3, "played slot removed");
}

void TestAegisWardCastViaModeAction() {
    BattleSimulation simulation(DuelConfig());
    Check(simulation.AddPlayer("p1", -20000, 0), "add p1");
    Check(simulation.AddPlayer("p2", 20000, 0), "add p2");
    Check(
        simulation.SeedPlayerHand("p1", {"aegis_ward", "focus_lens", "focus_lens", "focus_lens"}, 10.0, "9"),
        "seed p1 hand"
    );

    Check(simulation.AcceptModeAction(MakeCastAction("p1", 1, 1, 0)).ok, "accept cast slot 0");
    simulation.Tick();

    const auto effect = FindEvent(simulation, "card_effect_applied");
    Check(effect != nullptr && effect->card_id == "aegis_ward", "card_effect_applied aegis_ward");
    Check(effect != nullptr && effect->event_value == 0, "invulnerable event_value = 0");
    Check(
        simulation.PlayerInvulnerableUntil("p1") == 1 + 180,
        "invulnerable_until = tick + duration (181)"
    );
    Check(simulation.PlayerEnergy("p1") == 5.0, "energy 10 - cost 5");
}

void TestFinaleArcClearsField() {
    BattleSimulation simulation(DuelConfig());
    Check(simulation.AddPlayer("p1", 0, 0), "add p1");
    Check(simulation.AddPlayer("p2", 20000, 0), "add p2");
    Check(
        simulation.SeedPlayerHand("p1", {"finale_arc", "focus_lens", "focus_lens", "focus_lens"}, 10.0, "11"),
        "seed p1 hand"
    );

    for (int i = 0; i < 5; ++i) {
        simulation.Tick();  // tick 5 生成 4 颗子弹
    }
    const std::int64_t field = static_cast<std::int64_t>(simulation.BulletCount());
    Check(field == 4, "4 bullets on field before finisher");

    // 出牌 tick = 6。
    Check(simulation.AcceptModeAction(MakeCastAction("p1", 1, 6, 0)).ok, "accept cast at tick 6");
    simulation.Tick();

    const auto effect = FindEvent(simulation, "card_effect_applied");
    Check(effect != nullptr && effect->card_id == "finale_arc", "card_effect_applied finale_arc");
    Check(effect != nullptr && effect->event_value == field, "finisher cleared every bullet (event_value = field)");
    Check(simulation.BulletCount() == 0, "field cleared by finisher");
    Check(simulation.PlayerEnergy("p1") == 4.0, "energy 10 - cost 6");
}

// ---- 拒绝路径（每条例断 card_rejected 行 + 状态零变更） ----

void TestRejectionPaths() {
    // 未知卡（目录外 id 由票据校验保证不会下发，执行器仍须拒绝）。
    {
        BattleSimulation simulation(DuelConfig());
        Check(simulation.AddPlayer("p1", 0, 0), "add p1");
        Check(simulation.SeedPlayerHand("p1", {"not_a_card"}, 10.0, "3"), "seed unknown card hand");
        Check(simulation.AcceptModeAction(MakeCastAction("p1", 1, 1, 0)).ok, "accept cast");
        simulation.Tick();
        const auto rejected = FindEvent(simulation, "card_rejected");
        Check(rejected != nullptr, "unknown card -> card_rejected");
        Check(
            rejected != nullptr && rejected->reason == "cast_card_unknown_card",
            "unknown card reason"
        );
        Check(simulation.HandSize("p1") == 1, "rejected cast leaves hand untouched");
        Check(simulation.PlayerEnergy("p1") == 10.0, "rejected cast leaves energy untouched");
        Check(CountEvents(simulation, "card_accepted") == 0, "no card_accepted on rejection");
    }
    // slot 越界（手牌 1 张，slot 3）。
    {
        BattleSimulation simulation(DuelConfig());
        Check(simulation.AddPlayer("p1", 0, 0), "add p1");
        Check(simulation.SeedPlayerHand("p1", {"focus_lens"}, 10.0, "3"), "seed hand size 1");
        Check(simulation.AcceptModeAction(MakeCastAction("p1", 1, 1, 3)).ok, "accept cast slot 3");
        simulation.Tick();
        const auto rejected = FindEvent(simulation, "card_rejected");
        Check(
            rejected != nullptr && rejected->reason == "cast_card_slot_out_of_range",
            "slot out of range reason"
        );
        Check(simulation.HandSize("p1") == 1, "rejected cast leaves hand untouched");
    }
    // 空手牌（slot 0 亦越界）。
    {
        BattleSimulation simulation(DuelConfig());
        Check(simulation.AddPlayer("p1", 0, 0), "add p1");
        Check(simulation.SeedPlayerHand("p1", {}, 10.0, "3"), "seed empty hand");
        Check(simulation.AcceptModeAction(MakeCastAction("p1", 1, 1, 0)).ok, "accept cast slot 0");
        simulation.Tick();
        const auto rejected = FindEvent(simulation, "card_rejected");
        Check(
            rejected != nullptr && rejected->reason == "cast_card_slot_out_of_range",
            "empty hand -> out of range"
        );
    }
    // 冷却中（同卡二次出牌）。
    {
        BattleSimulation simulation(DuelConfig());
        Check(simulation.AddPlayer("p1", 0, 0), "add p1");
        Check(simulation.AddPlayer("p2", 20000, 0), "add p2");
        Check(
            simulation.SeedPlayerHand("p1", {"mirror_lance", "mirror_lance"}, 10.0, "3"),
            "seed two mirror_lance"
        );
        Check(simulation.AcceptModeAction(MakeCastAction("p1", 1, 1, 0)).ok, "accept first cast");
        simulation.Tick();
        Check(FindEvent(simulation, "card_accepted") != nullptr, "first cast accepted");
        simulation.ClearCardCastEvents();
        Check(simulation.AcceptModeAction(MakeCastAction("p1", 2, 2, 0)).ok, "accept second cast");
        simulation.Tick();
        const auto rejected = FindEvent(simulation, "card_rejected");
        Check(
            rejected != nullptr && rejected->reason == "cast_card_on_cooldown",
            "second cast on cooldown"
        );
    }
    // 能量不足。
    {
        BattleSimulation simulation(DuelConfig());
        Check(simulation.AddPlayer("p1", 0, 0), "add p1");
        Check(
            simulation.SeedPlayerHand("p1", {"mirror_lance"}, 0.0, "3"),
            "seed zero energy"
        );
        Check(simulation.AcceptModeAction(MakeCastAction("p1", 1, 1, 0)).ok, "accept cast");
        simulation.Tick();
        const auto rejected = FindEvent(simulation, "card_rejected");
        Check(
            rejected != nullptr && rejected->reason == "cast_card_insufficient_energy",
            "insufficient energy reason"
        );
        Check(simulation.HandSize("p1") == 1, "rejected cast leaves hand untouched");
    }
}

// ---- boss 伤害与 boss_defeated 行（lobby 已识别并路由 draft） ----

void TestBossDamageAndDefeatRow() {
    SimulationConfig config;
    config.match_id = kMatchId;
    config.mode_id = "world_boss";
    config.match_seed = 555;
    config.boss_max_hp = 100;  // finale_arc(60) + last_arc(48) 足够击杀
    config.spawn_period_ticks = 60;
    BattleSimulation simulation(config);
    for (int index = 1; index <= 4; ++index) {
        Check(
            simulation.AddPlayer("p" + std::to_string(index), 0, 0),
            "add boss player p" + std::to_string(index)
        );
    }
    for (int index = 1; index <= 4; ++index) {
        BattleModeAction ready;
        ready.match_id = kMatchId;
        ready.player_id = "p" + std::to_string(index);
        ready.tick = 1;
        ready.seq = static_cast<std::uint64_t>(index);
        ready.action_id = "ready-" + std::to_string(index);
        ready.action_type = "ready";
        ready.payload_json = "{\"ready\":true}";
        Check(simulation.AcceptModeAction(ready).ok, "accept ready p" + std::to_string(index));
    }
    // 两张不同的 finisher（冷却按 card_id 独立，可连出）：
    // finale_arc 60 伤害 / last_arc 48 伤害。
    Check(
        simulation.SeedPlayerHand("p1", {"finale_arc", "last_arc"}, 10.0, "31337"),
        "seed p1 finisher hand"
    );

    simulation.Tick();  // tick 1：ready 生效，boss 战斗开始
    const auto summary1 = simulation.Summary();
    Check(summary1.boss_current_hp == 100, "boss full hp after ready tick");

    // 第一击（输入通道，tick 2）：finale_arc 60 伤害，boss 剩 40。
    // 注意：p1 的 last_seq 已被 tick 1 的 ready 动作推进到 1，输入 seq 必须 > 1。
    Check(simulation.AcceptInput(MakeInput("p1", 2, 2, 0)).ok, "accept cast input at tick 2");
    simulation.Tick();  // tick 2
    const auto summary2 = simulation.Summary();
    Check(summary2.boss_current_hp == 40, "boss hp 100 - 60 = 40");
    Check(summary2.boss_damage_total == 60, "boss damage total 60");
    Check(CountEvents(simulation, "boss_defeated") == 0, "no boss_defeated yet");

    // 第二击（tick 3）：重新播种手牌+能量（模拟下一轮能量回复；冷却按
    // card_id 独立，last_arc 未冷却）后出 last_arc，48 伤害 clamp 到剩余
    // 40 → 击杀。
    Check(
        simulation.SeedPlayerHand("p1", {"last_arc"}, 10.0, "31337"),
        "re-seed p1 with last_arc"
    );
    Check(simulation.AcceptInput(MakeInput("p1", 3, 3, 0)).ok, "accept second cast input at tick 3");
    simulation.Tick();  // tick 3：执行第二击
    const auto summary3 = simulation.Summary();
    Check(summary3.boss_current_hp == 0, "boss hp clamped to 0");
    Check(summary3.boss_damage_total == 100, "damage clamped to remaining hp");
    Check(summary3.boss_defeated == 1, "boss defeated flag");
    Check(summary3.boss_defeated_tick == 3, "boss defeated tick = 3");
    const auto defeated = FindEvent(simulation, "boss_defeated");
    Check(defeated != nullptr, "boss_defeated row emitted");
    if (defeated != nullptr) {
        Check(defeated->reason.find("world-boss:") == 0, "boss_defeated reason carries boss instance id");
        Check(defeated->tick == 3, "boss_defeated tick");
        Check(defeated->player_id == "p1", "boss_defeated player");
    }
    bool trace_has_defeat = false;
    for (const auto& line : summary3.event_trace) {
        if (line.find("boss_defeated|") == 0) {
            trace_has_defeat = true;
        }
    }
    Check(trace_has_defeat, "boss_defeated also in replay event trace");
}


// ---- Step 2：payload 构造与回传（H-1） ----

// 捕获一个 HTTP 请求（request line / headers / body / 计数），响应 200。
class CapturingHttpServer {
public:
    CapturingHttpServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return;
        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return;
        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        ::listen(listen_fd_, 4);
        thread_ = std::thread([this] { Serve(); });
    }

    ~CapturingHttpServer() { Stop(); }

    std::uint16_t port() const { return port_; }

    std::string request_line() {
        std::lock_guard<std::mutex> lock(mutex_);
        return request_line_;
    }

    std::string header(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : headers_) {
            if (entry.first.size() == name.size() &&
                std::equal(
                    entry.first.begin(), entry.first.end(), name.begin(),
                    [](char a, char b) {
                        return std::tolower(static_cast<unsigned char>(a)) ==
                            std::tolower(static_cast<unsigned char>(b));
                    })) {
                return entry.second;
            }
        }
        return "";
    }

    std::string body() {
        std::lock_guard<std::mutex> lock(mutex_);
        return body_;
    }

    std::size_t request_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return request_count_;
    }

    void Stop() {
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

private:
    void Serve() {
        for (;;) {
            const int client = ::accept(listen_fd_, nullptr, nullptr);
            if (client < 0) return;

            std::string request;
            char buffer[4096];
            std::size_t header_end = std::string::npos;
            std::size_t content_length = 0;
            for (;;) {
                const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
                if (n <= 0) break;
                request.append(buffer, static_cast<std::size_t>(n));
                if (header_end == std::string::npos) {
                    header_end = request.find("\r\n\r\n");
                    if (header_end != std::string::npos) {
                        const std::string headers = request.substr(0, header_end);
                        const std::string key = "Content-Length:";
                        const auto pos = headers.find(key);
                        if (pos != std::string::npos) {
                            content_length = static_cast<std::size_t>(
                                std::strtoul(headers.c_str() + pos + key.size(), nullptr, 10));
                        }
                    }
                }
                if (header_end != std::string::npos &&
                    request.size() - (header_end + 4) >= content_length) {
                    break;
                }
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++request_count_;
                const auto line_end = request.find("\r\n");
                request_line_ = request.substr(0, line_end);
                if (header_end != std::string::npos) {
                    const std::string header_block = request.substr(0, header_end);
                    std::size_t cursor = header_block.find("\r\n");
                    while (cursor != std::string::npos) {
                        const std::size_t next = header_block.find("\r\n", cursor + 2);
                        const std::string line = header_block.substr(
                            cursor + 2,
                            (next == std::string::npos ? header_block.size() : next) - cursor - 2
                        );
                        const auto colon = line.find(':');
                        if (colon != std::string::npos) {
                            const std::string key = line.substr(0, colon);
                            std::string value = line.substr(colon + 1);
                            const auto first = value.find_first_not_of(" \t");
                            const auto last = value.find_last_not_of(" \t");
                            value = (first == std::string::npos) ? "" : value.substr(first, last - first + 1);
                            headers_.emplace_back(key, value);
                        }
                        cursor = next;
                    }
                    body_ = request.substr(header_end + 4);
                }
            }

            const char* response =
                "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
            ::send(client, response, std::strlen(response), 0);
            ::close(client);
        }
    }

    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::string request_line_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string body_;
    std::size_t request_count_ = 0;
};

// 已验票据（dev 签名形状，mode=certification 非 boss），手牌/能量按用例定制。
phk::battle::SignedBattleTicket MakeSignedTicket(
    const std::vector<std::string>& hand_card_ids,
    double initial_energy
) {
    phk::battle::SignedBattleTicket signed_ticket;
    signed_ticket.ticket.ticket_id = "ticket-cast-001";
    signed_ticket.ticket.match_id = kMatchId;
    signed_ticket.ticket.user_id = "user-alice";
    signed_ticket.ticket.player_id = "p1";
    signed_ticket.ticket.mode_id = "certification";
    signed_ticket.ticket.battle_server_id = "battle-local-1";
    signed_ticket.ticket.endpoint = "127.0.0.1:7901";
    signed_ticket.ticket.deck_snapshot_hash = "sha256:deck";
    signed_ticket.ticket.ruleset_version = "ruleset-local-s0";
    signed_ticket.ticket.ticket_nonce_hex = "00112233445566778899aabb";
    signed_ticket.ticket.issued_at_ms = 1782489600000;
    signed_ticket.ticket.expires_at_ms = 1782489660000;
    signed_ticket.ticket.business_session_id = "session-ref:dev-alice";
    signed_ticket.ticket.hand_card_ids = hand_card_ids;
    signed_ticket.ticket.initial_energy = initial_energy;
    signed_ticket.ticket.server_seed_decimal = "8035909259742954606";
    signed_ticket.key_id = "dev-ed25519-local";
    signed_ticket.public_key_hex = std::string(64, 'a');
    signed_ticket.signature_hex = std::string(128, 'b');
    signed_ticket.server_authoritative = true;
    return signed_ticket;
}

// W-1 phase 2: same shape, plus the authoritative ordered deck (its prefix
// is the opening hand, matching the lobby issuance contract).
phk::battle::SignedBattleTicket MakeSignedTicketWithDeck(
    const std::vector<std::string>& hand_card_ids,
    double initial_energy,
    const std::vector<std::string>& deck_card_ids
) {
    auto signed_ticket = MakeSignedTicket(hand_card_ids, initial_energy);
    signed_ticket.ticket.deck_card_ids = deck_card_ids;
    return signed_ticket;
}

// payload 形状逐项对大厅 BattleEventRow / BattleEventsSubmitRequest
// （battle_events.go:41-71）的字段名、omitempty 语义断言。
void TestBattleEventsPayloadShape() {
    using phk::battle::BuildBattleEventsJson;
    using phk::battle::CardCastEvent;

    CardCastEvent rejected;
    rejected.event_type = "card_rejected";
    rejected.tick = 12;
    rejected.player_id = "p1";
    rejected.slot = 2;
    rejected.reason = "cast_card_on_cooldown";

    CardCastEvent accepted;
    accepted.event_type = "card_accepted";
    accepted.tick = 13;
    accepted.player_id = "p1";
    accepted.card_id = "mirror_lance";
    accepted.event_value = 26;
    accepted.slot = 0;  // Go omitempty：0 省略

    CardCastEvent effect;
    effect.event_type = "card_effect_applied";
    effect.tick = 13;
    effect.player_id = "p1";
    effect.card_id = "mirror_lance";
    effect.event_value = 7;
    effect.target_user_id = "p2";

    CardCastEvent boss;
    boss.event_type = "boss_defeated";
    boss.tick = 14;
    boss.player_id = "p1";
    boss.reason = "world-boss:match-cast-001";

    const std::string json = BuildBattleEventsJson(
        "battle-local-1", kMatchId, {rejected, accepted, effect, boss});
    // 信封（BattleEventsSubmitRequest struct tag）
    Check(json.find("\"battle_server_id\":\"battle-local-1\"") != std::string::npos, "payload battle_server_id");
    Check(json.find("\"match_id\":\"match-cast-001\"") != std::string::npos, "payload match_id");
    Check(json.find("\"events\":[") != std::string::npos, "payload events array");
    // card_rejected：type/tick/user_id/card_id/value 恒发，slot=2，reason
    Check(
        json.find("\"type\":\"card_rejected\",\"tick\":12,\"user_id\":\"p1\","
                  "\"card_id\":\"\",\"value\":0,\"slot\":2,"
                  "\"reason\":\"cast_card_on_cooldown\"") != std::string::npos,
        "card_rejected row shape (BattleEventRow field names)"
    );
    // card_accepted：slot=0 省略（Go omitempty）
    Check(
        json.find("\"type\":\"card_accepted\",\"tick\":13,\"user_id\":\"p1\","
                  "\"card_id\":\"mirror_lance\",\"value\":26}") != std::string::npos,
        "card_accepted row shape (slot 0 omitted like Go)"
    );
    // card_effect_applied：target_user_id 在、slot 未设置省略
    Check(
        json.find("\"type\":\"card_effect_applied\",\"tick\":13,\"user_id\":\"p1\","
                  "\"card_id\":\"mirror_lance\",\"value\":7,"
                  "\"target_user_id\":\"p2\"") != std::string::npos,
        "card_effect_applied row shape (target_user_id present, slot omitted)"
    );
    // boss_defeated：card_id 恒发（空串），reason 携带 boss id
    Check(
        json.find("\"type\":\"boss_defeated\",\"tick\":14,\"user_id\":\"p1\","
                  "\"card_id\":\"\",\"value\":0,"
                  "\"reason\":\"world-boss:match-cast-001\"") != std::string::npos,
        "boss_defeated row shape (empty card_id, reason carries boss id)"
    );
    // JSON 转义（reason 含引号/反斜杠）
    CardCastEvent escaped = rejected;
    escaped.reason = "a\"b\\c";
    const std::string json2 = BuildBattleEventsJson("s", "m", {escaped});
    Check(
        json2.find("\"reason\":\"a\\\"b\\\\c\"") != std::string::npos,
        "json escaping in reason field"
    );
    // 回调头（service-callback 守卫要求的两头）
    const auto headers = phk::battle::BattleEventsCallbackHeaders();
    Check(headers.size() == 2, "two callback headers");
    Check(
        headers[0].first == "X-PhK-Service-Origin" && headers[0].second == "battle_server",
        "service origin header"
    );
    Check(
        headers[1].first == "X-PhK-Battle-Callback" && headers[1].second == "true",
        "battle callback header"
    );
    // 每次提交行数上限常量与大厅一致（battle_events.go:23）
    Check(phk::battle::kMaxBattleEventsPerSubmit == 64, "per-submit cap mirrors lobby (64)");
}

// T6：BattleCallbackHeaders = 公开两头 + 对精确 body 的 HMAC 签名。
// 签名为离线（Python hmac）预算的 KAT 值，跨 Go/C++ 逐字节对齐。
void TestBattleCallbackHeadersSigned() {
    using phk::battle::BattleCallbackHeaders;
    using phk::battle::HmacSha256Hex;

    const std::string secret =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const std::string body =
        "{\"battle_server_id\":\"battle-local-1\",\"match_id\":\"match-1\",\"events\":[]}";

    const auto headers = BattleCallbackHeaders(body, secret);
    Check(headers.size() == 3, "signed callback headers carry three entries");
    Check(headers[0].first == "X-PhK-Service-Origin" && headers[0].second == "battle_server",
          "public pre-filter origin header unchanged");
    Check(headers[1].first == "X-PhK-Battle-Callback" && headers[1].second == "true",
          "public pre-filter callback header unchanged");
    Check(headers[2].first == "X-PhK-Battle-Signature", "signature header name");
    Check(headers[2].second ==
              "5d20a470869511b7e0be3ed2c81fbeefc9d6ac06078c6fb0b608a798c49cc206",
          "signature KAT (Python hmac cross-check)");
    Check(headers[2].second == HmacSha256Hex(secret, body),
          "signature equals HMAC over the exact body");

    // 秘密为空：只发公开两头（大厅会 403，错配立即可见，不静默回退）。
    const auto unsigned_headers = BattleCallbackHeaders(body, "");
    Check(unsigned_headers.size() == 2, "empty secret yields only the public pair");

    // 换一个字节的 body，签名必须变（大厅侧篡改即拒的依据）。
    const std::string tampered = body;
    std::string mutated;
    for (char ch : tampered) {
        mutated.push_back(ch == 'a' ? 'b' : ch);
    }
    Check(HmacSha256Hex(secret, mutated) != headers[2].second,
          "tampered body changes the signature");
}

// 端到端：票据播种 → 出牌 → TickMatch → SubmitCardEvents →
// 捕获请求断言（路径 / service-callback 头 / payload）。
void TestSubmitCardEventsWire() {
    CapturingHttpServer http;
    Check(http.port() != 0, "capture server listening");

    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489605000;
    config.server_id = "battle-local-1";
    config.lobby_endpoint = "127.0.0.1:" + std::to_string(http.port());
    // T6：注册响应下发的回调秘密；断言见下（签名覆盖线上精确 body）。
    config.callback_secret =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    BattleServer server(config);

    const auto registered = server.RegisterTicket(
        MakeSignedTicket({"mirror_lance", "focus_lens", "focus_lens", "focus_lens"}, 10.0));
    Check(registered.ok, "ticket registered (" + registered.reason + ")");
    Check(server.PendingCardEventCount(kMatchId) == 0, "no rows before any cast");

    BattleModeAction action = MakeCastAction("p1", 1, 1, 0);
    Check(server.AcceptModeAction(action).ok, "accept cast via server");
    server.TickMatch(kMatchId);
    Check(server.PendingCardEventCount(kMatchId) == 2, "two rows pending (accepted + effect)");

    const auto submit = server.SubmitCardEvents(kMatchId);
    Check(submit.ok, "submit ok (" + submit.error + ")");
    Check(submit.status_code == 200, "submit status 200");
    Check(server.PendingCardEventCount(kMatchId) == 0, "rows drained after success");

    Check(http.request_line() == "POST /v1/battle/events HTTP/1.1", "events route");
    Check(http.header("X-PhK-Service-Origin") == "battle_server", "service origin header on wire");
    Check(http.header("X-PhK-Battle-Callback") == "true", "battle callback header on wire");
    Check(
        http.header("X-PhK-Battle-Signature") ==
            phk::battle::HmacSha256Hex(config.callback_secret, http.body()),
        "signature covers the exact posted body on the wire"
    );
    Check(http.header("Content-Type") == "application/json", "content type json");
    Check(
        http.body().find("\"battle_server_id\":\"battle-local-1\"") != std::string::npos,
        "posted battle_server_id"
    );
    Check(
        http.body().find("\"match_id\":\"match-cast-001\"") != std::string::npos,
        "posted match_id"
    );
    Check(
        http.body().find("\"type\":\"card_accepted\"") != std::string::npos,
        "posted card_accepted row"
    );
    Check(
        http.body().find("\"type\":\"card_effect_applied\"") != std::string::npos,
        "posted card_effect_applied row"
    );
    Check(
        http.body().find("\"card_id\":\"mirror_lance\"") != std::string::npos,
        "posted card id"
    );

    // 无行可发：no-op，不再发起 HTTP（lobby 拒空 events）。
    const auto again = server.SubmitCardEvents(kMatchId);
    Check(again.ok, "second submit ok");
    Check(again.status_code == 0, "second submit is a no-op (no HTTP)");
    Check(http.request_count() == 1, "exactly one HTTP request total");

    http.Stop();
}

// 回传失败（连接被拒）与未配置端点：行保留待重发，不丢事件。
void TestSubmitCardEventsFailureKeepsRows() {
    // 未配置端点。
    {
        phk::battle::BattleServerConfig config;
        config.now_ms = 1782489605000;
        BattleServer server(config);
        Check(
            server.RegisterTicket(
                MakeSignedTicket({"mirror_lance", "focus_lens", "focus_lens", "focus_lens"}, 10.0)).ok,
            "ticket registered (no endpoint case)"
        );
        BattleModeAction action = MakeCastAction("p1", 1, 1, 0);
        Check(server.AcceptModeAction(action).ok, "accept cast");
        server.TickMatch(kMatchId);
        const auto submit = server.SubmitCardEvents(kMatchId);
        Check(!submit.ok, "submit without endpoint fails");
        Check(submit.error == "no_lobby_endpoint", "no_lobby_endpoint error");
        Check(server.PendingCardEventCount(kMatchId) == 2, "rows kept without endpoint");
    }
    // 端点指向关闭端口（连接被拒）。
    {
        phk::battle::BattleServerConfig config;
        config.now_ms = 1782489605000;
        config.lobby_endpoint = "127.0.0.1:1";
        BattleServer server(config);
        Check(
            server.RegisterTicket(
                MakeSignedTicket({"mirror_lance", "focus_lens", "focus_lens", "focus_lens"}, 10.0)).ok,
            "ticket registered (refused case)"
        );
        BattleModeAction action = MakeCastAction("p1", 1, 1, 0);
        Check(server.AcceptModeAction(action).ok, "accept cast");
        server.TickMatch(kMatchId);
        const auto submit = server.SubmitCardEvents(kMatchId);
        Check(!submit.ok, "submit to closed port fails");
        Check(server.PendingCardEventCount(kMatchId) == 2, "rows restored after failure");
    }
}

// Take/Restore 语义（64 行上限由 SubmitCardEvents 强制）。
// W-1 phase 2: a ticket deck seeds HandState.deck; a cast redraws from where
// the lobby left off (draw_cursor = hand size), exactly like the Go
// drawServerCard cursor semantics. Without a deck the legacy no-op redraw
// stays unchanged.
void TestDeckTicketRedrawsAfterCast() {
    const std::vector<std::string> hand = {
        "mirror_lance", "focus_lens", "graze_engine", "guard_seal"};
    const std::vector<std::string> deck = {
        "mirror_lance", "focus_lens", "graze_engine", "guard_seal",
        "aegis_ward", "finale_arc"};

    BattleSimulation simulation(DuelConfig());
    Check(simulation.AddPlayer("p1", 0, 0), "deck redraw: add p1");
    Check(
        simulation.SeedPlayerHand("p1", hand, 10.0, "5", deck),
        "deck redraw: seed hand with the ticket deck"
    );
    Check(simulation.HandSize("p1") == 4, "deck redraw: hand seeded at the limit");
    // Cast slot 0: hand drops to 3, the deck redraws back to the limit.
    Check(
        simulation.AcceptModeAction(MakeCastAction("p1", 1, 1, 0)).ok,
        "deck redraw: accept cast slot 0"
    );
    simulation.Tick();
    Check(simulation.HandSize("p1") == 4, "deck redraw: cast redraws from the ticket deck");
    Check(simulation.CardCastEvents().size() == 2, "deck redraw: accepted + effect rows");

    // The whole server path: the validated ticket deck reaches the session.
    phk::battle::BattleServerConfig deck_config;
    deck_config.now_ms = 1782489605000;
    BattleServer server(deck_config);
    const auto registered = server.RegisterTicket(
        MakeSignedTicketWithDeck(hand, 10.0, deck));
    Check(registered.ok, "deck redraw: deck ticket registers");
    Check(registered.session.deck_card_ids == deck, "deck redraw: session carries the deck");

    // Legacy: no deck on the ticket => no redraw (unchanged behavior).
    BattleSimulation legacy(DuelConfig());
    Check(legacy.AddPlayer("p1", 0, 0), "deck redraw: add legacy p1");
    Check(
        legacy.SeedPlayerHand("p1", hand, 10.0, "5"),
        "deck redraw: seed hand without a deck"
    );
    Check(
        legacy.AcceptModeAction(MakeCastAction("p1", 1, 1, 0)).ok,
        "deck redraw: accept legacy cast"
    );
    legacy.Tick();
    Check(legacy.HandSize("p1") == 3, "deck redraw: no deck, no redraw");
}

void TestTakeRestoreCardCastEvents() {
    BattleSimulation simulation(DuelConfig());
    Check(simulation.AddPlayer("p1", 0, 0), "add p1");
    Check(
        simulation.SeedPlayerHand(
            "p1",
            {"mirror_lance", "mirror_lance", "mirror_lance", "mirror_lance"},
            10.0,
            "5"
        ),
        "seed 4 mirror_lance"
    );
    for (int index = 0; index < 4; ++index) {
        BattleModeAction action = MakeCastAction("p1", static_cast<std::uint64_t>(index + 1), 1, 0);
        // 四张同卡同 tick：第一张 accepted，其余 on_cooldown → 共 4+ 行。
        Check(simulation.AcceptModeAction(action).ok, "accept cast " + std::to_string(index));
    }
    simulation.Tick();
    const std::size_t pending = simulation.CardCastEvents().size();
    Check(pending >= 5, "multiple rows collected (" + std::to_string(pending) + ")");

    const auto taken = simulation.TakeCardCastEvents(2);
    Check(taken.size() == 2, "take 2 rows");
    Check(simulation.CardCastEvents().size() == pending - 2, "remaining rows after take");
    simulation.RestoreCardCastEvents(taken);
    Check(simulation.CardCastEvents().size() == pending, "rows restored to front");
    const auto taken_all = simulation.TakeCardCastEvents();
    Check(taken_all.size() == pending, "take all with limit 0");
    Check(simulation.CardCastEvents().empty(), "collector empty after take all");
}

}  // namespace

int main() {
    TestSeedAndSentinel();
    TestMirrorLanceCastViaModeAction();
    TestPurgeBellCastViaInputChannel();
    TestAegisWardCastViaModeAction();
    TestFinaleArcClearsField();
    TestRejectionPaths();
    TestBossDamageAndDefeatRow();
    TestBattleEventsPayloadShape();
    TestBattleCallbackHeadersSigned();
    TestSubmitCardEventsWire();
    TestSubmitCardEventsFailureKeepsRows();
    TestDeckTicketRedrawsAfterCast();
    TestTakeRestoreCardCastEvents();

    if (g_failures != 0) {
        std::cerr << g_failures << " card cast execution test(s) failed\n";
        return 1;
    }
    std::cout << "card cast execution tests passed\n";
    return 0;
}
