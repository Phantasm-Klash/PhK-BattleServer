// Tests for the per-match battle server lifecycle:
//   * application wire codec round-trip,
//   * deterministic result payload / JSON construction,
//   * a real UDP + KCP socket smoke test that races two players to a winner,
//   * the hand-written HTTP/1.1 result submission client.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ikcp.h"
#include "phk/battle/match_lifecycle.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& label) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << label << "\n";
    }
}

std::uint32_t NowMs() {
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

phk::battle::BattleInput MakeInput(
    const std::string& match_id,
    const std::string& player_id,
    std::uint64_t tick,
    std::uint64_t seq,
    bool shoot
) {
    phk::battle::BattleInput input;
    input.match_id = match_id;
    input.player_id = player_id;
    input.tick = tick;
    input.seq = seq;
    input.shoot = shoot;
    input.slow = false;
    input.bomb = false;
    input.card_slot = -1;
    input.direction_bits = 0;
    return input;
}

// ---------------------------------------------------------------------------
// 1. wire codec
// ---------------------------------------------------------------------------
void TestInputCodecRoundTrip() {
    phk::battle::BattleInput input;
    input.match_id = "match_codec_1";
    input.player_id = "player_x";
    input.tick = 12345;
    input.seq = 678;
    input.direction_bits = 0x5;
    input.slow = true;
    input.shoot = true;
    input.bomb = false;
    input.card_slot = 3;
    input.mode_action_id = "action_dash";

    const auto payload = phk::battle::EncodeBattleInputPayload(input);
    Check(!payload.empty(), "input payload non-empty");
    Check(payload[0] == static_cast<std::uint8_t>(phk::battle::BattlePayloadType::Input), "input payload type byte");

    phk::battle::BattleInput decoded;
    Check(phk::battle::DecodeBattleInputPayload(payload, &decoded), "input payload decodes");
    Check(decoded.match_id == input.match_id, "codec match_id");
    Check(decoded.player_id == input.player_id, "codec player_id");
    Check(decoded.tick == input.tick, "codec tick");
    Check(decoded.seq == input.seq, "codec seq");
    Check(decoded.direction_bits == input.direction_bits, "codec direction_bits");
    Check(decoded.slow == input.slow && decoded.shoot == input.shoot && decoded.bomb == input.bomb, "codec flags");
    Check(decoded.card_slot == input.card_slot, "codec card_slot");
    Check(decoded.mode_action_id == input.mode_action_id, "codec mode_action_id");

    std::vector<std::uint8_t> garbage = {0x00, 0x01, 0x02};
    Check(!phk::battle::DecodeBattleInputPayload(garbage, &decoded), "codec rejects non-input frame");

    std::vector<std::uint8_t> truncated(payload.begin(), payload.begin() + 4);
    Check(!phk::battle::DecodeBattleInputPayload(truncated, &decoded), "codec rejects truncated frame");
}

// §2.8 快照 JSON 新字段：键必须存在（客户端按名解析），占位值可解析。
void TestSnapshotJsonCarriesP1WireFields() {
    phk::battle::BossRaceConfig config;
    config.match_id = "match_wire_000001";
    config.match_seed = 20261024u;
    config.boss_max_hp = 1000000;
    phk::battle::BossRaceSimulation sim(config);
    sim.AddPlayer("player_a", 0, 60000);
    sim.AddPlayer("player_b", 0, 60000);
    for (std::uint64_t tick = 1; tick <= 45; ++tick) {
        sim.SubmitInput(MakeInput(config.match_id, "player_a", tick, tick, true));
        sim.SubmitInput(MakeInput(config.match_id, "player_b", tick, tick, true));
        sim.Tick();
    }

    const auto payload = phk::battle::EncodeBossRaceSnapshotPayload(sim.Snapshot());
    Check(!payload.empty() &&
              payload[0] == static_cast<std::uint8_t>(phk::battle::BattlePayloadType::Snapshot),
          "snapshot payload type byte");
    const std::string json(payload.begin() + 1, payload.end());

    for (const char* key : {
             "\"boss\":{",
             "\"max_hp\":",
             "\"phase\":",
             "\"movement\":{\"type\":\"",
             "\"phase_t\":",
             "\"effects\":[",
             "\"vx_milli\":",
             "\"vy_milli\":",
             "\"facing_millideg\":",
             "\"lives\":",
             "\"bombs\":",
             "\"invulnerable_until_tick\":",
             "\"graze\":",
             "\"chain_id\":",
             "\"generation\":",
             "\"hidden\":",
             "\"chains\":[",
             "\"hand\":[",
             "\"deck_remaining\":",
             "\"draw_cooldown_ticks\":",
             "\"events\":[",
             "\"event_cursor\":",
         }) {
        Check(json.find(key) != std::string::npos, std::string("snapshot json has ") + key);
    }
    Check(json.find("\"hidden\":false") != std::string::npos, "snapshot json: hidden defaults to false");
    Check(json.find("\"hand\":[]") != std::string::npos, "snapshot json: hand defaults to []");
    Check(json.find("\"events\":[]") != std::string::npos, "snapshot json: events defaults to []");
    Check(json.find("\"movement\":{\"type\":\"static\"") != std::string::npos,
          "snapshot json: movement defaults to static");
}

// ---------------------------------------------------------------------------
// 1b. signed ticket enforcement on the app (per-match) path
// ---------------------------------------------------------------------------
namespace ticket_test {

phk::battle::SignedBattleTicket MakeTicket(
    const std::string& match_id,
    const std::string& player_id,
    std::int64_t now_ms,
    const std::string& ticket_id = "ticket_0001"
) {
    phk::battle::SignedBattleTicket signed_ticket;
    phk::battle::BattleTicket& ticket = signed_ticket.ticket;
    ticket.version = phk::battle::VersionStamp();
    ticket.ticket_id = ticket_id;
    ticket.match_id = match_id;
    ticket.user_id = "user_0001";
    ticket.player_id = player_id;
    ticket.mode_id = "mvp_boss_race";
    ticket.battle_server_id = "bs-104-1";
    ticket.endpoint = "104.233.217.232:7400";
    ticket.deck_snapshot_hash = "sha256:" + std::string(64, 'a');
    ticket.ruleset_version = ticket.version.ruleset_version;
    ticket.ticket_nonce_hex = std::string(48, 'b');
    ticket.issued_at_ms = now_ms - 1000;
    ticket.expires_at_ms = now_ms + 600000;
    ticket.business_session_id = "session_0001";
    ticket.hand_card_ids = {"focus_lens", "graze_engine"};
    ticket.deck_card_ids = {"focus_lens", "graze_engine", "aegis_ward", "guard_seal", "tempo_break"};
    ticket.initial_energy = 3.5;
    ticket.server_seed_decimal = "987654321";
    signed_ticket.signature_alg = "ED25519";
    signed_ticket.key_id = "lobby-key-1";
    signed_ticket.signature_hex = std::string(128, 'c');
    signed_ticket.public_key_hex = std::string(64, 'd');
    signed_ticket.server_authoritative = true;
    return signed_ticket;
}

phk::battle::MatchLifecycleConfig MakeTicketConfig(phk::battle::MatchTicketRequirement mode) {
    phk::battle::MatchLifecycleConfig config;
    config.port = 0;
    config.match_id = "match_ticket_000001";
    config.seed = 42u;
    config.player_ids = {"player_a", "player_b"};
    config.boss_max_hp = 100;
    config.require_ticket = mode;
    config.battle_server_id = "bs-104-1";
    config.ticket_key_id = "lobby-key-1";
    config.now_ms = 1800000000000ll;
    return config;
}

const std::int64_t kNow = 1800000000000ll;

// Ticket verifies -> the conversation binds and the input is accepted.
void TestTicketVerifiedBindsPlayer() {
    auto config = MakeTicketConfig(phk::battle::MatchTicketRequirement::On);
    phk::battle::MatchServer server(config);
    std::string error;
    Check(server.Start(&error), "ticket(on): server starts (" + error + ")");

    const auto ticket = MakeTicket(config.match_id, "player_a", kNow);
    const auto payload = phk::battle::EncodeBattleInputWithTicketPayload(
        MakeInput(config.match_id, "player_a", 1, 1, true), ticket);
    Check(server.InjectPayload(0x0000A011u, payload), "ticket(on): payload injected");

    Check(server.BoundPlayerCount() == 1, "ticket(on): player bound on valid ticket");
    Check(server.AcceptedInputCount() == 1, "ticket(on): input accepted");
    Check(server.TicketRejectionCount() == 0, "ticket(on): no rejections");
    Check(server.TicketAuditLog().size() == 1, "ticket(on): one audit entry");
    Check(!server.TicketAuditLog().empty() &&
          server.TicketAuditLog().back().reason == "ok", "ticket(on): audit reason is ok");
    server.Stop();
}

// Every rejection path must bind nothing and record an explicit reason.
void TestTicketRejectionsRecordReason() {
    struct Case {
        const char* label;
        const char* expected_reason;
        bool attach_ticket;
        std::string match_id;
        std::string ticket_id;
        std::int64_t expires_at_ms;
    };
    const std::vector<Case> cases = {
        {"missing ticket", "ticket_missing", false, "", "", 0},
        {"ticket for another match", "ticket_match_mismatch", true, "match_other_000001", "t_other", 0},
        {"expired ticket", "ticket_expired", true, "match_ticket_000001", "t_expired", kNow - 1},
        {"replayed ticket", "ticket_replay", true, "match_ticket_000001", "t_replay", 0},
    };

    for (const Case& test_case : cases) {
        auto config = MakeTicketConfig(phk::battle::MatchTicketRequirement::On);
        phk::battle::MatchServer server(config);
        std::string error;
        Check(server.Start(&error), std::string("ticket(on): server starts (") + test_case.label + ")");

        if (test_case.expected_reason == std::string("ticket_replay")) {
            // Consume the ticket id once, then present it again from a new conv.
            const auto first = MakeTicket(config.match_id, "player_a", kNow, test_case.ticket_id);
            server.InjectPayload(0x0000A021u, phk::battle::EncodeBattleInputWithTicketPayload(
                MakeInput(config.match_id, "player_a", 1, 1, true), first));
        }

        auto ticket = MakeTicket(config.match_id, "player_a", kNow, test_case.ticket_id);
        if (!test_case.match_id.empty()) {
            ticket.ticket.match_id = test_case.match_id;
        }
        if (test_case.expires_at_ms != 0) {
            ticket.ticket.expires_at_ms = test_case.expires_at_ms;
        }

        std::vector<std::uint8_t> payload = test_case.attach_ticket
            ? phk::battle::EncodeBattleInputWithTicketPayload(
                MakeInput(config.match_id, "player_a", 1, 1, true), ticket)
            : phk::battle::EncodeBattleInputPayload(
                MakeInput(config.match_id, "player_a", 1, 1, true));
        server.InjectPayload(0x0000A031u, payload);

        Check(server.BoundPlayerCount() == (test_case.expected_reason == std::string("ticket_replay") ? 1u : 0u),
              std::string("ticket(on): ") + test_case.label + " does not bind the rejected conv");
        Check(server.AcceptedInputCount() == (test_case.expected_reason == std::string("ticket_replay") ? 1u : 0u),
              std::string("ticket(on): ") + test_case.label + " drops the input");
        Check(server.TicketRejectionCount() >= 1,
              std::string("ticket(on): ") + test_case.label + " counts a rejection");
        Check(!server.TicketAuditLog().empty() &&
              server.TicketAuditLog().back().reason == test_case.expected_reason,
              std::string("ticket(on): ") + test_case.label + " records reason " +
                  test_case.expected_reason + " (got " +
                  (server.TicketAuditLog().empty() ? std::string("<none>") : server.TicketAuditLog().back().reason) +
                  ")");
        Check(!server.TicketAuditLog().empty() && server.TicketAuditLog().back().rejected,
              std::string("ticket(on): ") + test_case.label + " audit entry is marked rejected");
        server.Stop();
    }
}

// warn mode: observe, do not interrupt. off mode: legacy behaviour, untouched.
void TestTicketWarnAndOffModes() {
    // warn: a ticket-less client still binds, but the gap is logged.
    {
        auto config = MakeTicketConfig(phk::battle::MatchTicketRequirement::Warn);
        phk::battle::MatchServer server(config);
        std::string error;
        Check(server.Start(&error), "ticket(warn): server starts");
        server.InjectPayload(0x0000A041u, phk::battle::EncodeBattleInputPayload(
            MakeInput(config.match_id, "player_a", 1, 1, true)));
        Check(server.BoundPlayerCount() == 1, "ticket(warn): legacy client still binds");
        Check(server.TicketRejectionCount() == 0, "ticket(warn): no hard rejection");
        Check(!server.TicketAuditLog().empty() &&
              server.TicketAuditLog().back().reason == "ticket_missing",
              "ticket(warn): missing ticket is recorded");
        Check(!server.TicketAuditLog().empty() && !server.TicketAuditLog().back().rejected,
              "ticket(warn): entry is a warning, not a rejection");
        server.Stop();
    }

    // off: no verification at all, and an unknown player id is still refused,
    // but now with a reason instead of a silent return.
    {
        auto config = MakeTicketConfig(phk::battle::MatchTicketRequirement::Off);
        phk::battle::MatchServer server(config);
        std::string error;
        Check(server.Start(&error), "ticket(off): server starts");
        server.InjectPayload(0x0000A051u, phk::battle::EncodeBattleInputPayload(
            MakeInput(config.match_id, "player_a", 1, 1, true)));
        Check(server.BoundPlayerCount() == 1, "ticket(off): binds without any ticket");
        Check(server.AcceptedInputCount() == 1, "ticket(off): input accepted");
        Check(server.TicketAuditLog().empty(), "ticket(off): no ticket audit at all");

        server.InjectPayload(0x0000A052u, phk::battle::EncodeBattleInputPayload(
            MakeInput(config.match_id, "player_intruder", 1, 1, true)));
        Check(server.BoundPlayerCount() == 1, "ticket(off): unknown player not bound");
        Check(server.TicketRejectionCount() == 1, "ticket(off): unknown player counted");
        Check(!server.TicketAuditLog().empty() &&
              server.TicketAuditLog().back().reason == "player_not_in_roster",
              "ticket(off): unknown player records a reason (was silent before)");
        server.Stop();
    }
}

// The optional ticket block must round-trip and must not break legacy frames.
void TestTicketFrameCodec() {
    const auto input = MakeInput("match_ticket_000001", "player_a", 7, 9, true);
    const auto ticket = MakeTicket("match_ticket_000001", "player_a", kNow);

    const auto with_ticket = phk::battle::EncodeBattleInputWithTicketPayload(input, ticket);
    phk::battle::BattleInput decoded_input;
    Check(phk::battle::DecodeBattleInputPayload(with_ticket, &decoded_input),
          "ticket codec: frame with ticket still decodes as input");
    Check(decoded_input.player_id == input.player_id && decoded_input.seq == input.seq,
          "ticket codec: input body intact");

    phk::battle::SignedBattleTicket decoded_ticket;
    Check(phk::battle::DecodeBattleInputTicketPayload(with_ticket, &decoded_ticket),
          "ticket codec: ticket block decodes");
    Check(decoded_ticket.ticket.ticket_id == ticket.ticket.ticket_id, "ticket codec: ticket_id");
    Check(decoded_ticket.ticket.match_id == ticket.ticket.match_id, "ticket codec: match_id");
    Check(decoded_ticket.ticket.player_id == ticket.ticket.player_id, "ticket codec: player_id");
    Check(decoded_ticket.ticket.hand_card_ids == ticket.ticket.hand_card_ids, "ticket codec: hand cards");
    Check(decoded_ticket.ticket.deck_card_ids == ticket.ticket.deck_card_ids, "ticket codec: deck cards");
    Check(decoded_ticket.ticket.initial_energy == ticket.ticket.initial_energy, "ticket codec: initial_energy");
    Check(decoded_ticket.ticket.server_seed_decimal == ticket.ticket.server_seed_decimal, "ticket codec: seed");
    Check(decoded_ticket.signature_hex == ticket.signature_hex, "ticket codec: signature");
    Check(decoded_ticket.server_authoritative, "ticket codec: server_authoritative");

    const auto legacy = phk::battle::EncodeBattleInputPayload(input);
    Check(!phk::battle::DecodeBattleInputTicketPayload(legacy, &decoded_ticket),
          "ticket codec: legacy frame carries no ticket");
    Check(phk::battle::DecodeBattleInputPayload(legacy, &decoded_input),
          "ticket codec: legacy frame still decodes");
}

}  // namespace ticket_test

// §2.3 抽卡/选卡/用卡：新帧往返、默认帧不加字节（老帧逐字节兼容）、
// 老帧带 ticket 仍能解出 ticket（magic 0x43 不与 ticket 存在位 0/1 冲突）。
void TestCardInputFieldsCodec() {
    phk::battle::BattleInput input = MakeInput("match_card_000001", "player_a", 11, 5, true);
    input.card_draw = true;
    input.card_select = 2;
    input.card_use = true;

    const auto payload = phk::battle::EncodeBattleInputPayload(input);
    phk::battle::BattleInput decoded;
    Check(phk::battle::DecodeBattleInputPayload(payload, &decoded), "card codec: frame with card block decodes");
    Check(decoded.card_draw, "card codec: card_draw round-trips");
    Check(decoded.card_select == 2, "card codec: card_select round-trips");
    Check(decoded.card_use, "card codec: card_use round-trips");
    Check(decoded.player_id == input.player_id && decoded.seq == input.seq && decoded.shoot,
          "card codec: legacy fields intact");

    // 默认三字段（false / -1 / false）不写块 → 与老客户端帧逐字节等长。
    const phk::battle::BattleInput plain = MakeInput("match_card_000001", "player_a", 11, 5, true);
    const auto legacy = phk::battle::EncodeBattleInputPayload(plain);
    Check(payload.size() == legacy.size() + 4, "card codec: default fields add exactly 0 bytes");
    phk::battle::BattleInput legacy_decoded;
    Check(phk::battle::DecodeBattleInputPayload(legacy, &legacy_decoded), "card codec: legacy frame decodes");
    Check(!legacy_decoded.card_draw && legacy_decoded.card_select == -1 && !legacy_decoded.card_use,
          "card codec: legacy frame yields default card fields");

    // 取消选择：card_select = -1 且其余为真 → 仍然写块并原样回读。
    phk::battle::BattleInput cancel = MakeInput("match_card_000001", "player_a", 12, 6, false);
    cancel.card_draw = true;
    cancel.card_select = -1;
    phk::battle::BattleInput cancel_decoded;
    Check(phk::battle::DecodeBattleInputPayload(
              phk::battle::EncodeBattleInputPayload(cancel), &cancel_decoded),
          "card codec: draw-only frame decodes");
    Check(cancel_decoded.card_draw && cancel_decoded.card_select == -1 && !cancel_decoded.card_use,
          "card codec: draw-only frame keeps card_select = -1");

    // 老帧 + ticket 块：新解码器必须跳过「非 magic」字节，交给 ticket 解析。
    const auto ticket = ticket_test::MakeTicket("match_card_000001", "player_a", ticket_test::kNow);
    const auto legacy_with_ticket = phk::battle::EncodeBattleInputWithTicketPayload(plain, ticket);
    phk::battle::SignedBattleTicket decoded_ticket;
    Check(phk::battle::DecodeBattleInputTicketPayload(legacy_with_ticket, &decoded_ticket),
          "card codec: legacy frame with ticket still decodes as ticket");
    Check(decoded_ticket.ticket.ticket_id == ticket.ticket.ticket_id, "card codec: ticket payload intact");

    // 新帧 + ticket 块：card block 在前，ticket 块仍可解。
    const auto new_with_ticket = phk::battle::EncodeBattleInputWithTicketPayload(input, ticket);
    Check(phk::battle::DecodeBattleInputTicketPayload(new_with_ticket, &decoded_ticket),
          "card codec: card block + ticket block both decode");
    Check(phk::battle::DecodeBattleInputPayload(new_with_ticket, &decoded), "card codec: card block + ticket as input");
    Check(decoded.card_select == 2, "card codec: card fields survive ticket suffix");
}

// ---------------------------------------------------------------------------
// 2. deterministic result payload + JSON
// ---------------------------------------------------------------------------
phk::battle::MatchResultPayload RunRaceToResult(std::uint64_t seed, std::uint64_t boss_hp) {
    phk::battle::BossRaceConfig config;
    config.match_id = "match_result_000001";
    config.match_seed = seed;
    config.boss_max_hp = boss_hp;

    phk::battle::BossRaceSimulation sim(config);
    sim.AddPlayer("player_a", -20000, 60000);
    sim.AddPlayer("player_b", 20000, 60000);

    std::uint64_t seq_a = 0;
    std::uint64_t seq_b = 0;
    for (std::uint64_t tick = 1; tick <= 200 && sim.State() != phk::battle::BossRaceState::Finished; ++tick) {
        sim.SubmitInput(MakeInput(config.match_id, "player_a", tick, ++seq_a, true));
        sim.SubmitInput(MakeInput(config.match_id, "player_b", tick, ++seq_b, false));
        sim.Tick();
    }
    return phk::battle::BuildMatchResultPayload(sim);
}

void TestResultPayloadAndJson() {
    const auto payload = RunRaceToResult(20260626u, 100);
    Check(payload.match_id == "match_result_000001", "result match_id");
    Check(payload.winner_player_id == "player_a", "result winner");
    Check(payload.winner_tick == 10, "result winner tick (100hp / 10 per tick)");
    Check(!payload.state_hash.empty(), "result state hash present");
    Check(payload.players.size() == 2, "result has two players");
    Check(payload.players[0].player_id == "player_a", "result players sorted deterministically");
    Check(payload.players[0].damage_dealt == 100, "winner damage accounted");
    Check(payload.players[0].boss_current_hp == 0, "winner boss defeated");
    Check(payload.players[1].boss_current_hp == 100, "loser boss intact");

    const std::string json = phk::battle::BuildMatchResultJson(payload);
    Check(json.find("\"match_id\":\"match_result_000001\"") != std::string::npos, "json match_id field");
    Check(json.find("\"winner_player_id\":\"player_a\"") != std::string::npos, "json winner field");
    Check(json.find("\"winner_tick\":10") != std::string::npos, "json winner_tick field");
    Check(json.find("\"state_hash\":\"" + payload.state_hash + "\"") != std::string::npos, "json state_hash field");
    Check(json.find("\"damage_dealt\":100") != std::string::npos, "json damage field");
    Check(json.find("\"boss_current_hp\":0") != std::string::npos, "json boss hp field");
    Check(json.front() == '{' && json.back() == '}', "json is a single object");

    // Determinism: identical inputs must yield identical JSON, byte for byte.
    const auto payload_again = RunRaceToResult(20260626u, 100);
    const std::string json_again = phk::battle::BuildMatchResultJson(payload_again);
    Check(json == json_again, "result json is deterministic");
}

// ---------------------------------------------------------------------------
// 3. real UDP + KCP socket smoke test
// ---------------------------------------------------------------------------
class KcpClient {
public:
    ~KcpClient() {
        if (kcp_ != nullptr) ikcp_release(kcp_);
        if (fd_ >= 0) ::close(fd_);
    }

    bool Open(const std::string& host, std::uint16_t port, std::uint32_t conv) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return false;

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = 0;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) return false;

        const int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags >= 0) ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

        server_addr_.sin_family = AF_INET;
        server_addr_.sin_port = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &server_addr_.sin_addr) != 1) return false;

        kcp_ = ikcp_create(conv, this);
        if (kcp_ == nullptr) return false;
        ikcp_setoutput(kcp_, &KcpClient::OutputCallback);
        ikcp_nodelay(kcp_, 1, 10, 2, 1);
        ikcp_wndsize(kcp_, 128, 128);
        return true;
    }

    void Send(const std::vector<std::uint8_t>& payload) {
        ikcp_send(kcp_, reinterpret_cast<const char*>(payload.data()), static_cast<int>(payload.size()));
        ikcp_update(kcp_, NowMs());
        ikcp_flush(kcp_);
    }

    void Pump() {
        char datagram[65536];
        for (;;) {
            const ssize_t n = ::recv(fd_, datagram, sizeof(datagram), 0);
            if (n <= 0) break;
            ikcp_input(kcp_, datagram, static_cast<long>(n));
        }
        ikcp_update(kcp_, NowMs());
        ikcp_flush(kcp_);

        char message[65536];
        for (;;) {
            const int n = ikcp_recv(kcp_, message, static_cast<int>(sizeof(message)));
            if (n < 0) break;
            received_.emplace_back(message, message + n);
        }
    }

    std::size_t ReceivedCount() const { return received_.size(); }
    const std::vector<std::uint8_t>& Received(std::size_t index) const { return received_[index]; }

private:
    static int OutputCallback(const char* buf, int len, ikcpcb* kcp, void* user) {
        (void)kcp;
        KcpClient* client = static_cast<KcpClient*>(user);
        ::sendto(
            client->fd_,
            buf,
            static_cast<std::size_t>(len),
            0,
            reinterpret_cast<const sockaddr*>(&client->server_addr_),
            sizeof(client->server_addr_));
        return 0;
    }

    int fd_ = -1;
    ikcpcb* kcp_ = nullptr;
    sockaddr_in server_addr_{};
    std::vector<std::vector<std::uint8_t>> received_;
};

void TestSocketSmokeRace() {
    phk::battle::MatchLifecycleConfig config;
    config.port = 0;
    config.match_id = "match_socket_smoke";
    config.seed = 987654321u;
    config.player_ids = {"player_a", "player_b"};
    config.boss_max_hp = 100;

    phk::battle::MatchServer server(config);
    std::string error;
    Check(server.Start(&error), "match server starts (" + error + ")");
    Check(server.Port() != 0, "server bound to an ephemeral port");

    std::string captured_body;
    server.SetResultSubmitter([&](const std::string& json) {
        captured_body = json;
        phk::battle::HttpPostResult result;
        result.ok = true;
        result.status_code = 200;
        result.body = "{}";
        return result;
    });

    KcpClient client_a;
    KcpClient client_b;
    Check(client_a.Open("127.0.0.1", server.Port(), 0x0000A001u), "client_a opens kcp session");
    Check(client_b.Open("127.0.0.1", server.Port(), 0x0000B002u), "client_b opens kcp session");

    std::uint64_t seq_a = 0;
    std::uint64_t seq_b = 0;
    for (std::uint64_t tick = 1; tick <= 400 && !server.finished(); ++tick) {
        client_a.Send(phk::battle::EncodeBattleInputPayload(
            MakeInput(config.match_id, "player_a", tick, ++seq_a, true)));
        client_b.Send(phk::battle::EncodeBattleInputPayload(
            MakeInput(config.match_id, "player_b", tick, ++seq_b, false)));

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        server.ProcessNetwork(0);
        server.AdvanceTick();

        client_a.Pump();
        client_b.Pump();
    }

    Check(server.finished(), "race finished over real sockets");
    Check(server.Simulation().WinnerPlayerId() == "player_a", "socket race winner is player_a");
    Check(server.AcceptedInputCount() > 0, "server accepted kcp inputs");
    Check(server.BoundPlayerCount() == 2, "both players bound to sessions");
    Check(server.SnapshotBroadcastCount() > 0, "snapshots were broadcast");
    Check(server.ResultSubmitted(), "result submitted on finish");
    Check(captured_body.find("\"winner_player_id\":\"player_a\"") != std::string::npos,
          "captured result body has winner");
    Check(captured_body.find("\"match_id\":\"match_socket_smoke\"") != std::string::npos,
          "captured result body has match id");

    server.Stop();
}

// 合作模式 socket 级 e2e（契约 §6.3 + §4）：两个 KcpClient 真实会话喂射击输入
// → 共享 Boss（5000×2=10000 HP）归零 → Finished → 结果提交（coop 无单人
// winner）→ 且广播快照负载首字节 = 12（10/11 被 battle.proto 占用）。
void TestSocketSmokeCoop() {
    phk::battle::MatchLifecycleConfig config;
    config.port = 0;
    config.match_id = "match_socket_coop";
    config.seed = 424242u;
    config.mode_id = "coop_boss";
    config.player_ids = {"player_a", "player_b"};

    phk::battle::MatchServer server(config);
    std::string error;
    Check(server.Start(&error), "coop match server starts (" + error + ")");
    Check(server.IsCoop(), "match server runs the coop simulation");

    std::string captured_body;
    server.SetResultSubmitter([&](const std::string& json) {
        captured_body = json;
        phk::battle::HttpPostResult result;
        result.ok = true;
        result.status_code = 200;
        result.body = "{}";
        return result;
    });

    KcpClient client_a;
    KcpClient client_b;
    Check(client_a.Open("127.0.0.1", server.Port(), 0x0000C001u), "client_a opens kcp session");
    Check(client_b.Open("127.0.0.1", server.Port(), 0x0000C002u), "client_b opens kcp session");

    std::uint64_t seq_a = 0;
    std::uint64_t seq_b = 0;
    // 2p Boss HP=10000，双射击 10/tick；shield/cannon 窗口会拖慢，1200 tick 预算足够。
    for (std::uint64_t tick = 1; tick <= 1200 && !server.finished(); ++tick) {
        client_a.Send(phk::battle::EncodeBattleInputPayload(
            MakeInput(config.match_id, "player_a", tick, ++seq_a, true)));
        client_b.Send(phk::battle::EncodeBattleInputPayload(
            MakeInput(config.match_id, "player_b", tick, ++seq_b, true)));

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        server.ProcessNetwork(0);
        server.AdvanceTick();

        client_a.Pump();
        client_b.Pump();
    }

    Check(server.finished(), "coop finished over real sockets (boss defeated)");
    Check(server.CoopSimulation().BossDefeated(), "coop boss hp reached zero");
    Check(server.CoopSimulation().Snapshot().boss_max_hp == 10000u,
          "coop boss hp must be 5000 x players");
    Check(server.AcceptedInputCount() > 0, "server accepted kcp inputs");
    Check(server.BoundPlayerCount() == 2, "both players bound to sessions");
    Check(server.SnapshotBroadcastCount() > 0, "snapshots were broadcast");
    Check(server.ResultSubmitted(), "result submitted on finish");
    Check(captured_body.find("\"match_id\":\"match_socket_coop\"") != std::string::npos,
          "captured result body has match id");
    // 契约 §4：CoopSnapshot 负载字节 = 12。广播帧里必须出现该类型的快照。
    bool saw_coop_snapshot = false;
    for (std::size_t i = 0; i < client_a.ReceivedCount(); ++i) {
        const std::vector<std::uint8_t>& frame = client_a.Received(i);
        if (!frame.empty() &&
            frame[0] == static_cast<std::uint8_t>(phk::battle::BattlePayloadType::CoopSnapshot)) {
            saw_coop_snapshot = true;
        }
    }
    Check(saw_coop_snapshot, "broadcast coop snapshot payload byte is 12");

    server.Stop();
}

// ---------------------------------------------------------------------------
// 4. HTTP/1.1 POST client
// ---------------------------------------------------------------------------
class TinyHttpServer {
public:
    TinyHttpServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return;
        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        timeval tv{};
        tv.tv_sec = 5;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

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

    ~TinyHttpServer() { Stop(); }

    std::uint16_t port() const { return port_; }

    std::string body() {
        std::lock_guard<std::mutex> lock(mutex_);
        return body_;
    }

    std::string request_line() {
        std::lock_guard<std::mutex> lock(mutex_);
        return request_line_;
    }

private:
    void Serve() {
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
            const auto line_end = request.find("\r\n");
            request_line_ = request.substr(0, line_end);
            if (header_end != std::string::npos) {
                body_ = request.substr(header_end + 4);
            }
        }

        const char* response =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
        ::send(client, response, std::strlen(response), 0);
        ::close(client);
    }

    void Stop() {
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::string body_;
    std::string request_line_;
};

void TestHttpPostClient() {
    TinyHttpServer http;
    Check(http.port() != 0, "tiny http server listening");

    const std::string body = "{\"match_id\":\"m1\",\"winner_player_id\":\"player_a\"}";
    const auto result = phk::battle::HttpPostJson("127.0.0.1", http.port(), "/internal/battle/result", body);
    Check(result.ok, "http post succeeded (" + result.error + ")");
    Check(result.status_code == 200, "http status 200");
    Check(http.request_line() == "POST /internal/battle/result HTTP/1.1", "http request line");
    Check(http.body() == body, "http server received exact json body");

    std::string host;
    std::uint16_t port = 0;
    Check(phk::battle::ParseHostPort("127.0.0.1:8080", &host, &port), "parse host:port");
    Check(host == "127.0.0.1" && port == 8080, "parse host:port values");
    Check(!phk::battle::ParseHostPort("no-port", &host, &port), "reject endpoint without port");
}

// ---------------------------------------------------------------------------
// dem 录制（thread B）：app 路径输入入链 + Finalize 产出可解析容器
// ---------------------------------------------------------------------------
void TestDemRecordingFromAppPath() {
    phk::battle::MatchLifecycleConfig config;
    config.match_id = "match_dem_app_1";
    config.mode_id = "mvp_boss_race";
    config.seed = 424242;
    config.ruleset_version = "ruleset-local-s0";
    config.player_ids = {"p1", "p2"};
    config.require_ticket = phk::battle::MatchTicketRequirement::Off;
    config.boss_id = "tian";
    config.battle_server_id = "bs-dem-1";
    phk::battle::MatchServer server(config);
    std::string error;
    Check(server.Start(&error), "dem(app): server starts (" + error + ")");

    // 提交 2 条被接受的输入（tick 落在 +/-8 窗口内）。
    for (int index = 0; index < 2; ++index) {
        phk::battle::BattleInput input;
        input.match_id = config.match_id;
        input.player_id = index == 0 ? "p1" : "p2";
        input.tick = static_cast<std::uint64_t>(index + 1);
        input.seq = 1;
        input.direction_bits = static_cast<std::uint32_t>(index);
        input.shoot = true;
        input.card_slot = -1;
        const auto payload = phk::battle::EncodeBattleInputPayload(input);
        Check(server.InjectPayload(0x0000D001u + static_cast<std::uint32_t>(index), payload), "dem(app): inject input");
    }
    Check(server.AcceptedInputCount() == 2, "dem(app): two accepted inputs");
    Check(server.DemEntryCount() == 3, "dem(app): genesis match_seed + 2 inputs on chain");

    const auto written = server.FinalizeDem();
    Check(written.ok, "dem(app): finalize ok (" + written.reason + ")");
    if (!written.ok) {
        return;
    }
    const auto parsed = phk::battle::ParseDemDocument(written.bytes);
    Check(parsed.ok, "dem(app): produced container parses (" + parsed.reason + ")");
    if (!parsed.ok) {
        return;
    }
    Check(parsed.document.header.match_id == config.match_id, "dem(app): header match_id");
    Check(parsed.document.header.boss_id == "tian", "dem(app): header boss_id from config");
    Check(parsed.document.header.server_seed == "424242", "dem(app): header server_seed decimal");
    Check(parsed.document.header.entry_count == 3, "dem(app): header entry_count");
    Check(parsed.document.entries.size() == 3, "dem(app): body entry count");
    Check(parsed.document.entries[0].action_type == "match_seed", "dem(app): genesis entry first");
    Check(parsed.document.entries[1].player_id == "p1", "dem(app): first input player");
    Check(parsed.document.entries[2].player_id == "p2", "dem(app): second input player");
}

// Full match on the app path: race to a defeat, which makes the server submit
// its result and write the .dem the replay pipeline later serves. This is the
// end-to-end proof that a *finished* match produces a container (the unit above
// only finalises a hand-driven recorder).
void TestDemProducedWhenMatchFinishes() {
    const std::string dir = "/tmp/phk-dem-lifecycle";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    setenv("PHK_DEM_DIR", dir.c_str(), 1);
    setenv("PHK_DEM_UPLOAD", "0", 1);  // no lobby in the test environment

    phk::battle::MatchLifecycleConfig config;
    config.match_id = "match_dem_finish_1";
    config.mode_id = "mvp_boss_race";
    config.seed = 777001;
    config.ruleset_version = "ruleset-local-s0";
    config.player_ids = {"p1", "p2"};
    config.require_ticket = phk::battle::MatchTicketRequirement::Off;
    // No boss_id on purpose: the catalog HP would override boss_max_hp, and this
    // case wants a two-shot race so the server reaches Finished quickly.
    config.boss_max_hp = 20;
    config.battle_server_id = "bs-dem-1";
    config.max_ticks = 600;

    phk::battle::MatchServer server(config);
    std::string error;
    Check(server.Start(&error), "dem(finish): server starts (" + error + ")");

    std::uint64_t seq = 0;
    for (std::uint64_t tick = 1; tick <= 40 && !server.finished(); ++tick) {
        std::uint32_t conv = 0x0000D200u;
        for (const std::string& player : config.player_ids) {
            phk::battle::BattleInput input;
            input.match_id = config.match_id;
            input.player_id = player;
            input.tick = tick;
            input.seq = ++seq;
            input.direction_bits = 0;
            input.shoot = true;
            input.card_slot = -1;
            Check(
                server.InjectPayload(conv++, phk::battle::EncodeBattleInputPayload(input)),
                "dem(finish): inject input");
        }
        server.AdvanceTick();
    }
    Check(server.finished(), "dem(finish): race finished");
    Check(server.DemEntryCount() >= 3, "dem(finish): inputs are on the dem chain");

    const std::string path = dir + "/" + config.match_id + ".dem";
    std::ifstream in(path, std::ios::binary);
    Check(in.is_open(), "dem(finish): .dem written to " + path);
    if (!in.is_open()) {
        return;
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    Check(!bytes.empty(), "dem(finish): container is not empty");

    const auto parsed = phk::battle::ParseDemDocument(bytes);
    Check(parsed.ok, "dem(finish): container parses (" + parsed.reason + ")");
    if (!parsed.ok) {
        return;
    }
    Check(parsed.document.header.match_id == config.match_id, "dem(finish): header match_id");
    Check(
        parsed.document.header.ruleset_version == config.ruleset_version,
        "dem(finish): header ruleset_version");
    Check(parsed.document.header.boss_id.empty(), "dem(finish): default path records no boss id");
    Check(parsed.document.header.final_tick > 0, "dem(finish): final tick recorded");
    Check(!parsed.document.header.final_state_hash.empty(), "dem(finish): final state hash recorded");
    Check(
        parsed.document.header.entry_count == parsed.document.entries.size(),
        "dem(finish): header entry_count matches the body");
    Check(parsed.document.header.entry_count == server.DemEntryCount(), "dem(finish): entry count matches the recorder");
    std::filesystem::remove(path, ec);
}

// 结算权在 agent 的签名 RPC：--no-result-submit 下对局服一个 result POST 都
// 不能发（否则两条路径竞争，先到者写 replay_ 前缀 hash，后到者 409），但
// .dem 仍必须上传，结果内容仍必须可从 LastSubmission()/stdout 读到。
void TestNoResultSubmitSkipsPostButUploadsDem() {
    const std::string dir = "/tmp/phk-dem-noresult";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    setenv("PHK_DEM_DIR", dir.c_str(), 1);
    setenv("PHK_DEM_UPLOAD", "1", 1);

    TinyHttpServer http;
    Check(http.port() != 0, "no-result: dem upload listener is up");

    phk::battle::MatchLifecycleConfig config;
    config.match_id = "match_no_result_submit_1";
    config.mode_id = "mvp_boss_race";
    config.seed = 777002;
    config.ruleset_version = "ruleset-local-s0";
    config.player_ids = {"p1", "p2"};
    config.require_ticket = phk::battle::MatchTicketRequirement::Off;
    config.boss_max_hp = 20;
    config.battle_server_id = "bs-noresult-1";
    config.callback_secret = "noresult-secret";
    config.lobby_endpoint = "127.0.0.1:" + std::to_string(http.port());
    config.submit_result = false;
    config.max_ticks = 600;

    phk::battle::MatchServer server(config);
    std::string error;
    Check(server.Start(&error), "no-result: server starts (" + error + ")");

    int submitter_calls = 0;
    server.SetResultSubmitter([&](const std::string&) {
        ++submitter_calls;
        phk::battle::HttpPostResult result;
        result.ok = true;
        result.status_code = 200;
        result.body = "{}";
        return result;
    });

    std::uint64_t seq = 0;
    for (std::uint64_t tick = 1; tick <= 40 && !server.finished(); ++tick) {
        std::uint32_t conv = 0x0000D300u;
        for (const std::string& player : config.player_ids) {
            phk::battle::BattleInput input;
            input.match_id = config.match_id;
            input.player_id = player;
            input.tick = tick;
            input.seq = ++seq;
            input.direction_bits = 0;
            input.shoot = true;
            input.card_slot = -1;
            Check(
                server.InjectPayload(conv++, phk::battle::EncodeBattleInputPayload(input)),
                "no-result: inject input");
        }
        server.AdvanceTick();
    }

    Check(server.finished(), "no-result: race finished");
    Check(server.ResultSubmitted(), "no-result: result is marked available");
    Check(submitter_calls == 0, "no-result: result submitter was never called");
    Check(server.LastSubmission().status_code == 0, "no-result: no HTTP status recorded");
    Check(
        server.LastSubmission().body.find("\"winner_player_id\":\"p1\"") != std::string::npos ||
            server.LastSubmission().body.find("\"winner_player_id\":\"p2\"") != std::string::npos,
        "no-result: result content still carries the winner");

    const std::string path = dir + "/" + config.match_id + ".dem";
    Check(std::filesystem::exists(path), "no-result: .dem still written to " + path);
    Check(
        http.request_line() == "POST /v1/battle/replays/dem HTTP/1.1",
        "no-result: the only request is the .dem upload, got: " + http.request_line());

    std::filesystem::remove(path, ec);
    setenv("PHK_DEM_UPLOAD", "0", 1);  // 不把上传开关泄漏给后续用例
}

// ---------------------------------------------------------------------------
// F3：事件回传（all_players_disconnected → POST /v1/battle/events）
// ---------------------------------------------------------------------------
void TestEventsFlushAllPlayersDisconnected() {
    phk::battle::MatchLifecycleConfig config;
    config.port = 0;
    config.match_id = "match_events_000001";
    config.seed = 424242u;
    config.player_ids = {"player_a", "player_b"};
    config.boss_max_hp = 1000000000u;  // 别让对局打完

    phk::battle::MatchServer server(config);
    std::string error;
    Check(server.Start(&error), "events: server starts (" + error + ")");

    int submit_calls = 0;
    std::string captured_json;
    server.SetEventsSubmitter([&](const std::string& json) {
        ++submit_calls;
        captured_json = json;
        phk::battle::HttpPostResult result;
        result.ok = true;
        result.status_code = 200;
        result.body = "{}";
        return result;
    });

    // 两名玩家各打几拍输入建立活动记录，然后全部静默。
    std::uint64_t seq_a = 0;
    std::uint64_t seq_b = 0;
    for (std::uint64_t tick = 1; tick <= 8; ++tick) {
        Check(server.InjectPayload(0x0000E001u, phk::battle::EncodeBattleInputPayload(
            MakeInput(config.match_id, "player_a", tick, ++seq_a, false))),
            "events: inject player_a input");
        Check(server.InjectPayload(0x0000E002u, phk::battle::EncodeBattleInputPayload(
            MakeInput(config.match_id, "player_b", tick, ++seq_b, false))),
            "events: inject player_b input");
        server.AdvanceTick();
    }
    Check(submit_calls == 0, "events: nothing flushed while players are active");

    // 全员静默超过 3 秒窗口（60Hz × 3 = 180 tick）后应推一次事件并 flush。
    for (int i = 0; i < 200; ++i) {
        server.AdvanceTick();
    }
    Check(submit_calls >= 1, "events: flushed after full silence");
    Check(submit_calls == 1, "events: flushed exactly once (no spam)");
    Check(
        captured_json.find("\"all_players_disconnected\"") != std::string::npos,
        "events: json carries all_players_disconnected");
    Check(captured_json.find("match_events_000001") != std::string::npos, "events: match id present");
    // 对局级行不得携带 user_id / card_id（大厅 isMatchLevelBattleEvent 约束）。
    Check(captured_json.find("user_id") == std::string::npos, "events: no user_id field");
    Check(captured_json.find("card_id") == std::string::npos, "events: no card_id field");
    Check(captured_json.find("\"battle_server_id\":\"battle-local\"") != std::string::npos,
        "events: empty battle_server_id falls back to battle-local");

    // 事件去重：继续推进不再产生新的提交。
    for (int i = 0; i < 60; ++i) {
        server.AdvanceTick();
    }
    Check(submit_calls == 1, "events: event emitted only once");
}

// 失败路径：submitter 拒绝时行要放回、不丢，恢复后重发成功。
void TestEventsFlushRetriesAfterFailure() {
    phk::battle::MatchLifecycleConfig config;
    config.port = 0;
    config.match_id = "match_events_000002";
    config.seed = 777u;
    config.player_ids = {"player_a", "player_b"};
    config.boss_max_hp = 1000000000u;

    phk::battle::MatchServer server(config);
    std::string error;
    Check(server.Start(&error), "events-retry: server starts (" + error + ")");

    int calls = 0;
    int ok_from = 3;  // 前两次失败，之后成功
    std::string last_json;
    server.SetEventsSubmitter([&](const std::string& json) {
        ++calls;
        last_json = json;
        phk::battle::HttpPostResult result;
        result.ok = calls >= ok_from;
        result.status_code = result.ok ? 200 : 503;
        result.body = "{}";
        return result;
    });

    std::uint64_t seq_a = 0;
    std::uint64_t seq_b = 0;
    for (std::uint64_t tick = 1; tick <= 8; ++tick) {
        (void)server.InjectPayload(0x0000E101u, phk::battle::EncodeBattleInputPayload(
            MakeInput(config.match_id, "player_a", tick, ++seq_a, false)));
        (void)server.InjectPayload(0x0000E102u, phk::battle::EncodeBattleInputPayload(
            MakeInput(config.match_id, "player_b", tick, ++seq_b, false)));
        server.AdvanceTick();
    }
    for (int i = 0; i < 300; ++i) {  // 300 tick：覆盖事件触发(~188)+两次失败退避(40×2)
        server.AdvanceTick();
    }
    Check(calls >= ok_from, "events-retry: submitter eventually succeeded");
    Check(
        last_json.find("\"all_players_disconnected\"") != std::string::npos,
        "events-retry: retried body still carries the event");
}

}  // namespace

int main() {
    TestInputCodecRoundTrip();
    TestSnapshotJsonCarriesP1WireFields();
    ticket_test::TestTicketFrameCodec();
    ticket_test::TestTicketVerifiedBindsPlayer();
    ticket_test::TestTicketRejectionsRecordReason();
    ticket_test::TestTicketWarnAndOffModes();
    TestCardInputFieldsCodec();
    TestResultPayloadAndJson();
    TestSocketSmokeRace();
    TestSocketSmokeCoop();
    TestHttpPostClient();
    TestDemRecordingFromAppPath();
    TestDemProducedWhenMatchFinishes();
    TestNoResultSubmitSkipsPostButUploadsDem();
    TestEventsFlushAllPlayersDisconnected();
    TestEventsFlushRetriesAfterFailure();

    if (g_failures != 0) {
        std::cerr << g_failures << " lifecycle test(s) failed\n";
        return 1;
    }
    std::cout << "battle lifecycle tests passed\n";
    return 0;
}
