#include <cstdlib>
#include <array>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "phk/v1/manifest.hpp"
#include "phk/battle/kcp_endpoint.hpp"
#include "phk/battle/server.hpp"
#include "phk/battle/simulation.hpp"

namespace {

#define CHECK_TRUE(expr)                                                                 \
    do {                                                                                 \
        if (!(expr)) {                                                                   \
            std::cerr << "CHECK_TRUE failed: " #expr << " at line " << __LINE__ << '\n'; \
            return false;                                                                \
        }                                                                                \
    } while (false)

#define CHECK_EQ(left, right)                                                                        \
    do {                                                                                             \
        const auto left_value = (left);                                                              \
        const auto right_value = (right);                                                            \
        if (!(left_value == right_value)) {                                                          \
            std::cerr << "CHECK_EQ failed: " #left " != " #right << " at line " << __LINE__       \
                      << " (" << left_value << " vs " << right_value << ")\n";                    \
            return false;                                                                            \
        }                                                                                            \
    } while (false)

std::string RepeatHex(char ch, std::size_t count) {
    return std::string(count, ch);
}

void FillHandshakeBytes(phk::battle::BattleHandshakeHello& hello) {
    hello.client_x25519_pub[0] = 1;
    hello.client_random[0] = 2;
}

void FillDistinctHandshakeBytes(phk::battle::BattleHandshakeHello& hello) {
    for (std::size_t i = 0; i < hello.client_x25519_pub.size(); ++i) {
        hello.client_x25519_pub[i] = static_cast<std::uint8_t>(i + 1);
        hello.client_random[i] = static_cast<std::uint8_t>(0xf0u - i);
    }
}

std::string ExpectedDevResultHash(const phk::battle::ReplaySummary& summary) {
    return phk::battle::DevResultHashFromReplaySummary(summary);
}

std::string ExpectedDevReplayId(const phk::battle::ReplaySummary& summary) {
    return phk::battle::DevReplayIdFromReplaySummary(summary);
}

std::string ReplaySummaryHashForSummary(const phk::battle::ReplaySummary& summary) {
    phk::battle::ReplayInputStreamSummaryRecord record;
    record.replay_id = ExpectedDevReplayId(summary);
    record.match_id = summary.match_id;
    record.input_count = summary.input_count;
    record.event_count = summary.event_count;
    record.input_stream_hash = summary.input_stream_hash;
    record.event_stream_hash = summary.event_stream_hash;
    record.final_state_hash = summary.final_state_hash;
    record.match_seed = summary.match_seed;
    record.final_tick = summary.final_tick;
    return phk::battle::DevReplayInputStreamSummaryHash(record);
}

std::string ReplaceFirst(std::string value, const std::string& old_value, const std::string& new_value) {
    const auto offset = value.find(old_value);
    if (offset != std::string::npos) {
        value.replace(offset, old_value.size(), new_value);
    }
    return value;
}

std::string ReplaceJsonStringField(
    std::string value,
    const std::string& field_name,
    const std::string& new_value
) {
    const std::string prefix = "\"" + field_name + "\":\"";
    const auto value_start = value.find(prefix);
    if (value_start == std::string::npos) {
        return value;
    }
    const auto string_start = value_start + prefix.size();
    const auto string_end = value.find('"', string_start);
    if (string_end == std::string::npos) {
        return value;
    }
    value.replace(string_start, string_end - string_start, new_value);
    return value;
}

phk::battle::SignedBattleTicket MakeTicket() {
	phk::battle::SignedBattleTicket signed_ticket;
    signed_ticket.ticket.ticket_id = "ticket-001";
    signed_ticket.ticket.match_id = "match-001";
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
    signed_ticket.key_id = "dev-ed25519-local";
    signed_ticket.public_key_hex = RepeatHex('a', 64);
    signed_ticket.signature_hex = RepeatHex('b', 128);
    signed_ticket.server_authoritative = true;
	return signed_ticket;
}

phk::battle::SignedBattleTicket MakeTicketForBob() {
	auto ticket = MakeTicket();
	ticket.ticket.ticket_id = "ticket-002";
	ticket.ticket.user_id = "user-bob";
	ticket.ticket.player_id = "p2";
	ticket.ticket.business_session_id = "session-ref:dev-bob";
	ticket.ticket.ticket_nonce_hex = "ffeeddccbbaa998877665544";
	return ticket;
}

phk::battle::SignedBattleTicket MakeModeTicket(
    std::string ticket_id,
    std::string user_id,
    std::string player_id,
    std::string mode_id,
    std::string nonce_hex
) {
    auto ticket = MakeTicket();
    ticket.ticket.ticket_id = std::move(ticket_id);
    ticket.ticket.user_id = std::move(user_id);
    ticket.ticket.player_id = std::move(player_id);
    ticket.ticket.mode_id = std::move(mode_id);
    ticket.ticket.ticket_nonce_hex = std::move(nonce_hex);
    ticket.ticket.business_session_id = "session-ref:" + ticket.ticket.user_id;
    return ticket;
}

phk::battle::BattleInput MakeInput(
    const std::string& player_id,
    std::uint64_t tick,
    std::uint64_t seq,
    std::uint32_t direction_bits
);

phk::battle::BattleModeAction MakeModeAction(std::uint64_t seq);

void FillEncryptedHeaderShape(phk::battle::BattlePacketHeader& header) {
    header.key_id = "battle-local-1";
    header.nonce_hex = phk::battle::DevAeadNonceHex(header);
}

void RefreshDevAeadNonce(phk::battle::BattlePacketHeader& header) {
    header.nonce_hex = phk::battle::DevAeadNonceHex(header);
}

phk::battle::BattleHandshakeAccept AcceptHandshakeForTicket(
    phk::battle::BattleServer& server,
    phk::battle::SignedBattleTicket ticket
) {
    phk::battle::BattleHandshakeHello hello;
    hello.battle_ticket = std::move(ticket);
    FillDistinctHandshakeBytes(hello);
    hello.supported_aead = {"CHACHA20_POLY1305"};
    return server.AcceptHandshake(hello);
}

phk::battle::BattleHandshakeAccept AcceptDefaultHandshake(phk::battle::BattleServer& server) {
    return AcceptHandshakeForTicket(server, MakeTicket());
}

phk::battle::SimulationConfig MakeAuthoritativeReplay60Config(std::string match_id) {
    phk::battle::SimulationConfig config;
    config.match_id = std::move(match_id);
    config.mode_id = "pvp_duel";
    config.match_seed = 424242;
    config.max_input_ahead_ticks = 16;
    config.max_seq_ahead = 128;
    config.spawn_period_ticks = 15;
    return config;
}

bool AddReplayFixturePlayers(phk::battle::BattleSimulation& simulation) {
    CHECK_TRUE(simulation.AddPlayer("p1", -20000, 0));
    CHECK_TRUE(simulation.AddPlayer("p2", 20000, 0));
    return true;
}

bool AddBossFixturePlayers(phk::battle::BattleSimulation& simulation) {
    CHECK_TRUE(simulation.AddPlayer("p1", 0, -60000));
    CHECK_TRUE(simulation.AddPlayer("p2", 60000, 0));
    CHECK_TRUE(simulation.AddPlayer("p3", 0, 60000));
    CHECK_TRUE(simulation.AddPlayer("p4", -60000, 0));
    return true;
}

bool AcceptBossReadyActions(
    phk::battle::BattleSimulation& simulation,
    const std::string& match_id,
    std::uint64_t tick,
    std::uint64_t p1_seq,
    std::uint64_t p2_seq,
    std::uint64_t p3_seq,
    std::uint64_t p4_seq,
    const std::string& action_prefix
) {
    const std::array<std::uint64_t, 4> seqs = {p1_seq, p2_seq, p3_seq, p4_seq};
    for (std::size_t index = 1; index <= 4; ++index) {
        auto ready = MakeModeAction(seqs[index - 1]);
        ready.match_id = match_id;
        ready.player_id = "p" + std::to_string(index);
        ready.tick = tick;
        ready.action_id = action_prefix + "-" + std::to_string(index);
        ready.action_type = "ready";
        ready.payload_json = "{\"ready\":true}";
        CHECK_TRUE(simulation.AcceptModeAction(ready).ok);
    }
    return true;
}

bool DriveAuthoritativeReplay60Ticks(
    phk::battle::BattleSimulation& first,
    phk::battle::BattleSimulation* second = nullptr
) {
    for (std::uint64_t tick = 1; tick <= 60; ++tick) {
        std::uint32_t p1_direction = (tick <= 30) ? (1u << 3) : (1u << 0);
        std::uint32_t p2_direction = (tick <= 30) ? (1u << 2) : (1u << 1);
        auto p1_input = MakeInput("p1", tick, tick, p1_direction);
        auto p2_input = MakeInput("p2", tick, tick, p2_direction);
        p1_input.match_id = first.Config().match_id;
        p2_input.match_id = first.Config().match_id;
        p1_input.slow = (tick % 2) == 0;
        p2_input.slow = (tick % 3) == 0;
        CHECK_TRUE(first.AcceptInput(p1_input).ok);
        CHECK_TRUE(first.AcceptInput(p2_input).ok);
        if (second != nullptr) {
            auto second_p1_input = p1_input;
            auto second_p2_input = p2_input;
            second_p1_input.match_id = second->Config().match_id;
            second_p2_input.match_id = second->Config().match_id;
            CHECK_TRUE(second->AcceptInput(second_p1_input).ok);
            CHECK_TRUE(second->AcceptInput(second_p2_input).ok);
            CHECK_EQ(first.Tick().state_hash, second->Tick().state_hash);
        } else {
            first.Tick();
        }
    }
    return true;
}

bool TestTicketVerifier() {
	phk::battle::TicketVerifier verifier;
    phk::battle::TicketVerificationOptions options;
    options.now_ms = 1782489605000;
    options.required_battle_server_id = "battle-local-1";
    options.required_endpoint = "127.0.0.1:7901";
    options.required_key_id = "dev-ed25519-local";

    const auto good = verifier.Verify(MakeTicket(), options);
    CHECK_TRUE(good.ok);
    CHECK_EQ(good.reason, std::string("ok"));
    CHECK_TRUE(!good.warnings.empty());

    auto expired = MakeTicket();
    expired.ticket.expires_at_ms = 1782489604000;
    const auto expired_result = verifier.Verify(expired, options);
    CHECK_TRUE(!expired_result.ok);
    CHECK_EQ(expired_result.reason, std::string("ticket_expired"));

    auto wrong_server = MakeTicket();
    wrong_server.ticket.battle_server_id = "other";
    const auto wrong_server_result = verifier.Verify(wrong_server, options);
    CHECK_TRUE(!wrong_server_result.ok);
    CHECK_EQ(wrong_server_result.reason, std::string("battle_server_mismatch"));

    auto raw_session = MakeTicket();
    raw_session.ticket.business_session_id = "Bearer raw-token";
    const auto raw_session_result = verifier.Verify(raw_session, options);
    CHECK_TRUE(!raw_session_result.ok);
    CHECK_EQ(raw_session_result.reason, std::string("raw_business_session_rejected"));

    auto ruleset_mismatch = MakeTicket();
    ruleset_mismatch.ticket.version.ruleset_version = "ruleset-other";
    const auto ruleset_mismatch_result = verifier.Verify(ruleset_mismatch, options);
    CHECK_TRUE(!ruleset_mismatch_result.ok);
    CHECK_EQ(ruleset_mismatch_result.reason, std::string("ruleset_version_mismatch"));

    auto missing_identity = MakeTicket();
    missing_identity.ticket.match_id.clear();
    const auto missing_identity_result = verifier.Verify(missing_identity, options);
    CHECK_TRUE(!missing_identity_result.ok);
    CHECK_EQ(missing_identity_result.reason, std::string("ticket_identity_missing"));
    return true;
}

bool TestProtocolManifest() {
    phk::battle::VersionStamp version;
    CHECK_EQ(version.protocol_version, phk::v1::kProtocolVersion);
    CHECK_EQ(version.business_api_version, std::string(phk::v1::kBusinessApiVersion));
    CHECK_EQ(version.battle_api_version, std::string(phk::v1::kBattleApiVersion));
    CHECK_EQ(version.ruleset_version, std::string(phk::v1::kRulesetVersion));
    CHECK_EQ(std::string(phk::battle::kRulesetHash), std::string(phk::v1::kRulesetHash));
    CHECK_EQ(static_cast<int>(phk::battle::BattlePayloadType::Unspecified), 0);
    CHECK_EQ(static_cast<int>(phk::battle::BattlePayloadType::HandshakeHello), 1);
    CHECK_EQ(static_cast<int>(phk::battle::BattlePayloadType::HandshakeAccept), 2);
    CHECK_EQ(static_cast<int>(phk::battle::BattlePayloadType::Input), 3);
    CHECK_EQ(static_cast<int>(phk::battle::BattlePayloadType::Snapshot), 4);
    CHECK_EQ(static_cast<int>(phk::battle::BattlePayloadType::Event), 5);
    CHECK_EQ(static_cast<int>(phk::battle::BattlePayloadType::Ping), 6);
    CHECK_EQ(static_cast<int>(phk::battle::BattlePayloadType::Reconnect), 7);
    CHECK_EQ(static_cast<int>(phk::battle::BattlePayloadType::Result), 8);
    CHECK_EQ(static_cast<int>(phk::battle::BattlePayloadType::ModeAction), 9);
    CHECK_EQ(std::string(phk::v1::kBattleModeActionActionType), std::string("select_round_card"));
    CHECK_EQ(
        std::string(phk::v1::kBattleModeActionPayloadJson),
        std::string("{\"card_id\":\"focus_lens\",\"round_index\":0}")
    );
    CHECK_TRUE(phk::v1::HasMessageField("BattleTicket", "ruleset_version"));
    CHECK_TRUE(phk::v1::HasMessageField("BattlePacketHeader", "seq"));
    CHECK_TRUE(phk::v1::HasMessageField("BattlePacketHeader", "nonce"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleHandshakeHello", "client_x25519_pub"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleHandshakeAccept", "server_signature"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleEncryptedPacket", "auth_tag"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleInput", "direction_bits"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleModeAction", "client_result_authoritative"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleSnapshot", "mode_state"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleResult", "player_ids"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleResult", "reward_projection_json"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleResult", "mode_result_json"));
    CHECK_TRUE(phk::v1::HasMessageField("SignedBattleTicket", "signature"));
    CHECK_TRUE(phk::v1::HasMessageField("SignedBattleResult", "signature"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleResultSubmitRequest", "signed_result"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleResultSubmitRequest", "replay_summary"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleResultSubmitResponse", "settlement_key"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleResultSubmitResponse", "accepted"));
    CHECK_TRUE(phk::v1::HasMessageField("ReplayInputStreamSummary", "final_state_hash"));
    CHECK_TRUE(phk::v1::HasMessageField("ReplayInputStreamSummary", "final_tick"));
    CHECK_TRUE(phk::v1::HasMessageField("ReplayRecord", "stream"));
    CHECK_TRUE(phk::v1::HasMessageField("ReplayRecord", "settlement"));
    CHECK_TRUE(phk::v1::HasMessageField("ReplayRecord", "server_authoritative"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleResult", "result_hash"));
    CHECK_TRUE(phk::v1::MessageFieldCount("BattleHandshakeHello") >= 5);
    CHECK_TRUE(phk::v1::MessageFieldCount("BattleHandshakeAccept") >= 10);
    CHECK_TRUE(phk::v1::MessageFieldCount("BattleInput") >= 10);
    CHECK_TRUE(phk::v1::MessageFieldCount("BattleResult") >= 9);
    CHECK_TRUE(phk::v1::MessageFieldCount("BattleResultSubmitRequest") >= 2);
    CHECK_TRUE(phk::v1::MessageFieldCount("ReplayInputStreamSummary") >= 10);
    CHECK_TRUE(phk::v1::MessageFieldCount("ReplayRecord") >= 10);
    return true;
}

bool TestSharedSnapshotEventFixtures() {
    phk::battle::BattleSnapshot snapshot;
    snapshot.match_id = std::string(phk::v1::kBattleSnapshotMatchId);
    snapshot.snapshot_tick = phk::v1::kBattleSnapshotSnapshotTick;
    snapshot.snapshot_kind = std::string(phk::v1::kBattleSnapshotSnapshotKind);
    snapshot.state_hash = std::string(phk::v1::kBattleSnapshotStateHash);
    snapshot.event_cursor = phk::v1::kBattleSnapshotEventCursor;

    phk::battle::BattlePlayerSnapshot player;
    player.player_id = "p1";
    player.x_milli = 120000;
    player.y_milli = 300000;
    player.connected = true;
    player.hand_size = 4;
    snapshot.players.push_back(player);

    phk::battle::BattleBulletDelta bullet;
    bullet.bullet_id = "b-001";
    bullet.op = "spawn";
    bullet.x_milli = 120000;
    bullet.y_milli = 300000;
    bullet.vx_milli = 0;
    bullet.vy_milli = -350;
    bullet.radius_milli = 5000;
    bullet.pattern_id = "opening_fan";
    bullet.color = "blue";
    snapshot.bullets_delta.push_back(bullet);
    snapshot.mode_state["duel_status"] = "running";

    CHECK_EQ(snapshot.match_id, std::string("match-001"));
    CHECK_EQ(snapshot.snapshot_tick, static_cast<std::uint64_t>(122));
    CHECK_EQ(snapshot.snapshot_kind, std::string("delta"));
    CHECK_EQ(snapshot.state_hash, std::string("sha256:state"));
    CHECK_EQ(snapshot.event_cursor, static_cast<std::uint64_t>(4));
    CHECK_TRUE(!snapshot.players.empty());
    CHECK_TRUE(!snapshot.bullets_delta.empty());
    CHECK_EQ(snapshot.mode_state.at("duel_status"), std::string("running"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleSnapshot", "event_cursor"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleEvent", "server_authoritative"));

    phk::battle::BattlePacketHeader event_header;
    event_header.match_id = std::string(phk::v1::kBattleEventMatchId);
    event_header.tick = phk::v1::kBattleEventTick;
    event_header.seq = phk::v1::kBattleEventCursor;
    event_header.payload_type = phk::battle::BattlePayloadType::Event;
    CHECK_EQ(event_header.match_id, snapshot.match_id);
    CHECK_EQ(event_header.tick, phk::v1::kBattleSnapshotSnapshotTick);
    CHECK_EQ(event_header.seq, phk::v1::kBattleSnapshotEventCursor);
    CHECK_EQ(std::string(phk::v1::kBattleEventType), std::string("bullet_spawn"));
    CHECK_TRUE(phk::v1::kBattleEventServerAuthoritative);
    CHECK_EQ(phk::battle::PayloadTypeName(event_header.payload_type), std::string("event"));
    return true;
}

bool TestGoldenReplaySummaryFixture() {
    phk::battle::ReplaySummary summary;
    summary.match_id = std::string(phk::v1::kGoldenReplaySummaryMatchId);
    summary.input_count = phk::v1::kGoldenReplaySummaryInputCount;
    summary.event_count = phk::v1::kGoldenReplaySummaryEventCount;
    summary.input_stream_hash = std::string(phk::v1::kGoldenReplaySummaryInputStreamHash);
    summary.event_stream_hash = std::string(phk::v1::kGoldenReplaySummaryEventStreamHash);
    summary.final_state_hash = std::string(phk::v1::kGoldenReplaySummaryFinalStateHash);
    summary.final_tick = phk::v1::kGoldenReplaySummaryFinalTick;

    CHECK_EQ(std::string(phk::v1::kGoldenReplaySummaryReplayId), std::string(phk::v1::kBattleResultCallbackReplayId));
    CHECK_EQ(summary.match_id, std::string(phk::v1::kBattleResultCallbackMatchId));
    CHECK_EQ(std::string(phk::v1::kGoldenReplaySummaryOwnerUserId), std::string("user-alice"));
    CHECK_EQ(std::string(phk::v1::kBattleResultCallbackResultHash), std::string("sha256:0123456789abcdef"));
    CHECK_EQ(
        std::string(phk::v1::kBattleResultCallbackRewardProjectionJson),
        std::string("{\"source\":\"battle-server\"}")
    );
    CHECK_EQ(
        std::string(phk::v1::kBattleResultCallbackModeResultJson),
        std::string("{\"battle_result_owner\":\"cpp\"}")
    );
    CHECK_EQ(phk::v1::kBattleResultCallbackSettledAtMs, static_cast<std::int64_t>(1782489610000));
    CHECK_EQ(std::string(phk::v1::kBattleResultCallbackKeyId), std::string("battle-local-1"));
    CHECK_EQ(std::string(phk::v1::kBattleResultCallbackPublicKeyHex).size(), static_cast<std::size_t>(64));
    CHECK_EQ(std::string(phk::v1::kBattleResultCallbackSignatureHex).size(), static_cast<std::size_t>(128));
    CHECK_EQ(summary.input_count, static_cast<std::uint64_t>(2));
    CHECK_EQ(summary.fallback_input_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(summary.neutral_fallback_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(summary.held_input_fallback_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(summary.mode_action_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(summary.event_count, static_cast<std::uint64_t>(4));
    CHECK_EQ(summary.final_tick, phk::v1::kBattleSnapshotSnapshotTick);
    CHECK_EQ(summary.final_state_hash, std::string(phk::v1::kBattleSnapshotStateHash));
    CHECK_TRUE(summary.input_stream_hash.rfind("sha256:", 0) == 0);
    CHECK_TRUE(summary.event_stream_hash.rfind("sha256:", 0) == 0);
    CHECK_TRUE(summary.final_state_hash.rfind("sha256:", 0) == 0);
    CHECK_TRUE(phk::v1::HasMessageField("ReplayInputStreamSummary", "input_stream_hash"));
    CHECK_TRUE(phk::v1::HasMessageField("ReplayInputStreamSummary", "final_tick"));

    phk::battle::SimulationConfig config;
    config.match_id = summary.match_id;
    config.mode_id = std::string(phk::v1::kBattleResultCallbackModeId);
    phk::battle::BattleSimulation simulation(config);
    CHECK_EQ(simulation.Summary().mode_id, std::string(phk::v1::kBattleResultCallbackModeId));
    CHECK_EQ(simulation.Summary().ruleset_version, std::string(phk::v1::kRulesetVersion));
    CHECK_TRUE(simulation.Summary().input_stream_hash.rfind("fnv64:", 0) == 0);
    CHECK_TRUE(simulation.Summary().event_stream_hash.rfind("fnv64:", 0) == 0);
    CHECK_TRUE(!simulation.Summary().final_state_hash.empty());
    CHECK_TRUE(simulation.Summary().input_trace.empty());
    CHECK_TRUE(simulation.Summary().event_trace.empty());
    return true;
}

bool TestServerAndHandshake() {
	phk::battle::BattleServerConfig config;
	config.now_ms = 1782489605000;
	phk::battle::BattleServer server(config);

	auto ticket = MakeTicket();
	const auto registered = server.RegisterTicket(ticket);
	CHECK_TRUE(registered.ok);
    CHECK_TRUE(registered.created_match);
    CHECK_EQ(registered.active_sessions_before, static_cast<std::size_t>(0));
    CHECK_EQ(registered.active_matches_before, static_cast<std::size_t>(0));
    CHECK_EQ(registered.match_session_count_before, static_cast<std::size_t>(0));
    CHECK_EQ(registered.active_sessions_after, static_cast<std::size_t>(1));
    CHECK_EQ(registered.active_matches_after, static_cast<std::size_t>(1));
    CHECK_EQ(registered.match_session_count_after, static_cast<std::size_t>(1));
	CHECK_EQ(server.ActiveSessionCount(), static_cast<std::size_t>(1));
	CHECK_EQ(registered.session.kcp_conv, phk::battle::DeriveDevKcpConv("match-001", "p1"));

	const auto replay = server.RegisterTicket(ticket);
	CHECK_TRUE(!replay.ok);
	CHECK_EQ(replay.reason, std::string("ticket_replay"));
    CHECK_TRUE(!replay.created_match);
    CHECK_EQ(replay.active_sessions_before, static_cast<std::size_t>(1));
    CHECK_EQ(replay.active_sessions_after, static_cast<std::size_t>(1));
    CHECK_EQ(replay.match_session_count_before, static_cast<std::size_t>(1));
    CHECK_EQ(replay.match_session_count_after, static_cast<std::size_t>(1));

	auto duplicate_player = MakeTicket();
	duplicate_player.ticket.ticket_id = "ticket-duplicate-player";
	duplicate_player.ticket.ticket_nonce_hex = "1234567890abcdef12345678";
	const auto duplicate_player_result = server.RegisterTicket(duplicate_player);
	CHECK_TRUE(!duplicate_player_result.ok);
	CHECK_EQ(duplicate_player_result.reason, std::string("player_session_replay"));
    CHECK_TRUE(!duplicate_player_result.created_match);
    CHECK_EQ(duplicate_player_result.active_sessions_after, static_cast<std::size_t>(1));
    CHECK_EQ(duplicate_player_result.active_matches_after, static_cast<std::size_t>(1));
    CHECK_EQ(duplicate_player_result.match_session_count_after, static_cast<std::size_t>(1));

    auto wrong_mode = MakeTicket();
    wrong_mode.ticket.ticket_id = "ticket-wrong-mode";
    wrong_mode.ticket.user_id = "user-eve";
    wrong_mode.ticket.player_id = "p3";
    wrong_mode.ticket.mode_id = "battle_royale";
    wrong_mode.ticket.ticket_nonce_hex = "abcdefabcdefabcdefabcdef";
    wrong_mode.ticket.business_session_id = "session-ref:dev-eve";
    const auto wrong_mode_result = server.RegisterTicket(wrong_mode);
    CHECK_TRUE(!wrong_mode_result.ok);
    CHECK_EQ(wrong_mode_result.reason, std::string("match_mode_ruleset_mismatch"));
    CHECK_TRUE(!wrong_mode_result.created_match);
    CHECK_EQ(wrong_mode_result.active_sessions_before, static_cast<std::size_t>(1));
    CHECK_EQ(wrong_mode_result.active_sessions_after, static_cast<std::size_t>(1));
    CHECK_EQ(wrong_mode_result.active_matches_after, static_cast<std::size_t>(1));
    CHECK_EQ(wrong_mode_result.match_session_count_after, static_cast<std::size_t>(1));
    CHECK_EQ(server.ActiveSessionCount(), static_cast<std::size_t>(1));

	const auto registered_bob = server.RegisterTicket(MakeTicketForBob());
	CHECK_TRUE(registered_bob.ok);
    CHECK_TRUE(!registered_bob.created_match);
    CHECK_EQ(registered_bob.active_sessions_before, static_cast<std::size_t>(1));
    CHECK_EQ(registered_bob.active_matches_before, static_cast<std::size_t>(1));
    CHECK_EQ(registered_bob.match_session_count_before, static_cast<std::size_t>(1));
    CHECK_EQ(registered_bob.active_sessions_after, static_cast<std::size_t>(2));
    CHECK_EQ(registered_bob.active_matches_after, static_cast<std::size_t>(1));
    CHECK_EQ(registered_bob.match_session_count_after, static_cast<std::size_t>(2));
	CHECK_EQ(server.ActiveSessionCount(), static_cast<std::size_t>(2));

	phk::battle::BattleHandshakeHello hello;
    hello.battle_ticket = ticket;
    FillHandshakeBytes(hello);
    hello.supported_aead = {"AES_256_GCM", "CHACHA20_POLY1305"};
    const auto accept = server.AcceptHandshake(hello);
    CHECK_TRUE(accept.ok);
    CHECK_EQ(accept.reason, std::string("ok"));
    CHECK_EQ(accept.match_id, std::string("match-001"));
    CHECK_EQ(accept.player_id, std::string("p1"));
    CHECK_EQ(accept.selected_aead, std::string("CHACHA20_POLY1305"));
    CHECK_TRUE(!accept.transcript_hash_hex.empty());
    CHECK_EQ(accept.transcript_hash_hex.size(), static_cast<std::size_t>(32));
    CHECK_EQ(accept.client_to_server_key_ref.rfind("hkdf-dev:client_to_server:", 0), static_cast<std::size_t>(0));
    CHECK_EQ(accept.server_to_client_key_ref.rfind("hkdf-dev:server_to_client:", 0), static_cast<std::size_t>(0));
    CHECK_TRUE(accept.client_to_server_key_ref != accept.server_to_client_key_ref);
    CHECK_EQ(accept.server_signature_alg, std::string("ED25519"));
    CHECK_EQ(accept.server_signature_hex.size(), static_cast<std::size_t>(128));
    CHECK_TRUE(phk::battle::IsHex(accept.server_signature_hex));
    CHECK_EQ(
        accept.server_signature_hex,
        phk::battle::DevHandshakeServerSignature(accept.transcript_hash_hex, config.signing_key_id)
    );
	CHECK_TRUE(!accept.dev_session_id.empty());

    phk::battle::BattleHandshakeHello xchacha_hello = hello;
    xchacha_hello.battle_ticket.ticket.ticket_id = "ticket-002";
    xchacha_hello.battle_ticket.ticket.user_id = "user-bob";
    xchacha_hello.battle_ticket.ticket.player_id = "p2";
    xchacha_hello.battle_ticket.ticket.business_session_id = "session-ref:dev-bob";
    xchacha_hello.battle_ticket.ticket.ticket_nonce_hex = "ffeeddccbbaa998877665544";
    xchacha_hello.supported_aead = {"XCHACHA20_POLY1305"};
    const auto xchacha_accept = server.AcceptHandshake(xchacha_hello);
    CHECK_TRUE(xchacha_accept.ok);
    CHECK_EQ(xchacha_accept.selected_aead, std::string("XCHACHA20_POLY1305"));
    CHECK_EQ(xchacha_accept.server_signature_hex.size(), static_cast<std::size_t>(128));
    CHECK_TRUE(xchacha_accept.client_to_server_key_ref != accept.client_to_server_key_ref);

    phk::battle::BattleHandshakeHello missing_key_hello = hello;
    missing_key_hello.client_x25519_pub = {};
    const auto missing_key_accept = server.AcceptHandshake(missing_key_hello);
    CHECK_TRUE(!missing_key_accept.ok);
    CHECK_EQ(missing_key_accept.reason, std::string("client_key_missing"));

    phk::battle::BattleHandshakeHello unsupported_aead_hello = hello;
    unsupported_aead_hello.supported_aead = {"AES_256_GCM"};
    const auto unsupported_aead_accept = server.AcceptHandshake(unsupported_aead_hello);
    CHECK_TRUE(!unsupported_aead_accept.ok);
    CHECK_EQ(unsupported_aead_accept.reason, std::string("aead_unsupported"));

    auto unregistered_ticket = MakeTicket();
    unregistered_ticket.ticket.ticket_id = "ticket-unregistered";
    unregistered_ticket.ticket.ticket_nonce_hex = "111111111111111111111111";
    phk::battle::BattleHandshakeHello unregistered_hello;
    unregistered_hello.battle_ticket = unregistered_ticket;
    FillHandshakeBytes(unregistered_hello);
    unregistered_hello.supported_aead = {"CHACHA20_POLY1305"};
    const auto unregistered_accept = server.AcceptHandshake(unregistered_hello);
    CHECK_TRUE(!unregistered_accept.ok);
    CHECK_EQ(unregistered_accept.reason, std::string("ticket_not_registered"));

    auto expired_ticket = ticket;
    expired_ticket.ticket.expires_at_ms = 1782489604000;
    phk::battle::BattleHandshakeHello expired_hello;
    expired_hello.battle_ticket = expired_ticket;
    FillHandshakeBytes(expired_hello);
    expired_hello.supported_aead = {"CHACHA20_POLY1305"};
    const auto expired_accept = server.AcceptHandshake(expired_hello);
    CHECK_TRUE(!expired_accept.ok);
    CHECK_EQ(expired_accept.reason, std::string("ticket_expired"));
	return true;
}

bool TestHandshakeTranscriptBindsFullClientMaterial() {
    const auto ticket = MakeTicket();

    phk::battle::BattleHandshakeHello hello;
    hello.battle_ticket = ticket;
    FillDistinctHandshakeBytes(hello);
    hello.supported_aead = {"CHACHA20_POLY1305", "XCHACHA20_POLY1305"};

    phk::battle::BattleHandshakeHello changed_late_key = hello;
    changed_late_key.client_x25519_pub[31] ^= 0x5au;
    CHECK_TRUE(phk::battle::DevTranscriptHash(hello, ticket.ticket) !=
        phk::battle::DevTranscriptHash(changed_late_key, ticket.ticket));

    phk::battle::BattleHandshakeHello changed_late_random = hello;
    changed_late_random.client_random[30] ^= 0xa5u;
    CHECK_TRUE(phk::battle::DevTranscriptHash(hello, ticket.ticket) !=
        phk::battle::DevTranscriptHash(changed_late_random, ticket.ticket));

    phk::battle::BattleHandshakeHello reordered_aead = hello;
    reordered_aead.supported_aead = {"XCHACHA20_POLY1305", "CHACHA20_POLY1305"};
    CHECK_TRUE(phk::battle::DevTranscriptHash(hello, ticket.ticket) !=
        phk::battle::DevTranscriptHash(reordered_aead, ticket.ticket));

    const auto transcript = phk::battle::DevTranscriptHash(hello, ticket.ticket);
    CHECK_EQ(transcript.size(), static_cast<std::size_t>(32));
    CHECK_TRUE(phk::battle::IsHex(transcript));
    CHECK_TRUE(
        phk::battle::DevHandshakeKeyRef(transcript, "client_to_server") !=
        phk::battle::DevHandshakeKeyRef(transcript, "server_to_client")
    );
    CHECK_EQ(
        phk::battle::DevHandshakeServerSignature(transcript, "dev-ed25519-local").size(),
        static_cast<std::size_t>(128)
    );
    return true;
}

bool TestRoomCapacityGuard() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489605000;
    config.max_players = 1;
    phk::battle::BattleServer server(config);

    CHECK_TRUE(server.RegisterTicket(MakeTicket()).ok);
    const auto full = server.RegisterTicket(MakeTicketForBob());
    CHECK_TRUE(!full.ok);
    CHECK_EQ(full.reason, std::string("match_full"));
    CHECK_TRUE(!full.created_match);
    CHECK_EQ(full.active_sessions_before, static_cast<std::size_t>(1));
    CHECK_EQ(full.active_matches_before, static_cast<std::size_t>(1));
    CHECK_EQ(full.match_session_count_before, static_cast<std::size_t>(1));
    CHECK_EQ(full.active_sessions_after, static_cast<std::size_t>(1));
    CHECK_EQ(full.active_matches_after, static_cast<std::size_t>(1));
    CHECK_EQ(full.match_session_count_after, static_cast<std::size_t>(1));
    CHECK_EQ(server.ActiveSessionCount(), static_cast<std::size_t>(1));
    return true;
}

bool TestBattleResultSubmission() {
	phk::battle::BattleServerConfig config;
	config.now_ms = 1782489620000;
	phk::battle::BattleServer server(config);
    CHECK_TRUE(server.RegisterTicket(MakeTicket()).ok);
    CHECK_TRUE(server.RegisterTicket(MakeTicketForBob()).ok);
    auto action = MakeModeAction(1);
    action.tick = 1;
    CHECK_TRUE(server.AcceptModeAction(action).ok);
    CHECK_EQ(server.MatchReplaySummary("match-001").event_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(server.TickMatch("match-001").snapshot_tick, static_cast<std::uint64_t>(1));
    const auto summary = server.MatchReplaySummary("match-001");
    CHECK_EQ(summary.event_count, static_cast<std::uint64_t>(1));
    const auto built_result = server.BuildSignedBattleResult("match-001");
    CHECK_TRUE(built_result.ok);
    CHECK_EQ(built_result.replay_summary.final_state_hash, summary.final_state_hash);
    const auto valid_result = built_result.signed_result;

	auto wrong_players = valid_result;
	wrong_players.result.player_ids = {"p1"};
	const auto wrong_players_result = server.SubmitBattleResult(wrong_players);
	CHECK_TRUE(!wrong_players_result.ok);
	CHECK_EQ(wrong_players_result.reason, std::string("player_ids_mismatch"));

    auto wrong_mode = valid_result;
    wrong_mode.result.mode_id = "battle_royale";
    const auto wrong_mode_result = server.SubmitBattleResult(wrong_mode);
    CHECK_TRUE(!wrong_mode_result.ok);
    CHECK_EQ(wrong_mode_result.reason, std::string("mode_mismatch"));

    auto wrong_ruleset = valid_result;
    wrong_ruleset.result.version.ruleset_version = "ruleset-other";
    const auto wrong_ruleset_result = server.SubmitBattleResult(wrong_ruleset);
    CHECK_TRUE(!wrong_ruleset_result.ok);
    CHECK_EQ(wrong_ruleset_result.reason, std::string("ruleset_version_mismatch"));

    auto wrong_hash = valid_result;
    wrong_hash.result.result_hash = "sha256:wrong";
    const auto wrong_hash_result = server.SubmitBattleResult(wrong_hash);
    CHECK_TRUE(!wrong_hash_result.ok);
    CHECK_EQ(wrong_hash_result.reason, std::string("result_hash_mismatch"));

    auto wrong_replay = valid_result;
    wrong_replay.result.replay_id = "battle-replay:wrong";
    const auto wrong_replay_result = server.SubmitBattleResult(wrong_replay);
    CHECK_TRUE(!wrong_replay_result.ok);
    CHECK_EQ(wrong_replay_result.reason, std::string("replay_id_mismatch"));

    auto wrong_cursor = valid_result;
    wrong_cursor.result.mode_result_json = ReplaceFirst(
        valid_result.result.mode_result_json,
        "\"event_cursor\":" + std::to_string(summary.event_count),
        "\"event_cursor\":999"
    );
    const auto wrong_cursor_result = server.SubmitBattleResult(wrong_cursor);
    CHECK_TRUE(!wrong_cursor_result.ok);
    CHECK_EQ(wrong_cursor_result.reason, std::string("event_cursor_mismatch"));

    auto wrong_owner = valid_result;
    wrong_owner.result.mode_result_json = ReplaceJsonStringField(
        valid_result.result.mode_result_json,
        "battle_result_owner",
        "client"
    );
    const auto wrong_owner_result = server.SubmitBattleResult(wrong_owner);
    CHECK_TRUE(!wrong_owner_result.ok);
    CHECK_EQ(wrong_owner_result.reason, std::string("battle_result_owner_mismatch"));

    auto missing_replay_counts = valid_result;
    missing_replay_counts.result.mode_result_json = "{\"battle_result_owner\":\"cpp\",\"event_cursor\":" +
        std::to_string(summary.event_count) + "}";
    const auto missing_replay_counts_result = server.SubmitBattleResult(missing_replay_counts);
    CHECK_TRUE(!missing_replay_counts_result.ok);
    CHECK_EQ(missing_replay_counts_result.reason, std::string("final_tick_mismatch"));

    auto wrong_final_tick = valid_result;
    wrong_final_tick.result.mode_result_json = ReplaceFirst(
        valid_result.result.mode_result_json,
        "\"final_tick\":" + std::to_string(summary.final_tick),
        "\"final_tick\":999"
    );
    const auto wrong_final_tick_result = server.SubmitBattleResult(wrong_final_tick);
    CHECK_TRUE(!wrong_final_tick_result.ok);
    CHECK_EQ(wrong_final_tick_result.reason, std::string("final_tick_mismatch"));

    auto wrong_tick_rate = valid_result;
    wrong_tick_rate.result.mode_result_json = ReplaceFirst(
        valid_result.result.mode_result_json,
        "\"tick_rate_hz\":60",
        "\"tick_rate_hz\":30"
    );
    const auto wrong_tick_rate_result = server.SubmitBattleResult(wrong_tick_rate);
    CHECK_TRUE(!wrong_tick_rate_result.ok);
    CHECK_EQ(wrong_tick_rate_result.reason, std::string("tick_rate_hz_mismatch"));

    auto wrong_mode_action_count = valid_result;
    wrong_mode_action_count.result.mode_result_json = ReplaceFirst(
        valid_result.result.mode_result_json,
        "\"mode_action_count\":" + std::to_string(summary.mode_action_count),
        "\"mode_action_count\":999"
    );
    const auto wrong_mode_action_count_result = server.SubmitBattleResult(wrong_mode_action_count);
    CHECK_TRUE(!wrong_mode_action_count_result.ok);
    CHECK_EQ(wrong_mode_action_count_result.reason, std::string("mode_action_count_mismatch"));

    auto wrong_event_trace_count = valid_result;
    wrong_event_trace_count.result.mode_result_json = ReplaceFirst(
        valid_result.result.mode_result_json,
        "\"event_trace_count\":" + std::to_string(summary.event_trace.size()),
        "\"event_trace_count\":999"
    );
    const auto wrong_event_trace_count_result = server.SubmitBattleResult(wrong_event_trace_count);
    CHECK_TRUE(!wrong_event_trace_count_result.ok);
    CHECK_EQ(wrong_event_trace_count_result.reason, std::string("event_trace_count_mismatch"));

    auto wrong_input_stream_hash = valid_result;
    wrong_input_stream_hash.result.mode_result_json = ReplaceFirst(
        valid_result.result.mode_result_json,
        "\"input_stream_hash\":\"" + summary.input_stream_hash + "\"",
        "\"input_stream_hash\":\"fnv64:0000000000000000\""
    );
    const auto wrong_input_stream_hash_result = server.SubmitBattleResult(wrong_input_stream_hash);
    CHECK_TRUE(!wrong_input_stream_hash_result.ok);
    CHECK_EQ(wrong_input_stream_hash_result.reason, std::string("input_stream_hash_mismatch"));

    auto wrong_event_stream_hash = valid_result;
    wrong_event_stream_hash.result.mode_result_json = ReplaceFirst(
        valid_result.result.mode_result_json,
        "\"event_stream_hash\":\"" + summary.event_stream_hash + "\"",
        "\"event_stream_hash\":\"fnv64:0000000000000000\""
    );
    const auto wrong_event_stream_hash_result = server.SubmitBattleResult(wrong_event_stream_hash);
    CHECK_TRUE(!wrong_event_stream_hash_result.ok);
    CHECK_EQ(wrong_event_stream_hash_result.reason, std::string("event_stream_hash_mismatch"));

    auto wrong_final_state_hash = valid_result;
    wrong_final_state_hash.result.mode_result_json = ReplaceFirst(
        valid_result.result.mode_result_json,
        "\"final_state_hash\":\"" + summary.final_state_hash + "\"",
        "\"final_state_hash\":\"fnv64:0000000000000000\""
    );
    const auto wrong_final_state_hash_result = server.SubmitBattleResult(wrong_final_state_hash);
    CHECK_TRUE(!wrong_final_state_hash_result.ok);
    CHECK_EQ(wrong_final_state_hash_result.reason, std::string("final_state_hash_mismatch"));

    auto wrong_replay_summary_hash = valid_result;
    wrong_replay_summary_hash.result.mode_result_json = ReplaceJsonStringField(
        valid_result.result.mode_result_json,
        "replay_summary_hash",
        "sha256:dev-fnv64-0000000000000000"
    );
    const auto wrong_replay_summary_hash_result = server.SubmitBattleResult(wrong_replay_summary_hash);
    CHECK_TRUE(!wrong_replay_summary_hash_result.ok);
    CHECK_EQ(wrong_replay_summary_hash_result.reason, std::string("replay_summary_hash_mismatch"));

    auto wrong_replay_fixture_hash = valid_result;
    wrong_replay_fixture_hash.result.mode_result_json = ReplaceJsonStringField(
        valid_result.result.mode_result_json,
        "replay_fixture_hash",
        "sha256:dev-fnv64-0000000000000000"
    );
    const auto wrong_replay_fixture_hash_result = server.SubmitBattleResult(wrong_replay_fixture_hash);
    CHECK_TRUE(!wrong_replay_fixture_hash_result.ok);
    CHECK_EQ(wrong_replay_fixture_hash_result.reason, std::string("replay_fixture_hash_mismatch"));

    auto wrong_final_snapshot_tick = valid_result;
    wrong_final_snapshot_tick.result.mode_result_json = ReplaceFirst(
        valid_result.result.mode_result_json,
        "\"final_snapshot_tick\":" + std::to_string(summary.final_tick),
        "\"final_snapshot_tick\":999"
    );
    const auto wrong_final_snapshot_tick_result = server.SubmitBattleResult(wrong_final_snapshot_tick);
    CHECK_TRUE(!wrong_final_snapshot_tick_result.ok);
    CHECK_EQ(wrong_final_snapshot_tick_result.reason, std::string("final_snapshot_tick_mismatch"));

    auto wrong_final_snapshot_kind = valid_result;
    wrong_final_snapshot_kind.result.mode_result_json = ReplaceJsonStringField(
        valid_result.result.mode_result_json,
        "final_snapshot_kind",
        "delta"
    );
    const auto wrong_final_snapshot_kind_result = server.SubmitBattleResult(wrong_final_snapshot_kind);
    CHECK_TRUE(!wrong_final_snapshot_kind_result.ok);
    CHECK_EQ(wrong_final_snapshot_kind_result.reason, std::string("final_snapshot_kind_mismatch"));

    auto wrong_final_snapshot_state_hash = valid_result;
    wrong_final_snapshot_state_hash.result.mode_result_json = ReplaceJsonStringField(
        valid_result.result.mode_result_json,
        "final_snapshot_state_hash",
        "fnv64:0000000000000000"
    );
    const auto wrong_final_snapshot_state_hash_result = server.SubmitBattleResult(wrong_final_snapshot_state_hash);
    CHECK_TRUE(!wrong_final_snapshot_state_hash_result.ok);
    CHECK_EQ(wrong_final_snapshot_state_hash_result.reason, std::string("final_snapshot_state_hash_mismatch"));

    auto wrong_final_snapshot_event_cursor = valid_result;
    wrong_final_snapshot_event_cursor.result.mode_result_json = ReplaceFirst(
        valid_result.result.mode_result_json,
        "\"final_snapshot_event_cursor\":" + std::to_string(summary.event_count),
        "\"final_snapshot_event_cursor\":999"
    );
    const auto wrong_final_snapshot_event_cursor_result = server.SubmitBattleResult(wrong_final_snapshot_event_cursor);
    CHECK_TRUE(!wrong_final_snapshot_event_cursor_result.ok);
    CHECK_EQ(
        wrong_final_snapshot_event_cursor_result.reason,
        std::string("final_snapshot_event_cursor_mismatch")
    );

    auto mutating_projection = valid_result;
    mutating_projection.result.reward_projection_json = "{\"source\":\"battle-server\",\"grant_currency\":100}";
    const auto mutating_projection_result = server.SubmitBattleResult(mutating_projection);
    CHECK_TRUE(!mutating_projection_result.ok);
    CHECK_EQ(mutating_projection_result.reason, std::string("reward_projection_mutation_forbidden"));

    auto mutating_mode_result = valid_result;
    mutating_mode_result.result.mode_result_json = ReplaceFirst(
        valid_result.result.mode_result_json,
        "}",
        ",\"inventory_grant\":100}"
    );
    const auto mutating_mode_result_result = server.SubmitBattleResult(mutating_mode_result);
    CHECK_TRUE(!mutating_mode_result_result.ok);
    CHECK_EQ(mutating_mode_result_result.reason, std::string("mode_result_mutation_forbidden"));

    auto wrong_signature = valid_result;
    wrong_signature.signature_hex = RepeatHex('c', 128);
    const auto wrong_signature_result = server.SubmitBattleResult(wrong_signature);
    CHECK_TRUE(!wrong_signature_result.ok);
    CHECK_EQ(wrong_signature_result.reason, std::string("dev_result_signature_mismatch"));

    auto stale_signature_payload = valid_result;
    stale_signature_payload.result.settled_at_ms -= 1;
    const auto stale_signature_payload_result = server.SubmitBattleResult(stale_signature_payload);
    CHECK_TRUE(!stale_signature_payload_result.ok);
    CHECK_EQ(stale_signature_payload_result.reason, std::string("dev_result_signature_mismatch"));

	const auto accepted = server.SubmitBattleResult(valid_result);
	CHECK_TRUE(accepted.ok);
	CHECK_EQ(accepted.reason, std::string("ok"));
	CHECK_EQ(accepted.settlement_key, std::string(phk::v1::kBattleResultCallbackSettlementKey));
	CHECK_TRUE(!accepted.verification.warnings.empty());

	const auto duplicate = server.SubmitBattleResult(valid_result);
	CHECK_TRUE(duplicate.ok);
	CHECK_TRUE(duplicate.duplicate);
	CHECK_EQ(duplicate.reason, std::string("ok"));
	return true;
}

bool TestBuildSignedBattleResultCallback() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489630000;
    phk::battle::BattleServer server(config);
    CHECK_TRUE(server.RegisterTicket(MakeTicket()).ok);
    CHECK_TRUE(server.RegisterTicket(MakeTicketForBob()).ok);
    CHECK_TRUE(server.AcceptInput(MakeInput("p1", 1, 1, 1u << 3)).ok);
    CHECK_TRUE(server.AcceptInput(MakeInput("p2", 1, 1, 1u << 2)).ok);
    CHECK_EQ(server.TickMatch("match-001").snapshot_tick, static_cast<std::uint64_t>(1));

    const auto missing = server.BuildSignedBattleResult("match-missing");
    CHECK_TRUE(!missing.ok);
    CHECK_EQ(missing.reason, std::string("match_unknown"));

    const auto built = server.BuildSignedBattleResult("match-001");
    CHECK_TRUE(built.ok);
    CHECK_EQ(built.reason, std::string("ok"));
    CHECK_EQ(built.replay_summary.final_tick, static_cast<std::uint64_t>(1));
    CHECK_TRUE(built.replay_summary.match_seed != 0);
    CHECK_EQ(built.replay_summary.input_count, static_cast<std::uint64_t>(2));
    CHECK_EQ(built.replay_summary.fallback_input_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(built.replay_summary.mode_action_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(built.replay_summary.input_stream_hash, std::string("fnv64:6b09da7d62e0941e"));
    CHECK_EQ(built.replay_summary.event_stream_hash, std::string("fnv64:14650fb0739d0383"));
    CHECK_EQ(built.replay_summary.final_state_hash, std::string("fnv64:72a3385f1a7c7fe3"));
    CHECK_EQ(built.signed_result.result.match_id, std::string("match-001"));
    CHECK_EQ(built.signed_result.result.mode_id, std::string("certification"));
    CHECK_EQ(built.signed_result.result.version.ruleset_version, std::string(phk::v1::kRulesetVersion));
    CHECK_EQ(built.signed_result.result.result_hash, ExpectedDevResultHash(built.replay_summary));
    CHECK_EQ(built.signed_result.result.result_hash, std::string("sha256:dev-fnv64-19959dc47479580d"));
    CHECK_EQ(built.signed_result.result.replay_id, ExpectedDevReplayId(built.replay_summary));
    CHECK_EQ(built.signed_result.result.replay_id, std::string("battle-replay:match-001:1"));
    CHECK_EQ(built.signed_result.result.player_ids.size(), static_cast<std::size_t>(2));
    CHECK_EQ(built.signed_result.key_id, config.server_id);
    CHECK_EQ(
        built.signed_result.public_key_hex,
        std::string("951038137a33596eb40aff1c8522a38f571aaa016454c52c7615710a6f440f4d")
    );
    CHECK_EQ(
        built.signed_result.signature_hex,
        phk::battle::DevBattleResultSignatureHex(built.signed_result.result, config.server_id)
    );
    CHECK_EQ(
        built.signed_result.signature_hex,
        std::string(
            "e33185f6558f0e1c022c4cff607e583d212714086b6da25e4021db11765cec7f"
            "674669d229d1e598864130db34c12fb9a53bf7e43fb079dac436beed4a9fc3fb"
        )
    );
    CHECK_TRUE(built.signed_result.server_authoritative);
    CHECK_EQ(
        phk::battle::CanonicalBattleResultPayload(built.signed_result.result),
        std::string(
            "1|0.1.0-draft|0.1.0-draft|ruleset-local-s0|match-001|certification|"
            "sha256:dev-fnv64-19959dc47479580d|battle-replay:match-001:1|p1,p2,|"
            "{\"source\":\"phk-battle-server\",\"projection_only\":true,\"settlement_authority\":\"nakama-go\"}|"
            "{\"battle_result_owner\":\"cpp\",\"event_cursor\":0,\"final_tick\":1,\"tick_rate_hz\":60,"
            "\"match_seed\":16031087345790602692,\"input_count\":2,"
            "\"fallback_input_count\":0,\"neutral_fallback_count\":0,\"held_input_fallback_count\":0,"
            "\"mode_action_count\":0,\"input_trace_count\":2,\"event_trace_count\":0,"
            "\"input_stream_hash\":\"fnv64:6b09da7d62e0941e\","
            "\"event_stream_hash\":\"fnv64:14650fb0739d0383\","
            "\"final_state_hash\":\"fnv64:72a3385f1a7c7fe3\","
            "\"replay_summary_hash\":\"sha256:dev-fnv64-e6fb6a98c2e6844d\","
            "\"replay_fixture_hash\":\"sha256:dev-fnv64-939e7eb9c028b0d4\","
            "\"final_snapshot_tick\":1,\"final_snapshot_kind\":\"replay_final\","
            "\"final_snapshot_state_hash\":\"fnv64:72a3385f1a7c7fe3\","
            "\"final_snapshot_event_cursor\":0}|1782489630000"
        )
    );
    CHECK_TRUE(
        built.signed_result.result.reward_projection_json.find("projection_only") != std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find(
            "\"event_cursor\":" + std::to_string(built.replay_summary.event_count)
        ) != std::string::npos
    );
    auto missing_zero_cursor = built.signed_result;
    missing_zero_cursor.result.mode_result_json = ReplaceFirst(
        built.signed_result.result.mode_result_json,
        "\"event_cursor\":" + std::to_string(built.replay_summary.event_count) + ",",
        ""
    );
    const auto missing_zero_cursor_result = server.SubmitBattleResult(missing_zero_cursor);
    CHECK_TRUE(!missing_zero_cursor_result.ok);
    CHECK_EQ(missing_zero_cursor_result.reason, std::string("event_cursor_mismatch"));

    auto wrong_zero_cursor = built.signed_result;
    wrong_zero_cursor.result.mode_result_json = ReplaceFirst(
        built.signed_result.result.mode_result_json,
        "\"event_cursor\":" + std::to_string(built.replay_summary.event_count),
        "\"event_cursor\":1"
    );
    const auto wrong_zero_cursor_result = server.SubmitBattleResult(wrong_zero_cursor);
    CHECK_TRUE(!wrong_zero_cursor_result.ok);
    CHECK_EQ(wrong_zero_cursor_result.reason, std::string("event_cursor_mismatch"));

    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find(
            "\"final_tick\":" + std::to_string(built.replay_summary.final_tick)
        ) != std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find("\"tick_rate_hz\":60") != std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find(
            "\"match_seed\":" + std::to_string(built.replay_summary.match_seed)
        ) != std::string::npos
    );
    auto missing_tick_rate = built.signed_result;
    missing_tick_rate.result.mode_result_json = ReplaceFirst(
        built.signed_result.result.mode_result_json,
        "\"tick_rate_hz\":60,",
        ""
    );
    const auto missing_tick_rate_result = server.SubmitBattleResult(missing_tick_rate);
    CHECK_TRUE(!missing_tick_rate_result.ok);
    CHECK_EQ(missing_tick_rate_result.reason, std::string("tick_rate_hz_mismatch"));

    auto wrong_tick_rate = built.signed_result;
    wrong_tick_rate.result.mode_result_json = ReplaceFirst(
        built.signed_result.result.mode_result_json,
        "\"tick_rate_hz\":60",
        "\"tick_rate_hz\":30"
    );
    const auto wrong_tick_rate_result = server.SubmitBattleResult(wrong_tick_rate);
    CHECK_TRUE(!wrong_tick_rate_result.ok);
    CHECK_EQ(wrong_tick_rate_result.reason, std::string("tick_rate_hz_mismatch"));

    auto missing_match_seed = built.signed_result;
    missing_match_seed.result.mode_result_json = ReplaceFirst(
        built.signed_result.result.mode_result_json,
        "\"match_seed\":" + std::to_string(built.replay_summary.match_seed) + ",",
        ""
    );
    const auto missing_match_seed_result = server.SubmitBattleResult(missing_match_seed);
    CHECK_TRUE(!missing_match_seed_result.ok);
    CHECK_EQ(missing_match_seed_result.reason, std::string("match_seed_mismatch"));

    auto wrong_match_seed = built.signed_result;
    wrong_match_seed.result.mode_result_json = ReplaceFirst(
        built.signed_result.result.mode_result_json,
        "\"match_seed\":" + std::to_string(built.replay_summary.match_seed),
        "\"match_seed\":" + std::to_string(built.replay_summary.match_seed + 1)
    );
    const auto wrong_match_seed_result = server.SubmitBattleResult(wrong_match_seed);
    CHECK_TRUE(!wrong_match_seed_result.ok);
    CHECK_EQ(wrong_match_seed_result.reason, std::string("match_seed_mismatch"));

    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find(
            "\"input_count\":" + std::to_string(built.replay_summary.input_count)
        ) != std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find(
            "\"fallback_input_count\":" + std::to_string(built.replay_summary.fallback_input_count)
        ) != std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find(
            "\"mode_action_count\":" + std::to_string(built.replay_summary.mode_action_count)
        ) != std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find(
            "\"input_trace_count\":" + std::to_string(built.replay_summary.input_trace.size())
        ) != std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find(
            "\"event_trace_count\":" + std::to_string(built.replay_summary.event_trace.size())
        ) != std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find(
            "\"input_stream_hash\":\"" + built.replay_summary.input_stream_hash + "\""
        ) != std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find(
            "\"event_stream_hash\":\"" + built.replay_summary.event_stream_hash + "\""
        ) != std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find(
            "\"final_state_hash\":\"" + built.replay_summary.final_state_hash + "\""
        ) != std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find(
            "\"replay_summary_hash\":\"" + ReplaySummaryHashForSummary(built.replay_summary) + "\""
        ) != std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find(
            "\"replay_fixture_hash\":\"sha256:dev-fnv64-939e7eb9c028b0d4\""
        ) != std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find(
            "\"final_snapshot_tick\":" + std::to_string(built.replay_summary.final_tick)
        ) != std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find("\"final_snapshot_kind\":\"replay_final\"") !=
        std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find(
            "\"final_snapshot_state_hash\":\"" + built.replay_summary.final_state_hash + "\""
        ) != std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find(
            "\"final_snapshot_event_cursor\":" + std::to_string(built.replay_summary.event_count)
        ) != std::string::npos
    );

    auto wrong_snapshot_state_hash = built.signed_result;
    wrong_snapshot_state_hash.result.mode_result_json = ReplaceJsonStringField(
        built.signed_result.result.mode_result_json,
        "final_snapshot_state_hash",
        "fnv64:0000000000000000"
    );
    const auto wrong_snapshot_state_hash_result = server.SubmitBattleResult(wrong_snapshot_state_hash);
    CHECK_TRUE(!wrong_snapshot_state_hash_result.ok);
    CHECK_EQ(wrong_snapshot_state_hash_result.reason, std::string("final_snapshot_state_hash_mismatch"));

    const auto submitted = server.SubmitBattleResult(built.signed_result);
    CHECK_TRUE(submitted.ok);
    CHECK_EQ(submitted.reason, std::string("ok"));
    CHECK_TRUE(!submitted.duplicate);
    CHECK_TRUE(!submitted.verification.warnings.empty());

    const auto duplicate = server.SubmitBattleResult(built.signed_result);
    CHECK_TRUE(duplicate.ok);
    CHECK_TRUE(duplicate.duplicate);
    CHECK_EQ(duplicate.settlement_key, std::string("battle-result:match-001"));
    return true;
}

phk::battle::BattleInput MakeInput(
    const std::string& player_id,
    std::uint64_t tick,
    std::uint64_t seq,
    std::uint32_t direction_bits
) {
    phk::battle::BattleInput input;
    input.match_id = "match-001";
    input.player_id = player_id;
    input.tick = tick;
    input.seq = seq;
    input.direction_bits = direction_bits;
    input.slow = true;
    input.shoot = true;
    input.card_slot = -1;
    return input;
}

phk::battle::BattleModeAction MakeModeAction(std::uint64_t seq = phk::v1::kBattleModeActionSeq) {
    phk::battle::BattleModeAction action;
    action.match_id = std::string(phk::v1::kBattleModeActionMatchId);
    action.player_id = std::string(phk::v1::kBattleModeActionPlayerId);
    action.tick = phk::v1::kBattleModeActionTick;
    action.seq = seq;
    action.action_id = std::string(phk::v1::kBattleModeActionActionId);
    action.action_type = "cast_card";
    action.payload_json = "{\"card_slot\":1}";
    action.client_result_authoritative = false;
    return action;
}

bool TestSimulationDeterminism() {
    phk::battle::SimulationConfig config;
    config.match_id = "match-001";
    config.mode_id = "world_boss";
    config.match_seed = 12345;
    config.spawn_period_ticks = 2;
    phk::battle::BattleSimulation first(config);
    phk::battle::BattleSimulation second(config);
    CHECK_EQ(first.Config().tick_rate_hz, phk::battle::kBattleTickRateHz);
    CHECK_TRUE(first.AddPlayer("p1", -20000, 0));
    CHECK_TRUE(first.AddPlayer("p2", 20000, 0));
    CHECK_TRUE(second.AddPlayer("p1", -20000, 0));
    CHECK_TRUE(second.AddPlayer("p2", 20000, 0));

    auto invalid_direction = MakeInput("p1", 1, 1, 0x10u);
    const auto invalid_direction_result = first.AcceptInput(invalid_direction);
    CHECK_TRUE(!invalid_direction_result.ok);
    CHECK_EQ(invalid_direction_result.reason, std::string("invalid_direction_bits"));

    auto invalid_card_slot = MakeInput("p1", 1, 1, 0);
    invalid_card_slot.card_slot = 8;
    const auto invalid_card_slot_result = first.AcceptInput(invalid_card_slot);
    CHECK_TRUE(!invalid_card_slot_result.ok);
    CHECK_EQ(invalid_card_slot_result.reason, std::string("invalid_card_slot"));

    auto oversized_input_mode_action_id = MakeInput("p1", 1, 1, 0);
    oversized_input_mode_action_id.mode_action_id =
        std::string(phk::battle::kDefaultMaxModeActionIdBytes + 1, 'a');
    const auto oversized_input_mode_action_id_result = first.AcceptInput(oversized_input_mode_action_id);
    CHECK_TRUE(!oversized_input_mode_action_id_result.ok);
    CHECK_EQ(
        oversized_input_mode_action_id_result.reason,
        std::string("input_mode_action_id_too_large")
    );

    auto unsafe_input_mode_action_id = MakeInput("p1", 1, 1, 0);
    unsafe_input_mode_action_id.mode_action_id = "mode-action\\escaped";
    const auto unsafe_input_mode_action_id_result = first.AcceptInput(unsafe_input_mode_action_id);
    CHECK_TRUE(!unsafe_input_mode_action_id_result.ok);
    CHECK_EQ(
        unsafe_input_mode_action_id_result.reason,
        std::string("input_mode_action_id_invalid")
    );

    const auto accepted = first.AcceptInput(MakeInput("p1", 1, 1, 1u << 3));
    CHECK_TRUE(accepted.ok);
    const auto duplicate_tick = first.AcceptInput(MakeInput("p1", 1, 2, 1u << 0));
    CHECK_TRUE(!duplicate_tick.ok);
    CHECK_EQ(duplicate_tick.reason, std::string("input_tick_duplicate"));
    const auto replay = first.AcceptInput(MakeInput("p1", 2, 1, 1u << 3));
    CHECK_TRUE(!replay.ok);
    CHECK_EQ(replay.reason, std::string("seq_replay"));
    const auto seq_jump = first.AcceptInput(MakeInput("p1", 2, 64, 1u << 3));
    CHECK_TRUE(!seq_jump.ok);
    CHECK_EQ(seq_jump.reason, std::string("seq_too_far_ahead"));
    const auto far_future = first.AcceptInput(MakeInput("p1", 99, 2, 1u << 3));
    CHECK_TRUE(!far_future.ok);
    CHECK_EQ(far_future.reason, std::string("input_tick_too_far_ahead"));

    CHECK_TRUE(second.AcceptInput(MakeInput("p1", 1, 1, 1u << 3)).ok);
    CHECK_TRUE(first.AcceptInput(MakeInput("p2", 1, 1, 1u << 2)).ok);
    CHECK_TRUE(second.AcceptInput(MakeInput("p2", 1, 1, 1u << 2)).ok);

    auto action = MakeModeAction(2);
    action.tick = 2;
    CHECK_TRUE(first.AcceptModeAction(action).ok);
    CHECK_TRUE(second.AcceptModeAction(action).ok);
    const auto replay_action = first.AcceptModeAction(action);
    CHECK_TRUE(!replay_action.ok);
    CHECK_EQ(replay_action.reason, std::string("seq_replay"));
    auto missing_action = MakeModeAction(3);
    missing_action.tick = 2;
    missing_action.action_id.clear();
    const auto missing_action_result = first.AcceptModeAction(missing_action);
    CHECK_TRUE(!missing_action_result.ok);
    CHECK_EQ(missing_action_result.reason, std::string("mode_action_missing_fields"));
    auto unsupported_action = MakeModeAction(3);
    unsupported_action.tick = 2;
    unsupported_action.action_type = "grant_reward";
    const auto unsupported_action_result = first.AcceptModeAction(unsupported_action);
    CHECK_TRUE(!unsupported_action_result.ok);
    CHECK_EQ(unsupported_action_result.reason, std::string("mode_action_type_unsupported"));
    auto far_action = MakeModeAction(3);
    far_action.tick = 99;
    const auto far_action_result = first.AcceptModeAction(far_action);
    CHECK_TRUE(!far_action_result.ok);
    CHECK_EQ(far_action_result.reason, std::string("mode_action_tick_too_far_ahead"));

    auto cast_card_missing_slot = MakeModeAction(3);
    cast_card_missing_slot.tick = 2;
    cast_card_missing_slot.action_id = "action-cast-missing-slot";
    cast_card_missing_slot.action_type = "cast_card";
    cast_card_missing_slot.payload_json = "{\"card_id\":\"focus_lens\"}";
    const auto cast_card_missing_slot_result = first.AcceptModeAction(cast_card_missing_slot);
    CHECK_TRUE(!cast_card_missing_slot_result.ok);
    CHECK_EQ(cast_card_missing_slot_result.reason, std::string("cast_card_slot_missing"));

    auto cast_card_bad_slot = cast_card_missing_slot;
    cast_card_bad_slot.payload_json = "{\"card_slot\":8}";
    const auto cast_card_bad_slot_result = first.AcceptModeAction(cast_card_bad_slot);
    CHECK_TRUE(!cast_card_bad_slot_result.ok);
    CHECK_EQ(cast_card_bad_slot_result.reason, std::string("cast_card_slot_invalid"));

    auto cast_card_fractional_slot = cast_card_missing_slot;
    cast_card_fractional_slot.payload_json = "{\"card_slot\":1.5}";
    const auto cast_card_fractional_slot_result = first.AcceptModeAction(cast_card_fractional_slot);
    CHECK_TRUE(!cast_card_fractional_slot_result.ok);
    CHECK_EQ(cast_card_fractional_slot_result.reason, std::string("cast_card_slot_missing"));

    auto cast_card_forged_damage = cast_card_missing_slot;
    cast_card_forged_damage.payload_json = "{\"card_slot\":1,\"damage\":999,\"boss_hp\":0}";
    const auto cast_card_forged_damage_result = first.AcceptModeAction(cast_card_forged_damage);
    CHECK_TRUE(!cast_card_forged_damage_result.ok);
    CHECK_EQ(cast_card_forged_damage_result.reason, std::string("mode_action_authority_field_forbidden"));

    auto cast_card = cast_card_missing_slot;
    cast_card.payload_json = "{\"card_slot\":1}";
    CHECK_TRUE(first.ValidateModeAction(cast_card).ok);

    auto select_round_non_br = MakeModeAction(3);
    select_round_non_br.tick = 2;
    select_round_non_br.action_id = "action-select-round-non-br";
    select_round_non_br.action_type = "select_round_card";
    select_round_non_br.payload_json = "{\"candidate_index\":1}";
    const auto select_round_non_br_result = first.AcceptModeAction(select_round_non_br);
    CHECK_TRUE(!select_round_non_br_result.ok);
    CHECK_EQ(select_round_non_br_result.reason, std::string("select_round_card_mode_unsupported"));

    auto transfer_missing = MakeModeAction(3);
    transfer_missing.tick = 2;
    transfer_missing.action_id = "action-transfer-missing";
    transfer_missing.action_type = "transfer_card";
    transfer_missing.payload_json = "{\"target_player_id\":\"p2\"}";
    const auto transfer_missing_result = first.AcceptModeAction(transfer_missing);
    CHECK_TRUE(!transfer_missing_result.ok);
    CHECK_EQ(transfer_missing_result.reason, std::string("transfer_card_payload_missing_fields"));

    auto transfer_self = transfer_missing;
    transfer_self.payload_json = "{\"target_player_id\":\"p1\",\"card_instance_id\":\"card-self\"}";
    const auto transfer_self_result = first.AcceptModeAction(transfer_self);
    CHECK_TRUE(!transfer_self_result.ok);
    CHECK_EQ(transfer_self_result.reason, std::string("transfer_card_self_forbidden"));

    auto transfer_unknown = transfer_missing;
    transfer_unknown.payload_json = "{\"target_player_id\":\"p3\",\"card_instance_id\":\"card-unknown\"}";
    const auto transfer_unknown_result = first.AcceptModeAction(transfer_unknown);
    CHECK_TRUE(!transfer_unknown_result.ok);
    CHECK_EQ(transfer_unknown_result.reason, std::string("transfer_card_target_unknown"));

    auto transfer = transfer_missing;
    transfer.payload_json = "{\"target_player_id\":\"p2\",\"card_instance_id\":\"boss-card-001\"}";
    phk::battle::TransferableCardState transfer_card;
    transfer_card.card_instance_id = "boss-card-001";
    transfer_card.owner_player_id = "p1";
    CHECK_TRUE(first.ConfigureTransferableCard(transfer_card));
    CHECK_TRUE(second.ConfigureTransferableCard(transfer_card));
    CHECK_TRUE(first.AcceptModeAction(transfer).ok);
    CHECK_TRUE(second.AcceptModeAction(transfer).ok);

    auto transfer_duplicate = transfer;
    transfer_duplicate.seq = 4;
    transfer_duplicate.action_id = "action-transfer-duplicate";
    const auto transfer_duplicate_result = first.AcceptModeAction(transfer_duplicate);
    CHECK_TRUE(!transfer_duplicate_result.ok);
    CHECK_EQ(transfer_duplicate_result.reason, std::string("transfer_card_duplicate"));

    phk::battle::BattleSnapshot first_snapshot;
    phk::battle::BattleSnapshot second_snapshot;
    for (int i = 0; i < 3; ++i) {
        first_snapshot = first.Tick();
        second_snapshot = second.Tick();
    }

    CHECK_EQ(first_snapshot.snapshot_tick, static_cast<std::uint64_t>(3));
    CHECK_EQ(first_snapshot.players.size(), static_cast<std::size_t>(2));
    CHECK_TRUE(first_snapshot.bullets_delta.size() >= 4);
    CHECK_EQ(first_snapshot.mode_state.at("mode_id"), std::string("world_boss"));
    CHECK_EQ(first_snapshot.mode_state.at("ruleset_version"), std::string(phk::v1::kRulesetVersion));
    CHECK_EQ(first_snapshot.state_hash, second_snapshot.state_hash);
    CHECK_EQ(first.Summary().input_stream_hash, second.Summary().input_stream_hash);
    CHECK_EQ(first.Summary().event_stream_hash, second.Summary().event_stream_hash);
    CHECK_EQ(first.Summary().final_state_hash, first_snapshot.state_hash);
    CHECK_EQ(first.Summary().fallback_input_count, static_cast<std::uint64_t>(4));
    CHECK_EQ(first.Summary().neutral_fallback_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(first.Summary().held_input_fallback_count, static_cast<std::uint64_t>(4));
    CHECK_EQ(first.Summary().mode_action_count, static_cast<std::uint64_t>(2));
    CHECK_EQ(first.Summary().event_count, static_cast<std::uint64_t>(3));
    CHECK_EQ(first.Summary().last_mode_action_id, transfer.action_id);
    CHECK_EQ(first.Summary().last_mode_action_type, transfer.action_type);
    CHECK_EQ(first.Summary().last_mode_action_player_id, action.player_id);
    CHECK_EQ(first.Summary().last_mode_action_tick, action.tick);
    CHECK_EQ(first.Summary().last_mode_action_seq, transfer.seq);
    CHECK_EQ(first.Summary().last_mode_action_id, second.Summary().last_mode_action_id);
    CHECK_TRUE(first.Summary().input_trace == second.Summary().input_trace);
    CHECK_TRUE(first.Summary().event_trace == second.Summary().event_trace);
    CHECK_EQ(first.Summary().input_trace.size(), static_cast<std::size_t>(6));
    CHECK_EQ(first.Summary().event_trace.size(), static_cast<std::size_t>(3));
    CHECK_TRUE(first.Summary().input_trace[0].find("input|p1|tick=1|seq=1") != std::string::npos);
    CHECK_TRUE(first.Summary().input_trace[2].find("fallback|held|p1|tick=2") != std::string::npos);
    CHECK_TRUE(first.Summary().event_trace[0].find("mode_action|p1|tick=2|seq=2") != std::string::npos);
    CHECK_TRUE(first.Summary().event_trace[1].find("mode_action|p1|tick=2|seq=3") != std::string::npos);
    CHECK_TRUE(first.Summary().event_trace[1].find("|card=boss-card-001|from=p1|to=p2") != std::string::npos);
    CHECK_TRUE(first.Summary().event_trace[1].find("|authority_owner=p1|mode_allowed=1|cost_paid=1|cooldown_ready=1") != std::string::npos);
    CHECK_TRUE(first.Summary().event_trace[2].find("bullet_spawn|tick=2") != std::string::npos);
    CHECK_EQ(first_snapshot.mode_state.at("last_mode_action_id"), transfer.action_id);
    CHECK_EQ(first_snapshot.mode_state.at("last_mode_action_type"), transfer.action_type);
    CHECK_EQ(first_snapshot.mode_state.at("last_mode_action_player_id"), action.player_id);
    CHECK_EQ(first_snapshot.mode_state.at("last_mode_action_tick"), std::to_string(action.tick));
    CHECK_EQ(first_snapshot.mode_state.at("last_mode_action_seq"), std::to_string(transfer.seq));
    CHECK_EQ(first_snapshot.mode_state.at("transfer_card_count"), std::string("1"));
    CHECK_EQ(
        first_snapshot.mode_state.at("transfer_card_edges_material"),
        std::string("boss-card-001:p1>p2:p1:1:1:1;")
    );
    CHECK_EQ(first_snapshot.mode_state.at("last_transfer_card_instance_id"), std::string("boss-card-001"));
    CHECK_EQ(first_snapshot.mode_state.at("last_transfer_from_player_id"), std::string("p1"));
    CHECK_EQ(first_snapshot.mode_state.at("last_transfer_to_player_id"), std::string("p2"));
    CHECK_EQ(first_snapshot.mode_state.at("last_transfer_authority_owner_player_id"), std::string("p1"));
    CHECK_EQ(first_snapshot.mode_state.at("last_transfer_authority_mode_allowed"), std::string("1"));
    CHECK_EQ(first_snapshot.mode_state.at("last_transfer_authority_cost_paid"), std::string("1"));
    CHECK_EQ(first_snapshot.mode_state.at("last_transfer_authority_cooldown_ready"), std::string("1"));
    CHECK_EQ(first_snapshot.mode_state.at("accepted_input_count"), std::string("2"));
    CHECK_EQ(first_snapshot.mode_state.at("fallback_input_count"), std::string("4"));
    CHECK_EQ(first_snapshot.mode_state.at("neutral_fallback_count"), std::string("0"));
    CHECK_EQ(first_snapshot.mode_state.at("held_input_fallback_count"), std::string("4"));
    CHECK_EQ(first_snapshot.mode_state.at("mode_action_count"), std::string("2"));
    return true;
}

bool TestFallbackInputReplayAudit() {
    phk::battle::SimulationConfig config;
    config.match_id = "match-fallback";
    config.mode_id = "pvp_duel";
    config.match_seed = 7;
    config.spawn_period_ticks = 1000;
    phk::battle::BattleSimulation simulation(config);
    CHECK_TRUE(simulation.AddPlayer("p1", 0, 0));
    CHECK_TRUE(simulation.AddPlayer("p2", 10000, 0));

    const auto neutral_snapshot = simulation.Tick();
    CHECK_EQ(neutral_snapshot.snapshot_tick, static_cast<std::uint64_t>(1));
    CHECK_EQ(simulation.Summary().input_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(simulation.Summary().fallback_input_count, static_cast<std::uint64_t>(2));
    CHECK_EQ(simulation.Summary().neutral_fallback_count, static_cast<std::uint64_t>(2));
    CHECK_EQ(simulation.Summary().held_input_fallback_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(simulation.Summary().mode_action_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(simulation.Summary().input_trace.size(), static_cast<std::size_t>(2));
    CHECK_TRUE(simulation.Summary().input_trace[0].find("fallback|neutral|p1|tick=1") != std::string::npos);
    CHECK_TRUE(simulation.Summary().input_trace[1].find("fallback|neutral|p2|tick=1") != std::string::npos);
    CHECK_EQ(neutral_snapshot.mode_state.at("fallback_input_count"), std::string("2"));
    CHECK_EQ(neutral_snapshot.mode_state.at("neutral_fallback_count"), std::string("2"));

    auto input = MakeInput("p1", 2, 1, 1u << 3);
    input.match_id = config.match_id;
    CHECK_TRUE(simulation.AcceptInput(input).ok);
    const auto mixed_snapshot = simulation.Tick();
    CHECK_EQ(mixed_snapshot.snapshot_tick, static_cast<std::uint64_t>(2));
    CHECK_EQ(simulation.Summary().input_count, static_cast<std::uint64_t>(1));
    CHECK_EQ(simulation.Summary().fallback_input_count, static_cast<std::uint64_t>(3));
    CHECK_EQ(simulation.Summary().neutral_fallback_count, static_cast<std::uint64_t>(3));
    CHECK_EQ(simulation.Summary().held_input_fallback_count, static_cast<std::uint64_t>(0));

    const auto held_snapshot = simulation.Tick();
    CHECK_EQ(held_snapshot.snapshot_tick, static_cast<std::uint64_t>(3));
    CHECK_EQ(simulation.Summary().fallback_input_count, static_cast<std::uint64_t>(5));
    CHECK_EQ(simulation.Summary().neutral_fallback_count, static_cast<std::uint64_t>(4));
    CHECK_EQ(simulation.Summary().held_input_fallback_count, static_cast<std::uint64_t>(1));
    CHECK_EQ(simulation.Summary().input_trace.size(), static_cast<std::size_t>(6));
    CHECK_TRUE(simulation.Summary().input_trace[2].find("input|p1|tick=2|seq=1") != std::string::npos);
    CHECK_TRUE(simulation.Summary().input_trace[4].find("fallback|held|p1|tick=3|seq=1") != std::string::npos);
    CHECK_EQ(held_snapshot.mode_state.at("held_input_fallback_count"), std::string("1"));
    CHECK_TRUE(simulation.Summary().input_stream_hash.rfind("fnv64:", 0) == 0);
    CHECK_EQ(simulation.Summary().final_state_hash, held_snapshot.state_hash);
    return true;
}

bool TestReadyModeActionLifecycleState() {
    phk::battle::SimulationConfig config;
    config.match_id = "match-ready";
    config.mode_id = "pvp_duel";
    config.spawn_period_ticks = 1000;
    phk::battle::BattleSimulation simulation(config);
    CHECK_TRUE(simulation.AddPlayer("p1", -20000, 0));
    CHECK_TRUE(simulation.AddPlayer("p2", 20000, 0));

    const auto initial_snapshot = simulation.Snapshot();
    CHECK_EQ(initial_snapshot.mode_state.at("ready_player_count"), std::string("0"));
    CHECK_EQ(initial_snapshot.mode_state.at("all_players_ready"), std::string("0"));

    auto missing_ready_payload = MakeModeAction(1);
    missing_ready_payload.match_id = config.match_id;
    missing_ready_payload.player_id = "p1";
    missing_ready_payload.tick = 1;
    missing_ready_payload.seq = 1;
    missing_ready_payload.action_id = "ready-missing-payload";
    missing_ready_payload.action_type = "ready";
    missing_ready_payload.payload_json = "{\"client_ready\":true}";
    const auto missing_ready_result = simulation.AcceptModeAction(missing_ready_payload);
    CHECK_TRUE(!missing_ready_result.ok);
    CHECK_EQ(missing_ready_result.reason, std::string("ready_payload_missing"));

    auto false_ready_payload = missing_ready_payload;
    false_ready_payload.payload_json = "{\"ready\":false}";
    const auto false_ready_result = simulation.AcceptModeAction(false_ready_payload);
    CHECK_TRUE(!false_ready_result.ok);
    CHECK_EQ(false_ready_result.reason, std::string("ready_payload_not_true"));
    CHECK_EQ(simulation.Snapshot().mode_state.at("ready_player_count"), std::string("0"));

    auto p1_ready = MakeModeAction(1);
    p1_ready.match_id = config.match_id;
    p1_ready.player_id = "p1";
    p1_ready.tick = 1;
    p1_ready.seq = 1;
    p1_ready.action_id = "ready-p1";
    p1_ready.action_type = "ready";
    p1_ready.payload_json = "{\"ready\":true}";
    CHECK_TRUE(simulation.AcceptModeAction(p1_ready).ok);
    auto pending_duplicate_p1_ready = p1_ready;
    pending_duplicate_p1_ready.seq = 2;
    pending_duplicate_p1_ready.action_id = "ready-p1-pending-duplicate";
    const auto pending_duplicate_p1_ready_result = simulation.AcceptModeAction(pending_duplicate_p1_ready);
    CHECK_TRUE(!pending_duplicate_p1_ready_result.ok);
    CHECK_EQ(pending_duplicate_p1_ready_result.reason, std::string("ready_already_set"));
    CHECK_EQ(simulation.Summary().mode_action_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(simulation.Snapshot().mode_state.at("ready_player_count"), std::string("0"));
    const auto p1_ready_snapshot = simulation.Tick();
    CHECK_EQ(p1_ready_snapshot.mode_state.at("ready_player_count"), std::string("1"));
    CHECK_EQ(p1_ready_snapshot.mode_state.at("all_players_ready"), std::string("0"));
    CHECK_EQ(simulation.Summary().mode_action_count, static_cast<std::uint64_t>(1));

    auto duplicate_p1_ready = p1_ready;
    duplicate_p1_ready.tick = 2;
    duplicate_p1_ready.seq = 2;
    duplicate_p1_ready.action_id = "ready-p1-duplicate";
    const auto duplicate_p1_ready_result = simulation.AcceptModeAction(duplicate_p1_ready);
    CHECK_TRUE(!duplicate_p1_ready_result.ok);
    CHECK_EQ(duplicate_p1_ready_result.reason, std::string("ready_already_set"));
    CHECK_EQ(simulation.Summary().mode_action_count, static_cast<std::uint64_t>(1));
    CHECK_EQ(simulation.Snapshot().mode_state.at("ready_player_count"), std::string("1"));

    auto p2_ready = p1_ready;
    p2_ready.player_id = "p2";
    p2_ready.tick = 2;
    p2_ready.seq = 1;
    p2_ready.action_id = "ready-p2";
    CHECK_TRUE(simulation.AcceptModeAction(p2_ready).ok);
    const auto all_ready_snapshot = simulation.Tick();
    CHECK_EQ(all_ready_snapshot.mode_state.at("ready_player_count"), std::string("2"));
    CHECK_EQ(all_ready_snapshot.mode_state.at("all_players_ready"), std::string("1"));

    CHECK_TRUE(simulation.SetPlayerConnected("p2", false).ok);
    const auto disconnected_snapshot = simulation.Snapshot();
    CHECK_EQ(disconnected_snapshot.mode_state.at("connected_player_count"), std::string("1"));
    CHECK_EQ(disconnected_snapshot.mode_state.at("ready_player_count"), std::string("1"));
    CHECK_EQ(disconnected_snapshot.mode_state.at("all_players_ready"), std::string("0"));

    CHECK_TRUE(simulation.SetPlayerConnected("p2", true).ok);
    const auto reconnected_snapshot = simulation.Snapshot();
    CHECK_EQ(reconnected_snapshot.mode_state.at("connected_player_count"), std::string("2"));
    CHECK_EQ(reconnected_snapshot.mode_state.at("ready_player_count"), std::string("1"));
    CHECK_EQ(reconnected_snapshot.mode_state.at("all_players_ready"), std::string("0"));
    return true;
}

bool TestBattleRoyaleSelectRoundCardPayloadBoundary() {
    phk::battle::SimulationConfig config;
    config.match_id = "match-br";
    config.mode_id = "battle_royale";
    config.max_input_ahead_ticks = 4;
    config.max_seq_ahead = 8;
    config.spawn_period_ticks = 1000;
    phk::battle::BattleSimulation simulation(config);
    CHECK_TRUE(simulation.AddPlayer("p1", 0, 0));

    auto missing_candidate = MakeModeAction(1);
    missing_candidate.match_id = config.match_id;
    missing_candidate.player_id = "p1";
    missing_candidate.tick = 1;
    missing_candidate.seq = 1;
    missing_candidate.action_id = "select-round-missing";
    missing_candidate.action_type = "select_round_card";
    missing_candidate.payload_json = "{\"choice\":1}";
    const auto missing_candidate_result = simulation.AcceptModeAction(missing_candidate);
    CHECK_TRUE(!missing_candidate_result.ok);
    CHECK_EQ(missing_candidate_result.reason, std::string("select_round_card_candidate_missing"));

    auto invalid_candidate = missing_candidate;
    invalid_candidate.payload_json = "{\"candidate_index\":3}";
    const auto invalid_candidate_result = simulation.AcceptModeAction(invalid_candidate);
    CHECK_TRUE(!invalid_candidate_result.ok);
    CHECK_EQ(invalid_candidate_result.reason, std::string("select_round_card_candidate_invalid"));

    auto fractional_candidate = missing_candidate;
    fractional_candidate.payload_json = "{\"candidate_index\":1.5}";
    const auto fractional_candidate_result = simulation.AcceptModeAction(fractional_candidate);
    CHECK_TRUE(!fractional_candidate_result.ok);
    CHECK_EQ(fractional_candidate_result.reason, std::string("select_round_card_candidate_missing"));

    auto forged_candidate = missing_candidate;
    forged_candidate.payload_json = "{\"candidate_index\":1,\"reward\":\"grant\"}";
    const auto forged_candidate_result = simulation.AcceptModeAction(forged_candidate);
    CHECK_TRUE(!forged_candidate_result.ok);
    CHECK_EQ(forged_candidate_result.reason, std::string("mode_action_authority_field_forbidden"));

    auto accepted_candidate = missing_candidate;
    accepted_candidate.payload_json = "{\"candidate_index\":1}";
    const auto accepted_candidate_result = simulation.AcceptModeAction(accepted_candidate);
    CHECK_TRUE(accepted_candidate_result.ok);
    CHECK_EQ(simulation.Summary().mode_action_count, static_cast<std::uint64_t>(0));
    const auto snapshot = simulation.Tick();
    CHECK_EQ(snapshot.mode_state.at("mode_action_count"), std::string("1"));
    CHECK_EQ(snapshot.mode_state.at("last_mode_action_type"), std::string("select_round_card"));
    CHECK_EQ(simulation.Summary().last_mode_action_id, accepted_candidate.action_id);
    return true;
}

bool TestModeActionPayloadSizeLimit() {
    phk::battle::SimulationConfig config;
    config.match_id = "match-mode-action-size";
    config.mode_id = "pvp_duel";
    config.max_input_ahead_ticks = 4;
    config.max_seq_ahead = 8;
    config.spawn_period_ticks = 1000;
    phk::battle::BattleSimulation simulation(config);
    CHECK_TRUE(simulation.AddPlayer("p1", 0, 0));
    CHECK_EQ(simulation.Config().max_mode_action_id_bytes, phk::battle::kDefaultMaxModeActionIdBytes);
    CHECK_EQ(simulation.Config().max_mode_action_type_bytes, phk::battle::kDefaultMaxModeActionTypeBytes);
    CHECK_EQ(simulation.Config().max_mode_action_payload_bytes, phk::battle::kDefaultMaxModeActionPayloadBytes);

    auto oversized_action_id = MakeModeAction(1);
    oversized_action_id.match_id = config.match_id;
    oversized_action_id.player_id = "p1";
    oversized_action_id.tick = 1;
    oversized_action_id.seq = 1;
    oversized_action_id.action_id = std::string(phk::battle::kDefaultMaxModeActionIdBytes + 1, 'a');
    oversized_action_id.action_type = "cast_card";
    oversized_action_id.payload_json = "{\"card_slot\":1}";
    const auto oversized_action_id_result = simulation.AcceptModeAction(oversized_action_id);
    CHECK_TRUE(!oversized_action_id_result.ok);
    CHECK_EQ(oversized_action_id_result.reason, std::string("mode_action_id_too_large"));

    auto unsafe_action_id = oversized_action_id;
    unsafe_action_id.action_id = "mode-action\nescaped";
    const auto unsafe_action_id_result = simulation.AcceptModeAction(unsafe_action_id);
    CHECK_TRUE(!unsafe_action_id_result.ok);
    CHECK_EQ(unsafe_action_id_result.reason, std::string("mode_action_id_invalid"));

    auto oversized_action_type = oversized_action_id;
    oversized_action_type.action_id = "mode-action-type-oversized";
    oversized_action_type.action_type = std::string(phk::battle::kDefaultMaxModeActionTypeBytes + 1, 't');
    const auto oversized_action_type_result = simulation.AcceptModeAction(oversized_action_type);
    CHECK_TRUE(!oversized_action_type_result.ok);
    CHECK_EQ(oversized_action_type_result.reason, std::string("mode_action_type_too_large"));

    auto oversized = MakeModeAction(1);
    oversized.match_id = config.match_id;
    oversized.player_id = "p1";
    oversized.tick = 1;
    oversized.seq = 1;
    oversized.action_id = "mode-action-oversized";
    oversized.action_type = "cast_card";
    oversized.payload_json =
        "{\"card_slot\":1,\"padding\":\"" +
        std::string(phk::battle::kDefaultMaxModeActionPayloadBytes, 'x') +
        "\"}";
    const auto oversized_result = simulation.AcceptModeAction(oversized);
    CHECK_TRUE(!oversized_result.ok);
    CHECK_EQ(oversized_result.reason, std::string("mode_action_payload_too_large"));

    auto accepted = oversized;
    accepted.payload_json = "{\"card_slot\":1}";
    const auto accepted_result = simulation.AcceptModeAction(accepted);
    CHECK_TRUE(accepted_result.ok);
    CHECK_EQ(simulation.Tick().mode_state.at("last_mode_action_id"), accepted.action_id);

    phk::battle::SimulationConfig tiny_config;
    tiny_config.match_id = "match-mode-action-size-tiny";
    tiny_config.mode_id = "pvp_duel";
    tiny_config.max_mode_action_id_bytes = 2;
    tiny_config.max_mode_action_type_bytes = 2;
    tiny_config.max_mode_action_payload_bytes = 2;
    phk::battle::BattleSimulation tiny_simulation(tiny_config);
    CHECK_TRUE(tiny_simulation.AddPlayer("p1", 0, 0));
    CHECK_EQ(tiny_simulation.Config().max_mode_action_id_bytes, static_cast<std::size_t>(2));
    CHECK_EQ(tiny_simulation.Config().max_mode_action_type_bytes, static_cast<std::size_t>(2));
    CHECK_EQ(tiny_simulation.Config().max_mode_action_payload_bytes, static_cast<std::size_t>(2));
    auto tiny_payload = MakeModeAction(1);
    tiny_payload.match_id = tiny_config.match_id;
    tiny_payload.player_id = "p1";
    tiny_payload.tick = 1;
    tiny_payload.seq = 1;
    tiny_payload.action_id = "mode-action-tiny";
    tiny_payload.action_type = "cast_card";
    tiny_payload.payload_json = "{}";
    const auto tiny_id_result = tiny_simulation.AcceptModeAction(tiny_payload);
    CHECK_TRUE(!tiny_id_result.ok);
    CHECK_EQ(tiny_id_result.reason, std::string("mode_action_id_too_large"));

    tiny_payload.action_id = "a";
    const auto tiny_type_result = tiny_simulation.AcceptModeAction(tiny_payload);
    CHECK_TRUE(!tiny_type_result.ok);
    CHECK_EQ(tiny_type_result.reason, std::string("mode_action_type_too_large"));

    phk::battle::SimulationConfig tiny_payload_config;
    tiny_payload_config.match_id = "match-mode-action-payload-tiny";
    tiny_payload_config.mode_id = "pvp_duel";
    tiny_payload_config.max_mode_action_payload_bytes = 2;
    phk::battle::BattleSimulation tiny_payload_simulation(tiny_payload_config);
    CHECK_TRUE(tiny_payload_simulation.AddPlayer("p1", 0, 0));
    CHECK_EQ(tiny_payload_simulation.Config().max_mode_action_payload_bytes, static_cast<std::size_t>(2));
    tiny_payload.match_id = tiny_payload_config.match_id;
    tiny_payload.action_id = "mode-action-tiny-payload";
    tiny_payload.action_type = "cast_card";
    tiny_payload.payload_json = "{}";
    const auto tiny_result = tiny_payload_simulation.AcceptModeAction(tiny_payload);
    CHECK_TRUE(!tiny_result.ok);
    CHECK_EQ(tiny_result.reason, std::string("cast_card_slot_missing"));

    tiny_payload.payload_json = "{\"card_slot\":1}";
    const auto tiny_oversized_result = tiny_payload_simulation.AcceptModeAction(tiny_payload);
    CHECK_TRUE(!tiny_oversized_result.ok);
    CHECK_EQ(tiny_oversized_result.reason, std::string("mode_action_payload_too_large"));
    return true;
}

bool TestBossTransferCardValidation() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489605000;
    phk::battle::BattleServer non_boss_server(config);
    CHECK_TRUE(non_boss_server.RegisterTicket(MakeTicket()).ok);
    CHECK_TRUE(non_boss_server.RegisterTicket(MakeTicketForBob()).ok);

    phk::battle::TransferableCardState non_boss_card;
    non_boss_card.card_instance_id = "non-boss-card";
    non_boss_card.owner_player_id = "p1";
    CHECK_TRUE(!non_boss_server.ConfigureTransferableCard("match-001", non_boss_card));

    auto non_boss_transfer = MakeModeAction(1);
    non_boss_transfer.tick = 1;
    non_boss_transfer.action_id = "action-non-boss-transfer-card";
    non_boss_transfer.action_type = "transfer_card";
    non_boss_transfer.payload_json = "{\"target_player_id\":\"p2\",\"card_instance_id\":\"non-boss-card\"}";
    const auto non_boss_result = non_boss_server.AcceptModeAction(non_boss_transfer);
    CHECK_TRUE(!non_boss_result.ok);
    CHECK_EQ(non_boss_result.reason, std::string("transfer_card_mode_unsupported"));

    phk::battle::BattleServer server(config);
    CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
        "ticket-boss-transfer-1",
        "user-boss-transfer-1",
        "p1",
        "world_boss",
        "00112233445566778899bb01"
    )).ok);
    CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
        "ticket-boss-transfer-2",
        "user-boss-transfer-2",
        "p2",
        "world_boss",
        "00112233445566778899bb02"
    )).ok);

    auto disconnected = server.SetPlayerConnected("match-001", "p2", false);
    CHECK_TRUE(disconnected.ok);

    auto transfer = MakeModeAction(1);
    transfer.tick = 1;
    transfer.action_id = "action-boss-transfer-card";
    transfer.action_type = "transfer_card";
    transfer.payload_json = "{\"target_player_id\":\"p2\",\"card_instance_id\":\"boss-card-disconnected\"}";
    const auto disconnected_result = server.AcceptModeAction(transfer);
    CHECK_TRUE(!disconnected_result.ok);
    CHECK_EQ(disconnected_result.reason, std::string("transfer_card_target_disconnected"));

    CHECK_TRUE(server.SetPlayerConnected("match-001", "p2", true).ok);

    phk::battle::TransferableCardState invalid_audit_card;
    invalid_audit_card.card_instance_id = "boss-card;tampered:p1>p2";
    invalid_audit_card.owner_player_id = "p1";
    CHECK_TRUE(!server.ConfigureTransferableCard("match-001", invalid_audit_card));

    auto invalid_audit_transfer = MakeModeAction(1);
    invalid_audit_transfer.tick = 1;
    invalid_audit_transfer.action_id = "action-boss-transfer-card-invalid-audit";
    invalid_audit_transfer.action_type = "transfer_card";
    invalid_audit_transfer.payload_json =
        "{\"target_player_id\":\"p2\",\"card_instance_id\":\"boss-card;tampered:p1>p2\"}";
    const auto invalid_audit_result = server.AcceptModeAction(invalid_audit_transfer);
    CHECK_TRUE(!invalid_audit_result.ok);
    CHECK_EQ(invalid_audit_result.reason, std::string("transfer_card_instance_id_invalid"));

    const auto unauthorized = server.AcceptModeAction(transfer);
    CHECK_TRUE(!unauthorized.ok);
    CHECK_EQ(unauthorized.reason, std::string("transfer_card_not_authorized"));

    phk::battle::TransferableCardState card;
    card.card_instance_id = "boss-card-disconnected";
    card.owner_player_id = "p2";
    CHECK_TRUE(server.ConfigureTransferableCard("match-001", card));
    const auto owner_mismatch = server.AcceptModeAction(transfer);
    CHECK_TRUE(!owner_mismatch.ok);
    CHECK_EQ(owner_mismatch.reason, std::string("transfer_card_owner_mismatch"));

    card.owner_player_id = "p1";
    card.mode_allowed = false;
    CHECK_TRUE(server.ConfigureTransferableCard("match-001", card));
    const auto mode_forbidden = server.AcceptModeAction(transfer);
    CHECK_TRUE(!mode_forbidden.ok);
    CHECK_EQ(mode_forbidden.reason, std::string("transfer_card_mode_forbidden"));

    card.mode_allowed = true;
    card.cost_paid = false;
    CHECK_TRUE(server.ConfigureTransferableCard("match-001", card));
    const auto cost_unpaid = server.AcceptModeAction(transfer);
    CHECK_TRUE(!cost_unpaid.ok);
    CHECK_EQ(cost_unpaid.reason, std::string("transfer_card_cost_unpaid"));

    card.cost_paid = true;
    card.cooldown_ready = false;
    CHECK_TRUE(server.ConfigureTransferableCard("match-001", card));
    const auto cooldown_blocked = server.AcceptModeAction(transfer);
    CHECK_TRUE(!cooldown_blocked.ok);
    CHECK_EQ(cooldown_blocked.reason, std::string("transfer_card_cooldown_blocked"));

    card.cooldown_ready = true;
    CHECK_TRUE(server.ConfigureTransferableCard("match-001", card));
    const auto accepted = server.AcceptModeAction(transfer);
    CHECK_TRUE(accepted.ok);
    const auto duplicate = server.AcceptModeAction(transfer);
    CHECK_TRUE(!duplicate.ok);
    CHECK_EQ(duplicate.reason, std::string("seq_replay"));

    auto duplicate_card = transfer;
    duplicate_card.seq = 2;
    duplicate_card.action_id = "action-boss-transfer-card-duplicate";
    const auto duplicate_card_result = server.AcceptModeAction(duplicate_card);
    CHECK_TRUE(!duplicate_card_result.ok);
    CHECK_EQ(duplicate_card_result.reason, std::string("transfer_card_duplicate"));

    CHECK_EQ(server.MatchReplaySummary("match-001").mode_action_count, static_cast<std::uint64_t>(0));
    const auto snapshot = server.TickMatch("match-001");
    CHECK_EQ(snapshot.mode_state.at("mode_action_count"), std::string("1"));
    CHECK_EQ(snapshot.mode_state.at("transfer_card_count"), std::string("1"));
    CHECK_EQ(
        snapshot.mode_state.at("transfer_card_edges_material"),
        std::string("boss-card-disconnected:p1>p2:p1:1:1:1;")
    );
    CHECK_EQ(snapshot.mode_state.at("last_transfer_card_instance_id"), std::string("boss-card-disconnected"));
    CHECK_EQ(snapshot.mode_state.at("last_transfer_from_player_id"), std::string("p1"));
    CHECK_EQ(snapshot.mode_state.at("last_transfer_to_player_id"), std::string("p2"));
    CHECK_EQ(snapshot.mode_state.at("last_transfer_authority_owner_player_id"), std::string("p1"));
    CHECK_EQ(snapshot.mode_state.at("last_transfer_authority_mode_allowed"), std::string("1"));
    CHECK_EQ(snapshot.mode_state.at("last_transfer_authority_cost_paid"), std::string("1"));
    CHECK_EQ(snapshot.mode_state.at("last_transfer_authority_cooldown_ready"), std::string("1"));
    CHECK_EQ(snapshot.mode_state.at("last_mode_action_type"), std::string("transfer_card"));
    CHECK_TRUE(server.MatchReplaySummary("match-001").event_trace.back().find("type=transfer_card") != std::string::npos);
    CHECK_TRUE(server.MatchReplaySummary("match-001").event_trace.back().find("|card=boss-card-disconnected|from=p1|to=p2") != std::string::npos);
    CHECK_TRUE(server.MatchReplaySummary("match-001").event_trace.back().find("|authority_owner=p1|mode_allowed=1|cost_paid=1|cooldown_ready=1") != std::string::npos);
    return true;
}

bool TestBossModeSpawnLayout() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489605000;
    phk::battle::BattleServer server(config);

    CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
        "ticket-boss-1",
        "user-boss-1",
        "p1",
        "world_boss",
        "00112233445566778899aa01"
    )).ok);
    CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
        "ticket-boss-2",
        "user-boss-2",
        "p2",
        "world_boss",
        "00112233445566778899aa02"
    )).ok);
    CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
        "ticket-boss-3",
        "user-boss-3",
        "p3",
        "world_boss",
        "00112233445566778899aa03"
    )).ok);
    CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
        "ticket-boss-4",
        "user-boss-4",
        "p4",
        "world_boss",
        "00112233445566778899aa04"
    )).ok);

    const auto boss_snapshot = server.MatchSnapshot("match-001");
    CHECK_EQ(boss_snapshot.mode_state.at("mode_id"), std::string("world_boss"));
    CHECK_EQ(boss_snapshot.mode_state.at("battle_layout"), std::string("boss_center_ring"));
    CHECK_EQ(boss_snapshot.mode_state.at("boss_center_x_milli"), std::string("0"));
    CHECK_EQ(boss_snapshot.mode_state.at("boss_center_y_milli"), std::string("0"));
    CHECK_EQ(boss_snapshot.mode_state.at("player_fire_target"), std::string("boss_center"));
    CHECK_EQ(boss_snapshot.mode_state.at("boss_min_players"), std::string("4"));
    CHECK_EQ(boss_snapshot.mode_state.at("boss_max_players"), std::string("8"));
    CHECK_EQ(boss_snapshot.mode_state.at("boss_registered_player_count"), std::string("4"));
    CHECK_EQ(boss_snapshot.mode_state.at("boss_layout_player_count"), std::string("4"));
    CHECK_EQ(boss_snapshot.mode_state.at("boss_start_ready"), std::string("1"));
    CHECK_EQ(boss_snapshot.mode_state.at("boss_ready_player_count"), std::string("0"));
    CHECK_EQ(boss_snapshot.mode_state.at("boss_all_registered_connected"), std::string("1"));
    CHECK_EQ(boss_snapshot.mode_state.at("boss_all_registered_ready"), std::string("0"));
    CHECK_EQ(boss_snapshot.mode_state.at("boss_ready_to_start"), std::string("0"));
    CHECK_EQ(boss_snapshot.mode_state.at("boss_player_p1_spawn_slot"), std::string("north"));
    CHECK_EQ(boss_snapshot.mode_state.at("boss_player_p1_fire_target"), std::string("boss_center"));
    CHECK_EQ(boss_snapshot.mode_state.at("boss_player_p2_spawn_slot"), std::string("east"));
    CHECK_EQ(boss_snapshot.mode_state.at("boss_player_p2_fire_target"), std::string("boss_center"));
    CHECK_EQ(boss_snapshot.mode_state.at("boss_player_p3_spawn_slot"), std::string("south"));
    CHECK_EQ(boss_snapshot.mode_state.at("boss_player_p3_fire_target"), std::string("boss_center"));
    CHECK_EQ(boss_snapshot.mode_state.at("boss_player_p4_spawn_slot"), std::string("west"));
    CHECK_EQ(boss_snapshot.mode_state.at("boss_player_p4_fire_target"), std::string("boss_center"));
    CHECK_EQ(boss_snapshot.players.size(), static_cast<std::size_t>(4));
    CHECK_EQ(boss_snapshot.players[0].x_milli, 0);
    CHECK_EQ(boss_snapshot.players[0].y_milli, -60000);
    CHECK_EQ(boss_snapshot.players[1].x_milli, 60000);
    CHECK_EQ(boss_snapshot.players[1].y_milli, 0);
    CHECK_EQ(boss_snapshot.players[2].x_milli, 0);
    CHECK_EQ(boss_snapshot.players[2].y_milli, 60000);
    CHECK_EQ(boss_snapshot.players[3].x_milli, -60000);
    CHECK_EQ(boss_snapshot.players[3].y_milli, 0);

    phk::battle::BattleServer pvp_server(config);
    CHECK_TRUE(pvp_server.RegisterTicket(MakeTicket()).ok);
    CHECK_TRUE(pvp_server.RegisterTicket(MakeTicketForBob()).ok);
    const auto pvp_snapshot = pvp_server.MatchSnapshot("match-001");
    CHECK_TRUE(pvp_snapshot.mode_state.find("battle_layout") == pvp_snapshot.mode_state.end());
    CHECK_TRUE(pvp_snapshot.mode_state.find("boss_player_p1_spawn_slot") == pvp_snapshot.mode_state.end());
    CHECK_TRUE(pvp_snapshot.mode_state.find("boss_player_p1_fire_target") == pvp_snapshot.mode_state.end());
    CHECK_EQ(pvp_snapshot.players[0].x_milli, -20000);
    CHECK_EQ(pvp_snapshot.players[0].y_milli, 0);
    CHECK_EQ(pvp_snapshot.players[1].x_milli, 20000);
    CHECK_EQ(pvp_snapshot.players[1].y_milli, 0);
    return true;
}

bool TestBossMatchPreconfiguration() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489605000;
    phk::battle::BattleServer server(config);

    phk::battle::BossMatchConfig missing_match_config;
    missing_match_config.mode_id = "world_boss";
    const auto missing_match = server.ConfigureBossMatch(missing_match_config);
    CHECK_TRUE(!missing_match.ok);
    CHECK_EQ(missing_match.reason, std::string("match_id_missing"));
    CHECK_EQ(missing_match.pending_boss_configs_before, static_cast<std::size_t>(0));
    CHECK_EQ(missing_match.pending_boss_configs_after, static_cast<std::size_t>(0));

    phk::battle::BossMatchConfig pvp_config;
    pvp_config.match_id = "match-001";
    pvp_config.mode_id = "pvp_duel";
    const auto non_boss = server.ConfigureBossMatch(pvp_config);
    CHECK_TRUE(!non_boss.ok);
    CHECK_EQ(non_boss.reason, std::string("boss_mode_required"));

    phk::battle::BossMatchConfig invalid_instance_config;
    invalid_instance_config.match_id = "match-invalid-instance";
    invalid_instance_config.mode_id = "world_boss";
    invalid_instance_config.boss_instance_id = "world boss with spaces";
    const auto invalid_instance = server.ConfigureBossMatch(invalid_instance_config);
    CHECK_TRUE(!invalid_instance.ok);
    CHECK_EQ(invalid_instance.reason, std::string("boss_instance_id_invalid"));
    CHECK_EQ(invalid_instance.pending_boss_configs_after, static_cast<std::size_t>(0));

    phk::battle::BossMatchConfig invalid_season_config;
    invalid_season_config.match_id = "match-invalid-season";
    invalid_season_config.mode_id = "world_boss";
    invalid_season_config.boss_season_id = "season/with/slash";
    const auto invalid_season = server.ConfigureBossMatch(invalid_season_config);
    CHECK_TRUE(!invalid_season.ok);
    CHECK_EQ(invalid_season.reason, std::string("boss_season_id_invalid"));
    CHECK_EQ(invalid_season.pending_boss_configs_after, static_cast<std::size_t>(0));

    phk::battle::BossMatchConfig invalid_phase_config;
    invalid_phase_config.match_id = "match-invalid-phase";
    invalid_phase_config.mode_id = "world_boss";
    invalid_phase_config.boss_phase_id = "phase#frag";
    const auto invalid_phase = server.ConfigureBossMatch(invalid_phase_config);
    CHECK_TRUE(!invalid_phase.ok);
    CHECK_EQ(invalid_phase.reason, std::string("boss_phase_id_invalid"));
    CHECK_EQ(invalid_phase.pending_boss_configs_after, static_cast<std::size_t>(0));

    phk::battle::BossMatchConfig boss_config;
    boss_config.match_id = "match-001";
    boss_config.mode_id = "world_boss";
    boss_config.boss_instance_id = "world-boss-season-042";
    boss_config.boss_season_id = "season-beta";
    boss_config.boss_phase_id = "phase-enrage";
    boss_config.boss_max_hp = 4242;
    boss_config.boss_friendly_fire_policy = "client_authored_damage";
    const auto configured = server.ConfigureBossMatch(boss_config);
    CHECK_TRUE(configured.ok);
    CHECK_EQ(configured.reason, std::string("ok"));
    CHECK_EQ(configured.active_sessions_before, static_cast<std::size_t>(0));
    CHECK_EQ(configured.active_matches_before, static_cast<std::size_t>(0));
    CHECK_EQ(configured.pending_boss_configs_before, static_cast<std::size_t>(0));
    CHECK_EQ(configured.active_sessions_after, static_cast<std::size_t>(0));
    CHECK_EQ(configured.active_matches_after, static_cast<std::size_t>(0));
    CHECK_EQ(configured.pending_boss_configs_after, static_cast<std::size_t>(1));

    auto duplicate_pending_config = boss_config;
    duplicate_pending_config.boss_instance_id = "world-boss-client-overwrite";
    duplicate_pending_config.boss_max_hp = 999999;
    const auto duplicate_pending = server.ConfigureBossMatch(duplicate_pending_config);
    CHECK_TRUE(!duplicate_pending.ok);
    CHECK_EQ(duplicate_pending.reason, std::string("boss_config_already_pending"));
    CHECK_EQ(duplicate_pending.active_sessions_before, static_cast<std::size_t>(0));
    CHECK_EQ(duplicate_pending.active_matches_before, static_cast<std::size_t>(0));
    CHECK_EQ(duplicate_pending.pending_boss_configs_before, static_cast<std::size_t>(1));
    CHECK_EQ(duplicate_pending.active_sessions_after, static_cast<std::size_t>(0));
    CHECK_EQ(duplicate_pending.active_matches_after, static_cast<std::size_t>(0));
    CHECK_EQ(duplicate_pending.pending_boss_configs_after, static_cast<std::size_t>(1));

    const auto wrong_mode = server.RegisterTicket(MakeModeTicket(
        "ticket-boss-preconfig-wrong-mode",
        "user-boss-preconfig-wrong-mode",
        "p1",
        "instance_boss",
        "00112233445566778899ab01"
    ));
    CHECK_TRUE(!wrong_mode.ok);
    CHECK_EQ(wrong_mode.reason, std::string("boss_config_mode_mismatch"));
    CHECK_EQ(wrong_mode.pending_boss_configs_before, static_cast<std::size_t>(1));
    CHECK_EQ(wrong_mode.pending_boss_configs_after, static_cast<std::size_t>(1));
    CHECK_EQ(server.ActiveMatchCount(), static_cast<std::size_t>(0));

    const auto first_boss_ticket = server.RegisterTicket(MakeModeTicket(
        "ticket-boss-preconfig-1",
        "user-boss-preconfig-1",
        "p1",
        "world_boss",
        "00112233445566778899ab02"
    ));
    CHECK_TRUE(first_boss_ticket.ok);
    CHECK_TRUE(first_boss_ticket.created_match);
    CHECK_EQ(first_boss_ticket.pending_boss_configs_before, static_cast<std::size_t>(1));
    CHECK_EQ(first_boss_ticket.pending_boss_configs_after, static_cast<std::size_t>(0));
    const auto snapshot = server.MatchSnapshot("match-001");
    CHECK_EQ(snapshot.mode_state.at("boss_instance_id"), std::string("world-boss-season-042"));
    CHECK_EQ(snapshot.mode_state.at("boss_season_id"), std::string("season-beta"));
    CHECK_EQ(snapshot.mode_state.at("boss_phase_id"), std::string("phase-enrage"));
    CHECK_EQ(snapshot.mode_state.at("boss_max_hp"), std::string("4242"));
    CHECK_EQ(snapshot.mode_state.at("boss_current_hp"), std::string("4242"));
    CHECK_EQ(snapshot.mode_state.at("boss_friendly_fire_policy"), std::string("disabled"));

    const auto duplicate_config = server.ConfigureBossMatch(boss_config);
    CHECK_TRUE(!duplicate_config.ok);
    CHECK_EQ(duplicate_config.reason, std::string("match_already_created"));

    for (std::size_t index = 2; index <= 4; ++index) {
        CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
            "ticket-boss-preconfig-" + std::to_string(index),
            "user-boss-preconfig-" + std::to_string(index),
            "p" + std::to_string(index),
            "world_boss",
            "00112233445566778899ab0" + std::to_string(index + 1)
        )).ok);
    }
    for (std::size_t index = 1; index <= 4; ++index) {
        auto ready = MakeModeAction(index);
        ready.match_id = "match-001";
        ready.player_id = "p" + std::to_string(index);
        ready.tick = 1;
        ready.seq = 1;
        ready.action_id = "boss-preconfig-ready-" + std::to_string(index);
        ready.action_type = "ready";
        ready.payload_json = "{\"ready\":true}";
        CHECK_TRUE(server.AcceptModeAction(ready).ok);
    }
    CHECK_EQ(server.TickMatch("match-001").mode_state.at("boss_ready_to_start"), std::string("1"));
    const auto built = server.BuildSignedBattleResult("match-001");
    CHECK_TRUE(built.ok);
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find("\"boss_instance_id\":\"world-boss-season-042\"") !=
        std::string::npos
    );
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_max_hp\":4242") != std::string::npos);
    CHECK_TRUE(server.SubmitBattleResult(built.signed_result).ok);
    CHECK_TRUE(server.RetireMatch("match-001").ok);

    const auto retired_config = server.ConfigureBossMatch(boss_config);
    CHECK_TRUE(!retired_config.ok);
    CHECK_EQ(retired_config.reason, std::string("match_retired"));
    return true;
}

bool TestBossModeCapacityGuard() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489605000;
    config.max_players = 10;
    phk::battle::BattleServer server(config);

    for (std::size_t index = 1; index <= 8; ++index) {
        CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
            "ticket-boss-cap-" + std::to_string(index),
            "user-boss-cap-" + std::to_string(index),
            "p" + std::to_string(index),
            "instance_boss",
            "00112233445566778899bb0" + std::to_string(index)
        )).ok);
    }

    const auto rejected = server.RegisterTicket(MakeModeTicket(
        "ticket-boss-cap-9",
        "user-boss-cap-9",
        "p9",
        "instance_boss",
        "00112233445566778899bb09"
    ));
    CHECK_TRUE(!rejected.ok);
    CHECK_EQ(rejected.reason, std::string("match_full"));

    const auto snapshot = server.MatchSnapshot("match-001");
    CHECK_EQ(snapshot.players.size(), static_cast<std::size_t>(8));
    CHECK_EQ(snapshot.mode_state.at("boss_registered_player_count"), std::string("8"));
    CHECK_EQ(snapshot.mode_state.at("boss_layout_player_count"), std::string("8"));
    CHECK_EQ(snapshot.mode_state.at("boss_start_ready"), std::string("1"));
    CHECK_EQ(snapshot.mode_state.at("boss_all_registered_connected"), std::string("1"));
    CHECK_EQ(snapshot.mode_state.at("boss_all_registered_ready"), std::string("0"));
    CHECK_EQ(snapshot.mode_state.at("boss_ready_to_start"), std::string("0"));
    CHECK_EQ(snapshot.mode_state.at("boss_max_players"), std::string("8"));
    CHECK_EQ(snapshot.mode_state.at("boss_player_p1_spawn_slot"), std::string("north"));
    CHECK_EQ(snapshot.mode_state.at("boss_player_p1_fire_target"), std::string("boss_center"));
    CHECK_EQ(snapshot.mode_state.at("boss_player_p2_spawn_slot"), std::string("east"));
    CHECK_EQ(snapshot.mode_state.at("boss_player_p2_fire_target"), std::string("boss_center"));
    CHECK_EQ(snapshot.mode_state.at("boss_player_p3_spawn_slot"), std::string("south"));
    CHECK_EQ(snapshot.mode_state.at("boss_player_p3_fire_target"), std::string("boss_center"));
    CHECK_EQ(snapshot.mode_state.at("boss_player_p4_spawn_slot"), std::string("west"));
    CHECK_EQ(snapshot.mode_state.at("boss_player_p4_fire_target"), std::string("boss_center"));
    CHECK_EQ(snapshot.mode_state.at("boss_player_p5_spawn_slot"), std::string("northeast"));
    CHECK_EQ(snapshot.mode_state.at("boss_player_p5_fire_target"), std::string("boss_center"));
    CHECK_EQ(snapshot.mode_state.at("boss_player_p6_spawn_slot"), std::string("southeast"));
    CHECK_EQ(snapshot.mode_state.at("boss_player_p6_fire_target"), std::string("boss_center"));
    CHECK_EQ(snapshot.mode_state.at("boss_player_p7_spawn_slot"), std::string("southwest"));
    CHECK_EQ(snapshot.mode_state.at("boss_player_p7_fire_target"), std::string("boss_center"));
    CHECK_EQ(snapshot.mode_state.at("boss_player_p8_spawn_slot"), std::string("northwest"));
    CHECK_EQ(snapshot.mode_state.at("boss_player_p8_fire_target"), std::string("boss_center"));
    CHECK_EQ(snapshot.players[4].x_milli, 42426);
    CHECK_EQ(snapshot.players[4].y_milli, -42426);
    CHECK_EQ(snapshot.players[5].x_milli, 42426);
    CHECK_EQ(snapshot.players[5].y_milli, 42426);
    CHECK_EQ(snapshot.players[6].x_milli, -42426);
    CHECK_EQ(snapshot.players[6].y_milli, 42426);
    CHECK_EQ(snapshot.players[7].x_milli, -42426);
    CHECK_EQ(snapshot.players[7].y_milli, -42426);

    phk::battle::BattleServer pvp_server(config);
    for (std::size_t index = 1; index <= 10; ++index) {
        const std::string nonce_hex = std::string("00112233445566778899cc") +
            (index < 10 ? "0" : "") +
            std::to_string(index);
        CHECK_TRUE(pvp_server.RegisterTicket(MakeModeTicket(
            "ticket-pvp-cap-" + std::to_string(index),
            "user-pvp-cap-" + std::to_string(index),
            "p" + std::to_string(index),
            "pvp_duel",
            nonce_hex
        )).ok);
    }
    CHECK_EQ(pvp_server.MatchSnapshot("match-001").players.size(), static_cast<std::size_t>(10));
    return true;
}

bool TestBossSimulationRejectsNinthPlayer() {
    phk::battle::SimulationConfig config;
    config.match_id = "match-boss-simulation-cap";
    config.mode_id = "world_boss";
    phk::battle::BattleSimulation simulation(config);

    for (std::size_t index = 1; index <= phk::battle::kBossModeMaxPlayers; ++index) {
        CHECK_TRUE(simulation.AddPlayer(
            "p" + std::to_string(index),
            static_cast<std::int32_t>(index * 1000),
            0
        ));
    }

    CHECK_TRUE(!simulation.AddPlayer("p9", 9000, 0));
    CHECK_EQ(simulation.PlayerCount(), phk::battle::kBossModeMaxPlayers);
    const auto snapshot = simulation.Snapshot();
    CHECK_EQ(snapshot.players.size(), phk::battle::kBossModeMaxPlayers);
    CHECK_EQ(snapshot.mode_state.at("boss_registered_player_count"), std::string("8"));
    CHECK_EQ(snapshot.mode_state.at("boss_layout_player_count"), std::string("8"));
    CHECK_TRUE(snapshot.mode_state.find("boss_player_p9_spawn_slot") == snapshot.mode_state.end());

    phk::battle::SimulationConfig pvp_config;
    pvp_config.match_id = "match-pvp-simulation-cap";
    pvp_config.mode_id = "pvp_duel";
    phk::battle::BattleSimulation pvp_simulation(pvp_config);
    for (std::size_t index = 1; index <= phk::battle::kBossModeMaxPlayers + 1; ++index) {
        CHECK_TRUE(pvp_simulation.AddPlayer(
            "p" + std::to_string(index),
            static_cast<std::int32_t>(index * 1000),
            0
        ));
    }
    CHECK_EQ(pvp_simulation.PlayerCount(), phk::battle::kBossModeMaxPlayers + 1);
    return true;
}

bool TestBossStartReadinessTracksConnectedPlayers() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489605000;
    phk::battle::BattleServer server(config);

    for (std::size_t index = 1; index <= 4; ++index) {
        CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
            "ticket-boss-ready-" + std::to_string(index),
            "user-boss-ready-" + std::to_string(index),
            "p" + std::to_string(index),
            "world_boss",
            "00112233445566778899dd0" + std::to_string(index)
        )).ok);
    }
    CHECK_EQ(server.MatchSnapshot("match-001").mode_state.at("boss_start_ready"), std::string("1"));
    CHECK_EQ(server.MatchSnapshot("match-001").mode_state.at("boss_ready_to_start"), std::string("0"));
    CHECK_EQ(server.MatchSnapshot("match-001").mode_state.at("boss_lifecycle_state"), std::string("waiting_for_ready"));

    CHECK_TRUE(server.SetPlayerConnected("match-001", "p4", false).ok);
    const auto disconnected_snapshot = server.MatchSnapshot("match-001");
    CHECK_EQ(disconnected_snapshot.mode_state.at("connected_player_count"), std::string("3"));
    CHECK_EQ(disconnected_snapshot.mode_state.at("disconnected_player_count"), std::string("1"));
    CHECK_EQ(disconnected_snapshot.mode_state.at("boss_registered_player_count"), std::string("4"));
    CHECK_EQ(disconnected_snapshot.mode_state.at("boss_all_registered_connected"), std::string("0"));
    CHECK_EQ(disconnected_snapshot.mode_state.at("boss_all_registered_ready"), std::string("0"));
    CHECK_EQ(disconnected_snapshot.mode_state.at("boss_start_ready"), std::string("0"));
    CHECK_EQ(disconnected_snapshot.mode_state.at("boss_ready_to_start"), std::string("0"));
    CHECK_EQ(disconnected_snapshot.mode_state.at("boss_lifecycle_state"), std::string("waiting_for_players"));

    CHECK_TRUE(server.SetPlayerConnected("match-001", "p4", true).ok);
    const auto reconnected_snapshot = server.MatchSnapshot("match-001");
    CHECK_EQ(reconnected_snapshot.mode_state.at("connected_player_count"), std::string("4"));
    CHECK_EQ(reconnected_snapshot.mode_state.at("disconnected_player_count"), std::string("0"));
    CHECK_EQ(reconnected_snapshot.mode_state.at("boss_registered_player_count"), std::string("4"));
    CHECK_EQ(reconnected_snapshot.mode_state.at("boss_all_registered_connected"), std::string("1"));
    CHECK_EQ(reconnected_snapshot.mode_state.at("boss_all_registered_ready"), std::string("0"));
    CHECK_EQ(reconnected_snapshot.mode_state.at("boss_start_ready"), std::string("1"));
    CHECK_EQ(reconnected_snapshot.mode_state.at("boss_ready_to_start"), std::string("0"));
    CHECK_EQ(reconnected_snapshot.mode_state.at("boss_lifecycle_state"), std::string("waiting_for_ready"));
    return true;
}

bool TestBossReadyToStartRequiresAllReadyPlayers() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489605000;
    phk::battle::BattleServer server(config);

    for (std::size_t index = 1; index <= 4; ++index) {
        CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
            "ticket-boss-all-ready-" + std::to_string(index),
            "user-boss-all-ready-" + std::to_string(index),
            "p" + std::to_string(index),
            "instance_boss",
            "00112233445566778899ee0" + std::to_string(index)
        )).ok);
    }

    for (std::size_t index = 1; index <= 4; ++index) {
        auto ready = MakeModeAction(index);
        ready.match_id = "match-001";
        ready.player_id = "p" + std::to_string(index);
        ready.tick = 1;
        ready.seq = 1;
        ready.action_id = "boss-ready-" + std::to_string(index);
        ready.action_type = "ready";
        ready.payload_json = "{\"ready\":true}";
        CHECK_TRUE(server.AcceptModeAction(ready).ok);
    }

    const auto ready_snapshot = server.TickMatch("match-001");
    CHECK_EQ(ready_snapshot.mode_state.at("boss_start_ready"), std::string("1"));
    CHECK_EQ(ready_snapshot.mode_state.at("boss_registered_player_count"), std::string("4"));
    CHECK_EQ(ready_snapshot.mode_state.at("boss_ready_player_count"), std::string("4"));
    CHECK_EQ(ready_snapshot.mode_state.at("boss_all_registered_connected"), std::string("1"));
    CHECK_EQ(ready_snapshot.mode_state.at("boss_all_registered_ready"), std::string("1"));
    CHECK_EQ(ready_snapshot.mode_state.at("all_players_ready"), std::string("1"));
    CHECK_EQ(ready_snapshot.mode_state.at("boss_ready_to_start"), std::string("1"));
    CHECK_EQ(ready_snapshot.mode_state.at("boss_lifecycle_state"), std::string("start_ready"));

    CHECK_TRUE(server.SetPlayerConnected("match-001", "p4", false).ok);
    const auto disconnected_snapshot = server.MatchSnapshot("match-001");
    CHECK_EQ(disconnected_snapshot.mode_state.at("boss_start_ready"), std::string("0"));
    CHECK_EQ(disconnected_snapshot.mode_state.at("boss_registered_player_count"), std::string("4"));
    CHECK_EQ(disconnected_snapshot.mode_state.at("boss_ready_player_count"), std::string("3"));
    CHECK_EQ(disconnected_snapshot.mode_state.at("boss_all_registered_connected"), std::string("0"));
    CHECK_EQ(disconnected_snapshot.mode_state.at("boss_all_registered_ready"), std::string("0"));
    CHECK_EQ(disconnected_snapshot.mode_state.at("all_players_ready"), std::string("0"));
    CHECK_EQ(disconnected_snapshot.mode_state.at("boss_ready_to_start"), std::string("0"));
    CHECK_EQ(disconnected_snapshot.mode_state.at("boss_lifecycle_state"), std::string("waiting_for_players"));
    return true;
}

bool TestBossModeBulletPattern() {
    phk::battle::SimulationConfig world_config;
    world_config.match_id = "match-world-boss-pattern";
    world_config.mode_id = "world_boss";
    world_config.match_seed = 99;
    world_config.spawn_period_ticks = 1;
    world_config.max_bullets = 16;
    world_config.boss_friendly_fire_policy = "player_bullets_only";
    world_config.boss_instance_id = "world-boss-season-001";
    world_config.boss_season_id = "season-alpha";
    world_config.boss_phase_id = "phase-opening";
    phk::battle::BattleSimulation world_simulation(world_config);
    CHECK_TRUE(world_simulation.AddPlayer("p1", 0, -60000));
    const auto world_snapshot = world_simulation.Tick();
    CHECK_EQ(world_snapshot.bullets_delta.size(), static_cast<std::size_t>(8));
    CHECK_EQ(world_snapshot.bullets_delta[0].pattern_id, std::string("boss_center_radial"));
    CHECK_EQ(world_snapshot.bullets_delta[0].color, std::string("ruby"));
    CHECK_EQ(world_snapshot.bullets_delta[0].radius_milli, static_cast<std::uint32_t>(5000));
    CHECK_EQ(world_snapshot.bullets_delta[0].x_milli, 0);
    CHECK_EQ(world_snapshot.bullets_delta[0].y_milli, 2600);
    CHECK_TRUE(world_simulation.Summary().event_trace.back().find("pattern=boss_center_radial") != std::string::npos);
    CHECK_EQ(world_snapshot.mode_state.at("battle_layout"), std::string("boss_center_ring"));
    CHECK_EQ(world_snapshot.mode_state.at("boss_start_ready"), std::string("0"));
    CHECK_EQ(world_snapshot.mode_state.at("boss_instance_id"), std::string("world-boss-season-001"));
    CHECK_EQ(world_snapshot.mode_state.at("boss_season_id"), std::string("season-alpha"));
    CHECK_EQ(world_snapshot.mode_state.at("boss_phase_id"), std::string("phase-opening"));
    CHECK_EQ(world_snapshot.mode_state.at("boss_friendly_fire_policy"), std::string("player_bullets_only"));

    phk::battle::SimulationConfig instance_config = world_config;
    instance_config.match_id = "match-instance-boss-pattern";
    instance_config.mode_id = "instance_boss";
    instance_config.boss_instance_id.clear();
    instance_config.boss_season_id.clear();
    instance_config.boss_phase_id.clear();
    instance_config.boss_friendly_fire_policy = "all_friendly_fire";
    phk::battle::BattleSimulation instance_simulation(instance_config);
    CHECK_TRUE(instance_simulation.AddPlayer("p1", 0, -60000));
    const auto instance_snapshot = instance_simulation.Tick();
    CHECK_EQ(instance_snapshot.bullets_delta.size(), static_cast<std::size_t>(8));
    CHECK_EQ(instance_snapshot.bullets_delta[0].pattern_id, std::string("boss_center_radial"));
    CHECK_EQ(instance_snapshot.bullets_delta[0].color, std::string("violet"));
    CHECK_EQ(instance_snapshot.bullets_delta[0].radius_milli, static_cast<std::uint32_t>(5000));
    CHECK_TRUE(instance_simulation.Summary().event_trace.back().find("pattern=boss_center_radial") != std::string::npos);
    CHECK_EQ(
        instance_snapshot.mode_state.at("boss_instance_id"),
        std::string("instance-boss:match-instance-boss-pattern")
    );
    CHECK_EQ(instance_snapshot.mode_state.at("boss_season_id"), std::string("season-local-s0"));
    CHECK_EQ(instance_snapshot.mode_state.at("boss_phase_id"), std::string("phase-1"));
    CHECK_EQ(instance_snapshot.mode_state.at("boss_friendly_fire_policy"), std::string("all_friendly_fire"));

    phk::battle::SimulationConfig invalid_policy_config = world_config;
    invalid_policy_config.match_id = "match-world-boss-invalid-friendly-fire";
    invalid_policy_config.boss_friendly_fire_policy = "client_authored_damage";
    phk::battle::BattleSimulation invalid_policy_simulation(invalid_policy_config);
    CHECK_TRUE(invalid_policy_simulation.AddPlayer("p1", 0, -60000));
    CHECK_EQ(
        invalid_policy_simulation.Snapshot().mode_state.at("boss_friendly_fire_policy"),
        std::string("disabled")
    );

    phk::battle::SimulationConfig invalid_identity_config = world_config;
    invalid_identity_config.match_id = "match-world-boss-invalid-identity";
    invalid_identity_config.boss_instance_id = "world-boss\"\nclient";
    invalid_identity_config.boss_season_id = std::string(phk::battle::kDefaultMaxBossIdentityBytes + 1, 's');
    invalid_identity_config.boss_phase_id = "phase/client";
    phk::battle::BattleSimulation invalid_identity_simulation(invalid_identity_config);
    CHECK_TRUE(invalid_identity_simulation.AddPlayer("p1", 0, -60000));
    const auto invalid_identity_snapshot = invalid_identity_simulation.Snapshot();
    CHECK_EQ(
        invalid_identity_snapshot.mode_state.at("boss_instance_id"),
        std::string("world-boss:match-world-boss-invalid-identity")
    );
    CHECK_EQ(invalid_identity_snapshot.mode_state.at("boss_season_id"), std::string("season-local-s0"));
    CHECK_EQ(invalid_identity_snapshot.mode_state.at("boss_phase_id"), std::string("phase-1"));
    return true;
}

bool TestBossModeAuthoritativeDamageState() {
    phk::battle::SimulationConfig world_config;
    world_config.match_id = "match-world-boss-damage";
    world_config.mode_id = "world_boss";
    world_config.match_seed = 100;
    world_config.spawn_period_ticks = 1000;
    world_config.boss_max_hp = 100;
    phk::battle::BattleSimulation world_simulation(world_config);
    CHECK_TRUE(world_simulation.AddPlayer("p1", 0, -60000));
    CHECK_TRUE(world_simulation.AddPlayer("p2", 60000, 0));
    CHECK_TRUE(world_simulation.AddPlayer("p3", 0, 60000));
    CHECK_TRUE(world_simulation.AddPlayer("p4", -60000, 0));

    auto p1_shoot = MakeInput("p1", 1, 1, 0);
    p1_shoot.match_id = world_config.match_id;
    p1_shoot.shoot = true;
    auto p2_shoot = MakeInput("p2", 1, 1, 0);
    p2_shoot.match_id = world_config.match_id;
    p2_shoot.shoot = true;
    CHECK_TRUE(world_simulation.AcceptInput(p1_shoot).ok);
    CHECK_TRUE(world_simulation.AcceptInput(p2_shoot).ok);
    const auto not_started_snapshot = world_simulation.Tick();
    CHECK_EQ(not_started_snapshot.mode_state.at("boss_current_hp"), std::string("100"));
    CHECK_EQ(not_started_snapshot.mode_state.at("boss_damage_total"), std::string("0"));
    CHECK_EQ(not_started_snapshot.mode_state.at("boss_combat_started"), std::string("0"));

    for (std::size_t index = 1; index <= 4; ++index) {
        auto ready = MakeModeAction(index + 1);
        ready.match_id = world_config.match_id;
        ready.player_id = "p" + std::to_string(index);
        ready.tick = 2;
        ready.seq = index <= 2 ? 2 : 1;
        ready.action_id = "world-damage-ready-" + std::to_string(index);
        ready.action_type = "ready";
        ready.payload_json = "{\"ready\":true}";
        CHECK_TRUE(world_simulation.AcceptModeAction(ready).ok);
    }
    p1_shoot.tick = 2;
    p1_shoot.seq = 3;
    p2_shoot.tick = 2;
    p2_shoot.seq = 3;
    CHECK_TRUE(world_simulation.AcceptInput(p1_shoot).ok);
    CHECK_TRUE(world_simulation.AcceptInput(p2_shoot).ok);
    const auto damaged_snapshot = world_simulation.Tick();
    CHECK_EQ(damaged_snapshot.mode_state.at("boss_scope"), std::string("world_persistent"));
    CHECK_EQ(damaged_snapshot.mode_state.at("boss_completion_policy"), std::string("damage_report_to_business"));
    CHECK_EQ(damaged_snapshot.mode_state.at("boss_max_hp"), std::string("100"));
    CHECK_EQ(damaged_snapshot.mode_state.at("boss_current_hp"), std::string("80"));
    CHECK_EQ(damaged_snapshot.mode_state.at("boss_damage_total"), std::string("20"));
    CHECK_EQ(damaged_snapshot.mode_state.at("boss_damage_p1"), std::string("10"));
    CHECK_EQ(damaged_snapshot.mode_state.at("boss_damage_p2"), std::string("10"));
    CHECK_EQ(damaged_snapshot.mode_state.at("boss_defeated"), std::string("0"));
    CHECK_EQ(damaged_snapshot.mode_state.at("boss_combat_started"), std::string("1"));
    CHECK_EQ(damaged_snapshot.mode_state.at("boss_clear_status"), std::string("running"));
    CHECK_EQ(damaged_snapshot.mode_state.at("boss_result_disposition"), std::string("world_damage_report"));

    CHECK_TRUE(world_simulation.SetPlayerConnected("p2", false).ok);
    auto disconnected_input = p2_shoot;
    disconnected_input.tick = 3;
    disconnected_input.seq = 4;
    const auto disconnected_result = world_simulation.AcceptInput(disconnected_input);
    CHECK_TRUE(!disconnected_result.ok);
    CHECK_EQ(disconnected_result.reason, std::string("player_disconnected"));
    p1_shoot.tick = 3;
    p1_shoot.seq = 4;
    CHECK_TRUE(world_simulation.AcceptInput(p1_shoot).ok);
    const auto disconnected_damage_snapshot = world_simulation.Tick();
    CHECK_EQ(disconnected_damage_snapshot.mode_state.at("boss_current_hp"), std::string("70"));
    CHECK_EQ(disconnected_damage_snapshot.mode_state.at("boss_damage_total"), std::string("30"));
    CHECK_EQ(disconnected_damage_snapshot.mode_state.at("boss_damage_p1"), std::string("20"));
    CHECK_EQ(disconnected_damage_snapshot.mode_state.at("boss_damage_p2"), std::string("10"));
    CHECK_EQ(disconnected_damage_snapshot.mode_state.at("connected_player_count"), std::string("3"));
    CHECK_EQ(disconnected_damage_snapshot.mode_state.at("disconnected_player_count"), std::string("1"));

    phk::battle::SimulationConfig instance_config;
    instance_config.match_id = "match-instance-boss-damage";
    instance_config.mode_id = "instance_boss";
    instance_config.spawn_period_ticks = 1000;
    instance_config.boss_max_hp = 20;
    phk::battle::BattleSimulation instance_simulation(instance_config);
    CHECK_TRUE(instance_simulation.AddPlayer("p1", 0, -60000));
    CHECK_TRUE(instance_simulation.AddPlayer("p2", 60000, 0));
    CHECK_TRUE(instance_simulation.AddPlayer("p3", 0, 60000));
    CHECK_TRUE(instance_simulation.AddPlayer("p4", -60000, 0));
    p1_shoot.match_id = instance_config.match_id;
    p1_shoot.tick = 1;
    p1_shoot.seq = 1;
    p2_shoot.match_id = instance_config.match_id;
    p2_shoot.tick = 1;
    p2_shoot.seq = 1;
    CHECK_TRUE(instance_simulation.AcceptInput(p1_shoot).ok);
    CHECK_TRUE(instance_simulation.AcceptInput(p2_shoot).ok);
    for (std::size_t index = 1; index <= 4; ++index) {
        auto ready = MakeModeAction(index + 1);
        ready.match_id = instance_config.match_id;
        ready.player_id = "p" + std::to_string(index);
        ready.tick = 1;
        ready.seq = index <= 2 ? 2 : 1;
        ready.action_id = "instance-damage-ready-" + std::to_string(index);
        ready.action_type = "ready";
        ready.payload_json = "{\"ready\":true}";
        CHECK_TRUE(instance_simulation.AcceptModeAction(ready).ok);
    }
    const auto defeated_snapshot = instance_simulation.Tick();
    CHECK_EQ(defeated_snapshot.mode_state.at("boss_scope"), std::string("instance_match"));
    CHECK_EQ(defeated_snapshot.mode_state.at("boss_completion_policy"), std::string("defeat_required"));
    CHECK_EQ(defeated_snapshot.mode_state.at("boss_current_hp"), std::string("0"));
    CHECK_EQ(defeated_snapshot.mode_state.at("boss_damage_total"), std::string("20"));
    CHECK_EQ(defeated_snapshot.mode_state.at("boss_defeated"), std::string("1"));
    CHECK_EQ(defeated_snapshot.mode_state.at("boss_defeated_tick"), std::string("1"));
    CHECK_EQ(defeated_snapshot.mode_state.at("boss_combat_started"), std::string("1"));
    CHECK_EQ(defeated_snapshot.mode_state.at("boss_clear_status"), std::string("cleared"));
    CHECK_EQ(defeated_snapshot.mode_state.at("boss_result_disposition"), std::string("instance_cleared"));
    CHECK_EQ(instance_simulation.Summary().event_count, static_cast<std::uint64_t>(5));
    bool saw_boss_defeated_event = false;
    for (const auto& trace : instance_simulation.Summary().event_trace) {
        saw_boss_defeated_event =
            saw_boss_defeated_event || trace.find("boss_defeated|tick=1") != std::string::npos;
    }
    CHECK_TRUE(saw_boss_defeated_event);

    phk::battle::SimulationConfig pvp_config;
    pvp_config.match_id = "match-pvp-no-boss-damage";
    pvp_config.mode_id = "pvp_duel";
    phk::battle::BattleSimulation pvp_simulation(pvp_config);
    CHECK_TRUE(pvp_simulation.AddPlayer("p1", -20000, 0));
    const auto pvp_snapshot = pvp_simulation.Snapshot();
    CHECK_TRUE(pvp_snapshot.mode_state.find("boss_current_hp") == pvp_snapshot.mode_state.end());
    return true;
}

bool TestBossModeResultProjection() {
    phk::battle::SimulationConfig instance_config;
    instance_config.match_id = "match-instance-boss-result";
    instance_config.mode_id = "instance_boss";
    instance_config.spawn_period_ticks = 1000;
    instance_config.boss_max_hp = 20;
    instance_config.boss_friendly_fire_policy = "all_friendly_fire";
    instance_config.boss_instance_id = "instance-boss-result-001";
    instance_config.boss_season_id = "instance-season-s0";
    instance_config.boss_phase_id = "instance-phase-a";
    phk::battle::BattleSimulation simulation(instance_config);
    CHECK_TRUE(AddBossFixturePlayers(simulation));

    auto p1_shoot = MakeInput("p1", 1, 1, 0);
    p1_shoot.match_id = instance_config.match_id;
    p1_shoot.shoot = true;
    auto p2_shoot = MakeInput("p2", 1, 1, 0);
    p2_shoot.match_id = instance_config.match_id;
    p2_shoot.shoot = true;
    auto transfer = MakeModeAction(1);
    transfer.match_id = instance_config.match_id;
    transfer.tick = 1;
    transfer.seq = 2;
    transfer.action_id = "action-instance-transfer";
    transfer.action_type = "transfer_card";
    transfer.payload_json = "{\"target_player_id\":\"p2\",\"card_instance_id\":\"instance-card-001\"}";
    phk::battle::TransferableCardState transfer_card;
    transfer_card.card_instance_id = "instance-card-001";
    transfer_card.owner_player_id = "p1";
    CHECK_TRUE(simulation.ConfigureTransferableCard(transfer_card));
    CHECK_TRUE(simulation.AcceptInput(p1_shoot).ok);
    CHECK_TRUE(simulation.AcceptInput(p2_shoot).ok);
    CHECK_TRUE(simulation.AcceptModeAction(transfer).ok);
    CHECK_TRUE(AcceptBossReadyActions(
        simulation,
        instance_config.match_id,
        1,
        3,
        2,
        1,
        1,
        "instance-result-projection-ready"
    ));
    CHECK_EQ(simulation.Tick().mode_state.at("boss_clear_status"), std::string("cleared"));

    const auto fixture = simulation.BuildReplayFixture("user-boss");
    const auto mode_result_json = phk::battle::DevModeResultJsonFromReplayFixture(fixture);
    CHECK_TRUE(mode_result_json.find("\"battle_result_owner\":\"cpp\"") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_scope\":\"instance_match\"") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_completion_policy\":\"defeat_required\"") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_instance_id\":\"instance-boss-result-001\"") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_season_id\":\"instance-season-s0\"") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_phase_id\":\"instance-phase-a\"") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_friendly_fire_policy\":\"all_friendly_fire\"") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_min_players\":4") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_max_players\":8") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_registered_player_count\":4") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_start_ready\":1") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_ready_player_count\":4") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_all_registered_connected\":1") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_all_registered_ready\":1") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_ready_to_start\":1") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"connected_player_count\":4") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"disconnected_player_count\":0") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_max_hp\":20") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_current_hp\":0") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_damage_total\":20") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_damage_p1\":10") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_player_p1_spawn_slot\":\"north\"") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_player_p1_fire_target\":\"boss_center\"") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_damage_p2\":10") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_player_p2_spawn_slot\":\"east\"") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_player_p2_fire_target\":\"boss_center\"") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_damage_p3\":0") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_damage_p4\":0") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_defeated\":1") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_defeated_tick\":1") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_clear_status\":\"cleared\"") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_result_disposition\":\"instance_cleared\"") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_instance_surviving_player_count\":4") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_instance_clear_credit\":1") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"boss_instance_result_state\":\"cleared\"") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"transfer_card_count\":1") != std::string::npos);
    CHECK_TRUE(
        mode_result_json.find("\"transfer_card_edges_material\":\"instance-card-001:p1>p2:p1:1:1:1;\"") !=
        std::string::npos
    );
    CHECK_TRUE(mode_result_json.find("\"last_transfer_card_instance_id\":\"instance-card-001\"") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"last_transfer_authority_owner_player_id\":\"p1\"") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"last_transfer_authority_mode_allowed\":1") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"last_transfer_authority_cost_paid\":1") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"last_transfer_authority_cooldown_ready\":1") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"last_transfer_from_player_id\":\"p1\"") != std::string::npos);
    CHECK_TRUE(mode_result_json.find("\"last_transfer_to_player_id\":\"p2\"") != std::string::npos);

    phk::battle::SimulationConfig pvp_config;
    pvp_config.match_id = "match-pvp-result";
    pvp_config.mode_id = "pvp_duel";
    phk::battle::BattleSimulation pvp_simulation(pvp_config);
    CHECK_TRUE(pvp_simulation.AddPlayer("p1", -20000, 0));
    const auto pvp_mode_result_json = phk::battle::DevModeResultJsonFromReplayFixture(
        pvp_simulation.BuildReplayFixture("user-pvp")
    );
    CHECK_TRUE(pvp_mode_result_json.find("boss_") == std::string::npos);
    CHECK_TRUE(pvp_mode_result_json.find("transfer_card_count") == std::string::npos);

    phk::battle::SimulationConfig incomplete_config;
    incomplete_config.match_id = "match-instance-boss-incomplete-result";
    incomplete_config.mode_id = "instance_boss";
    incomplete_config.spawn_period_ticks = 1000;
    incomplete_config.boss_max_hp = 100;
    phk::battle::BattleSimulation incomplete_simulation(incomplete_config);
    CHECK_TRUE(incomplete_simulation.AddPlayer("p1", 0, -60000));
    const auto incomplete_result_json = phk::battle::DevModeResultJsonFromReplayFixture(
        incomplete_simulation.BuildReplayFixture("user-boss-incomplete")
    );
    CHECK_TRUE(incomplete_result_json.find("\"boss_clear_status\":\"failed\"") != std::string::npos);
    CHECK_TRUE(incomplete_result_json.find("\"boss_result_disposition\":\"instance_failed\"") != std::string::npos);
    CHECK_TRUE(incomplete_result_json.find("\"boss_instance_surviving_player_count\":1") != std::string::npos);
    CHECK_TRUE(incomplete_result_json.find("\"boss_instance_clear_credit\":0") != std::string::npos);
    CHECK_TRUE(incomplete_result_json.find("\"boss_instance_result_state\":\"failed\"") != std::string::npos);
    return true;
}

bool TestInstanceBossResultStateMutualExclusion() {
    phk::battle::SimulationConfig failed_config;
    failed_config.match_id = "match-instance-boss-final-failed";
    failed_config.mode_id = "instance_boss";
    failed_config.spawn_period_ticks = 1000;
    failed_config.boss_max_hp = 100;
    phk::battle::BattleSimulation failed_simulation(failed_config);
    CHECK_TRUE(failed_simulation.AddPlayer("p1", 0, -60000));
    const auto running_snapshot = failed_simulation.Snapshot();
    CHECK_EQ(running_snapshot.mode_state.at("boss_clear_status"), std::string("running"));
    CHECK_EQ(running_snapshot.mode_state.at("boss_result_disposition"), std::string("instance_incomplete"));
    CHECK_EQ(running_snapshot.mode_state.at("boss_instance_result_state"), std::string("running"));
    const auto failed_fixture = failed_simulation.BuildReplayFixture("user-instance-failed");
    CHECK_EQ(failed_fixture.final_snapshot.mode_state.at("boss_clear_status"), std::string("failed"));
    CHECK_EQ(
        failed_fixture.final_snapshot.mode_state.at("boss_result_disposition"),
        std::string("instance_failed")
    );
    CHECK_EQ(failed_fixture.final_snapshot.mode_state.at("boss_instance_result_state"), std::string("failed"));
    CHECK_EQ(failed_fixture.final_snapshot.mode_state.at("boss_instance_surviving_player_count"), std::string("1"));
    CHECK_EQ(failed_fixture.final_snapshot.mode_state.at("boss_instance_clear_credit"), std::string("0"));

    phk::battle::SimulationConfig cleared_config = failed_config;
    cleared_config.match_id = "match-instance-boss-final-cleared";
    cleared_config.boss_max_hp = 10;
    phk::battle::BattleSimulation cleared_simulation(cleared_config);
    CHECK_TRUE(AddBossFixturePlayers(cleared_simulation));
    auto shoot = MakeInput("p1", 1, 1, 0);
    shoot.match_id = cleared_config.match_id;
    shoot.shoot = true;
    CHECK_TRUE(cleared_simulation.AcceptInput(shoot).ok);
    CHECK_TRUE(AcceptBossReadyActions(
        cleared_simulation,
        cleared_config.match_id,
        1,
        2,
        1,
        1,
        1,
        "instance-cleared-ready"
    ));
    CHECK_EQ(cleared_simulation.Tick().mode_state.at("boss_clear_status"), std::string("cleared"));
    const auto cleared_fixture = cleared_simulation.BuildReplayFixture("user-instance-cleared");
    CHECK_EQ(cleared_fixture.final_snapshot.mode_state.at("boss_clear_status"), std::string("cleared"));
    CHECK_EQ(
        cleared_fixture.final_snapshot.mode_state.at("boss_result_disposition"),
        std::string("instance_cleared")
    );
    CHECK_EQ(cleared_fixture.final_snapshot.mode_state.at("boss_instance_result_state"), std::string("cleared"));
    CHECK_EQ(cleared_fixture.final_snapshot.mode_state.at("boss_instance_surviving_player_count"), std::string("4"));
    CHECK_EQ(cleared_fixture.final_snapshot.mode_state.at("boss_instance_clear_credit"), std::string("1"));

    phk::battle::SimulationConfig no_survivor_config = failed_config;
    no_survivor_config.match_id = "match-instance-boss-no-survivor";
    no_survivor_config.boss_max_hp = 10;
    phk::battle::BattleSimulation no_survivor_simulation(no_survivor_config);
    CHECK_TRUE(AddBossFixturePlayers(no_survivor_simulation));
    shoot.match_id = no_survivor_config.match_id;
    shoot.tick = 1;
    shoot.seq = 1;
    CHECK_TRUE(no_survivor_simulation.AcceptInput(shoot).ok);
    CHECK_TRUE(AcceptBossReadyActions(
        no_survivor_simulation,
        no_survivor_config.match_id,
        1,
        2,
        1,
        1,
        1,
        "instance-no-survivor-ready"
    ));
    CHECK_EQ(no_survivor_simulation.Tick().mode_state.at("boss_clear_status"), std::string("cleared"));
    CHECK_TRUE(no_survivor_simulation.SetPlayerConnected("p1", false).ok);
    CHECK_TRUE(no_survivor_simulation.SetPlayerConnected("p2", false).ok);
    CHECK_TRUE(no_survivor_simulation.SetPlayerConnected("p3", false).ok);
    CHECK_TRUE(no_survivor_simulation.SetPlayerConnected("p4", false).ok);
    const auto no_survivor_fixture = no_survivor_simulation.BuildReplayFixture("user-instance-no-survivor");
    CHECK_EQ(no_survivor_fixture.final_snapshot.mode_state.at("boss_defeated"), std::string("1"));
    CHECK_EQ(no_survivor_fixture.final_snapshot.mode_state.at("boss_clear_status"), std::string("failed"));
    CHECK_EQ(
        no_survivor_fixture.final_snapshot.mode_state.at("boss_result_disposition"),
        std::string("instance_failed")
    );
    CHECK_EQ(no_survivor_fixture.final_snapshot.mode_state.at("boss_instance_result_state"), std::string("failed"));
    CHECK_EQ(no_survivor_fixture.final_snapshot.mode_state.at("boss_instance_surviving_player_count"), std::string("0"));
    CHECK_EQ(no_survivor_fixture.final_snapshot.mode_state.at("boss_instance_clear_credit"), std::string("0"));
    return true;
}

bool TestBossModeResultSubmissionRequiresBossProjection() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489640000;
    phk::battle::BattleServer server(config);
    CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
        "ticket-instance-result-1",
        "user-instance-result-1",
        "p1",
        "instance_boss",
        "00112233445566778899ef01"
    )).ok);
    CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
        "ticket-instance-result-2",
        "user-instance-result-2",
        "p2",
        "instance_boss",
        "00112233445566778899ef02"
    )).ok);
    CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
        "ticket-instance-result-3",
        "user-instance-result-3",
        "p3",
        "instance_boss",
        "00112233445566778899ef03"
    )).ok);
    CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
        "ticket-instance-result-4",
        "user-instance-result-4",
        "p4",
        "instance_boss",
        "00112233445566778899ef04"
    )).ok);

    phk::battle::TransferableCardState transfer_card;
    transfer_card.card_instance_id = "instance-result-card-001";
    transfer_card.owner_player_id = "p1";
    phk::battle::TransferableCardState transfer_card_2;
    transfer_card_2.card_instance_id = "instance-result-card-002";
    transfer_card_2.owner_player_id = "p2";
    CHECK_TRUE(server.AcceptInput(MakeInput("p1", 1, 1, 0)).ok);
    CHECK_TRUE(server.AcceptInput(MakeInput("p2", 1, 1, 0)).ok);
    CHECK_TRUE(server.ConfigureTransferableCard("match-001", transfer_card));
    CHECK_TRUE(server.ConfigureTransferableCard("match-001", transfer_card_2));

    auto transfer = MakeModeAction(5);
    transfer.match_id = "match-001";
    transfer.player_id = "p1";
    transfer.tick = 1;
    transfer.seq = 2;
    transfer.action_id = "action-instance-result-transfer";
    transfer.action_type = "transfer_card";
    transfer.payload_json = "{\"target_player_id\":\"p2\",\"card_instance_id\":\"instance-result-card-001\"}";
    CHECK_TRUE(server.AcceptModeAction(transfer).ok);

    auto transfer_2 = MakeModeAction(6);
    transfer_2.match_id = "match-001";
    transfer_2.player_id = "p2";
    transfer_2.tick = 1;
    transfer_2.seq = 2;
    transfer_2.action_id = "action-instance-result-transfer-2";
    transfer_2.action_type = "transfer_card";
    transfer_2.payload_json = "{\"target_player_id\":\"p3\",\"card_instance_id\":\"instance-result-card-002\"}";
    CHECK_TRUE(server.AcceptModeAction(transfer_2).ok);

    for (std::size_t index = 1; index <= 4; ++index) {
        auto ready = MakeModeAction(index + 1);
        ready.match_id = "match-001";
        ready.player_id = "p" + std::to_string(index);
        ready.tick = 1;
        ready.seq = index <= 2 ? 2 : 1;
        ready.action_id = "instance-result-ready-" + std::to_string(index);
        ready.action_type = "ready";
        ready.payload_json = "{\"ready\":true}";
        if (index == 1) {
            ready.seq = 3;
        } else if (index == 2) {
            ready.seq = 3;
        }
        CHECK_TRUE(server.AcceptModeAction(ready).ok);
    }
    CHECK_EQ(server.TickMatch("match-001").snapshot_tick, static_cast<std::uint64_t>(1));
    const auto built = server.BuildSignedBattleResult("match-001");
    CHECK_TRUE(built.ok);
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_scope\":\"instance_match\"") != std::string::npos);
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_completion_policy\":\"defeat_required\"") != std::string::npos);
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find("\"boss_instance_id\":\"instance-boss:match-001\"") !=
        std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find("\"boss_season_id\":\"season-local-s0\"") !=
        std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find("\"boss_phase_id\":\"phase-1\"") !=
        std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find("\"boss_friendly_fire_policy\":\"disabled\"") !=
        std::string::npos
    );
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_min_players\":4") != std::string::npos);
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_max_players\":8") != std::string::npos);
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_registered_player_count\":4") != std::string::npos);
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_layout_player_count\":4") != std::string::npos);
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_start_ready\":1") != std::string::npos);
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_ready_player_count\":4") != std::string::npos);
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_all_registered_connected\":1") != std::string::npos);
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_all_registered_ready\":1") != std::string::npos);
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_ready_to_start\":1") != std::string::npos);
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find("\"boss_lifecycle_state\":\"start_ready\"") !=
        std::string::npos
    );
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"connected_player_count\":4") != std::string::npos);
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"disconnected_player_count\":0") != std::string::npos);
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_max_hp\":1000") != std::string::npos);
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_current_hp\":980") != std::string::npos);
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_damage_total\":20") != std::string::npos);
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_damage_p1\":10") != std::string::npos);
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find("\"boss_player_p1_spawn_slot\":\"north\"") !=
        std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find("\"boss_player_p1_fire_target\":\"boss_center\"") !=
        std::string::npos
    );
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_damage_p2\":10") != std::string::npos);
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find("\"boss_player_p2_spawn_slot\":\"east\"") !=
        std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find("\"boss_player_p2_fire_target\":\"boss_center\"") !=
        std::string::npos
    );
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_damage_p3\":0") != std::string::npos);
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_damage_p4\":0") != std::string::npos);
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_defeated\":0") != std::string::npos);
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_defeated_tick\":0") != std::string::npos);
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_clear_status\":\"failed\"") != std::string::npos);
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_result_disposition\":\"instance_failed\"") != std::string::npos);
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find("\"boss_instance_surviving_player_count\":4") !=
        std::string::npos
    );
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"boss_instance_clear_credit\":0") != std::string::npos);
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find("\"boss_instance_result_state\":\"failed\"") !=
        std::string::npos
    );
    CHECK_TRUE(built.signed_result.result.mode_result_json.find("\"transfer_card_count\":2") != std::string::npos);
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find(
            "\"transfer_card_edges_material\":\"instance-result-card-001:p1>p2:p1:1:1:1;"
            "instance-result-card-002:p2>p3:p2:1:1:1;\""
        ) != std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find(
            "\"last_transfer_card_instance_id\":\"instance-result-card-002\""
        ) != std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find("\"last_transfer_from_player_id\":\"p2\"") !=
        std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find("\"last_transfer_to_player_id\":\"p3\"") !=
        std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find("\"last_transfer_authority_owner_player_id\":\"p2\"") !=
        std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find("\"last_transfer_authority_mode_allowed\":1") !=
        std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find("\"last_transfer_authority_cost_paid\":1") !=
        std::string::npos
    );
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find("\"last_transfer_authority_cooldown_ready\":1") !=
        std::string::npos
    );

    auto wrong_scope = built.signed_result;
    wrong_scope.result.mode_result_json = ReplaceJsonStringField(
        wrong_scope.result.mode_result_json,
        "boss_scope",
        "world_persistent"
    );
    const auto wrong_scope_result = server.SubmitBattleResult(wrong_scope);
    CHECK_TRUE(!wrong_scope_result.ok);
    CHECK_EQ(wrong_scope_result.reason, std::string("boss_scope_mismatch"));

    auto wrong_boss_instance = built.signed_result;
    wrong_boss_instance.result.mode_result_json = ReplaceJsonStringField(
        wrong_boss_instance.result.mode_result_json,
        "boss_instance_id",
        "client-authored-boss-instance"
    );
    const auto wrong_boss_instance_result = server.SubmitBattleResult(wrong_boss_instance);
    CHECK_TRUE(!wrong_boss_instance_result.ok);
    CHECK_EQ(wrong_boss_instance_result.reason, std::string("boss_instance_id_mismatch"));

    auto wrong_boss_season = built.signed_result;
    wrong_boss_season.result.mode_result_json = ReplaceJsonStringField(
        wrong_boss_season.result.mode_result_json,
        "boss_season_id",
        "season-client"
    );
    const auto wrong_boss_season_result = server.SubmitBattleResult(wrong_boss_season);
    CHECK_TRUE(!wrong_boss_season_result.ok);
    CHECK_EQ(wrong_boss_season_result.reason, std::string("boss_season_id_mismatch"));

    auto wrong_boss_phase = built.signed_result;
    wrong_boss_phase.result.mode_result_json = ReplaceJsonStringField(
        wrong_boss_phase.result.mode_result_json,
        "boss_phase_id",
        "phase-client"
    );
    const auto wrong_boss_phase_result = server.SubmitBattleResult(wrong_boss_phase);
    CHECK_TRUE(!wrong_boss_phase_result.ok);
    CHECK_EQ(wrong_boss_phase_result.reason, std::string("boss_phase_id_mismatch"));

    auto wrong_friendly_fire = built.signed_result;
    wrong_friendly_fire.result.mode_result_json = ReplaceJsonStringField(
        wrong_friendly_fire.result.mode_result_json,
        "boss_friendly_fire_policy",
        "all_friendly_fire"
    );
    const auto wrong_friendly_fire_result = server.SubmitBattleResult(wrong_friendly_fire);
    CHECK_TRUE(!wrong_friendly_fire_result.ok);
    CHECK_EQ(wrong_friendly_fire_result.reason, std::string("boss_friendly_fire_policy_mismatch"));

    auto wrong_min_players = built.signed_result;
    wrong_min_players.result.mode_result_json = ReplaceFirst(
        wrong_min_players.result.mode_result_json,
        "\"boss_min_players\":4",
        "\"boss_min_players\":1"
    );
    const auto wrong_min_players_result = server.SubmitBattleResult(wrong_min_players);
    CHECK_TRUE(!wrong_min_players_result.ok);
    CHECK_EQ(wrong_min_players_result.reason, std::string("boss_min_players_mismatch"));

    auto wrong_registered_player_count = built.signed_result;
    wrong_registered_player_count.result.mode_result_json = ReplaceFirst(
        wrong_registered_player_count.result.mode_result_json,
        "\"boss_registered_player_count\":4",
        "\"boss_registered_player_count\":3"
    );
    const auto wrong_registered_player_count_result = server.SubmitBattleResult(wrong_registered_player_count);
    CHECK_TRUE(!wrong_registered_player_count_result.ok);
    CHECK_EQ(
        wrong_registered_player_count_result.reason,
        std::string("boss_registered_player_count_mismatch")
    );

    auto wrong_layout_player_count = built.signed_result;
    wrong_layout_player_count.result.mode_result_json = ReplaceFirst(
        wrong_layout_player_count.result.mode_result_json,
        "\"boss_layout_player_count\":4",
        "\"boss_layout_player_count\":8"
    );
    const auto wrong_layout_player_count_result = server.SubmitBattleResult(wrong_layout_player_count);
    CHECK_TRUE(!wrong_layout_player_count_result.ok);
    CHECK_EQ(
        wrong_layout_player_count_result.reason,
        std::string("boss_layout_player_count_mismatch")
    );

    auto wrong_start_ready = built.signed_result;
    wrong_start_ready.result.mode_result_json = ReplaceFirst(
        wrong_start_ready.result.mode_result_json,
        "\"boss_start_ready\":1",
        "\"boss_start_ready\":0"
    );
    const auto wrong_start_ready_result = server.SubmitBattleResult(wrong_start_ready);
    CHECK_TRUE(!wrong_start_ready_result.ok);
    CHECK_EQ(wrong_start_ready_result.reason, std::string("boss_start_ready_mismatch"));

    auto wrong_ready_count = built.signed_result;
    wrong_ready_count.result.mode_result_json = ReplaceFirst(
        wrong_ready_count.result.mode_result_json,
        "\"boss_ready_player_count\":4",
        "\"boss_ready_player_count\":3"
    );
    const auto wrong_ready_count_result = server.SubmitBattleResult(wrong_ready_count);
    CHECK_TRUE(!wrong_ready_count_result.ok);
    CHECK_EQ(wrong_ready_count_result.reason, std::string("boss_ready_player_count_mismatch"));

    auto wrong_all_registered_connected = built.signed_result;
    wrong_all_registered_connected.result.mode_result_json = ReplaceFirst(
        wrong_all_registered_connected.result.mode_result_json,
        "\"boss_all_registered_connected\":1",
        "\"boss_all_registered_connected\":0"
    );
    const auto wrong_all_registered_connected_result = server.SubmitBattleResult(wrong_all_registered_connected);
    CHECK_TRUE(!wrong_all_registered_connected_result.ok);
    CHECK_EQ(
        wrong_all_registered_connected_result.reason,
        std::string("boss_all_registered_connected_mismatch")
    );

    auto wrong_all_registered_ready = built.signed_result;
    wrong_all_registered_ready.result.mode_result_json = ReplaceFirst(
        wrong_all_registered_ready.result.mode_result_json,
        "\"boss_all_registered_ready\":1",
        "\"boss_all_registered_ready\":0"
    );
    const auto wrong_all_registered_ready_result = server.SubmitBattleResult(wrong_all_registered_ready);
    CHECK_TRUE(!wrong_all_registered_ready_result.ok);
    CHECK_EQ(
        wrong_all_registered_ready_result.reason,
        std::string("boss_all_registered_ready_mismatch")
    );

    auto wrong_ready_to_start = built.signed_result;
    wrong_ready_to_start.result.mode_result_json = ReplaceFirst(
        wrong_ready_to_start.result.mode_result_json,
        "\"boss_ready_to_start\":1",
        "\"boss_ready_to_start\":0"
    );
    const auto wrong_ready_to_start_result = server.SubmitBattleResult(wrong_ready_to_start);
    CHECK_TRUE(!wrong_ready_to_start_result.ok);
    CHECK_EQ(wrong_ready_to_start_result.reason, std::string("boss_ready_to_start_mismatch"));

    auto wrong_lifecycle_state = built.signed_result;
    wrong_lifecycle_state.result.mode_result_json = ReplaceJsonStringField(
        wrong_lifecycle_state.result.mode_result_json,
        "boss_lifecycle_state",
        "waiting_for_ready"
    );
    const auto wrong_lifecycle_state_result = server.SubmitBattleResult(wrong_lifecycle_state);
    CHECK_TRUE(!wrong_lifecycle_state_result.ok);
    CHECK_EQ(wrong_lifecycle_state_result.reason, std::string("boss_lifecycle_state_mismatch"));

    auto wrong_connected_count = built.signed_result;
    wrong_connected_count.result.mode_result_json = ReplaceFirst(
        wrong_connected_count.result.mode_result_json,
        "\"connected_player_count\":4",
        "\"connected_player_count\":3"
    );
    const auto wrong_connected_count_result = server.SubmitBattleResult(wrong_connected_count);
    CHECK_TRUE(!wrong_connected_count_result.ok);
    CHECK_EQ(wrong_connected_count_result.reason, std::string("connected_player_count_mismatch"));

    auto wrong_disconnected_count = built.signed_result;
    wrong_disconnected_count.result.mode_result_json = ReplaceFirst(
        wrong_disconnected_count.result.mode_result_json,
        "\"disconnected_player_count\":0",
        "\"disconnected_player_count\":1"
    );
    const auto wrong_disconnected_count_result = server.SubmitBattleResult(wrong_disconnected_count);
    CHECK_TRUE(!wrong_disconnected_count_result.ok);
    CHECK_EQ(wrong_disconnected_count_result.reason, std::string("disconnected_player_count_mismatch"));

    auto wrong_max_hp = built.signed_result;
    wrong_max_hp.result.mode_result_json = ReplaceFirst(
        wrong_max_hp.result.mode_result_json,
        "\"boss_max_hp\":1000",
        "\"boss_max_hp\":1"
    );
    const auto wrong_max_hp_result = server.SubmitBattleResult(wrong_max_hp);
    CHECK_TRUE(!wrong_max_hp_result.ok);
    CHECK_EQ(wrong_max_hp_result.reason, std::string("boss_max_hp_mismatch"));

    auto wrong_current_hp = built.signed_result;
    wrong_current_hp.result.mode_result_json = ReplaceFirst(
        wrong_current_hp.result.mode_result_json,
        "\"boss_current_hp\":980",
        "\"boss_current_hp\":0"
    );
    const auto wrong_current_hp_result = server.SubmitBattleResult(wrong_current_hp);
    CHECK_TRUE(!wrong_current_hp_result.ok);
    CHECK_EQ(wrong_current_hp_result.reason, std::string("boss_current_hp_mismatch"));

    auto wrong_damage_total = built.signed_result;
    wrong_damage_total.result.mode_result_json = ReplaceFirst(
        wrong_damage_total.result.mode_result_json,
        "\"boss_damage_total\":20",
        "\"boss_damage_total\":999"
    );
    const auto wrong_damage_total_result = server.SubmitBattleResult(wrong_damage_total);
    CHECK_TRUE(!wrong_damage_total_result.ok);
    CHECK_EQ(wrong_damage_total_result.reason, std::string("boss_damage_total_mismatch"));

    auto wrong_player_damage = built.signed_result;
    wrong_player_damage.result.mode_result_json = ReplaceFirst(
        wrong_player_damage.result.mode_result_json,
        "\"boss_damage_p1\":10",
        "\"boss_damage_p1\":0"
    );
    const auto wrong_player_damage_result = server.SubmitBattleResult(wrong_player_damage);
    CHECK_TRUE(!wrong_player_damage_result.ok);
    CHECK_EQ(wrong_player_damage_result.reason, std::string("boss_player_damage_mismatch"));

    auto wrong_spawn_slot = built.signed_result;
    wrong_spawn_slot.result.mode_result_json = ReplaceJsonStringField(
        wrong_spawn_slot.result.mode_result_json,
        "boss_player_p1_spawn_slot",
        "south"
    );
    const auto wrong_spawn_slot_result = server.SubmitBattleResult(wrong_spawn_slot);
    CHECK_TRUE(!wrong_spawn_slot_result.ok);
    CHECK_EQ(wrong_spawn_slot_result.reason, std::string("boss_player_spawn_slot_mismatch"));

    auto wrong_fire_target = built.signed_result;
    wrong_fire_target.result.mode_result_json = ReplaceJsonStringField(
        wrong_fire_target.result.mode_result_json,
        "boss_player_p2_fire_target",
        "client_cursor"
    );
    const auto wrong_fire_target_result = server.SubmitBattleResult(wrong_fire_target);
    CHECK_TRUE(!wrong_fire_target_result.ok);
    CHECK_EQ(wrong_fire_target_result.reason, std::string("boss_player_fire_target_mismatch"));

    auto wrong_defeated = built.signed_result;
    wrong_defeated.result.mode_result_json = ReplaceFirst(
        wrong_defeated.result.mode_result_json,
        "\"boss_defeated\":0",
        "\"boss_defeated\":1"
    );
    const auto wrong_defeated_result = server.SubmitBattleResult(wrong_defeated);
    CHECK_TRUE(!wrong_defeated_result.ok);
    CHECK_EQ(wrong_defeated_result.reason, std::string("boss_defeated_mismatch"));

    auto wrong_defeated_tick = built.signed_result;
    wrong_defeated_tick.result.mode_result_json = ReplaceFirst(
        wrong_defeated_tick.result.mode_result_json,
        "\"boss_defeated_tick\":0",
        "\"boss_defeated_tick\":1"
    );
    const auto wrong_defeated_tick_result = server.SubmitBattleResult(wrong_defeated_tick);
    CHECK_TRUE(!wrong_defeated_tick_result.ok);
    CHECK_EQ(wrong_defeated_tick_result.reason, std::string("boss_defeated_tick_mismatch"));

    auto wrong_clear_status = built.signed_result;
    wrong_clear_status.result.mode_result_json = ReplaceJsonStringField(
        wrong_clear_status.result.mode_result_json,
        "boss_clear_status",
        "cleared"
    );
    const auto wrong_clear_status_result = server.SubmitBattleResult(wrong_clear_status);
    CHECK_TRUE(!wrong_clear_status_result.ok);
    CHECK_EQ(wrong_clear_status_result.reason, std::string("boss_clear_status_mismatch"));

    auto wrong_disposition = built.signed_result;
    wrong_disposition.result.mode_result_json = ReplaceJsonStringField(
        wrong_disposition.result.mode_result_json,
        "boss_result_disposition",
        "instance_cleared"
    );
    const auto wrong_disposition_result = server.SubmitBattleResult(wrong_disposition);
    CHECK_TRUE(!wrong_disposition_result.ok);
    CHECK_EQ(wrong_disposition_result.reason, std::string("boss_result_disposition_mismatch"));

    auto wrong_instance_survivors = built.signed_result;
    wrong_instance_survivors.result.mode_result_json = ReplaceFirst(
        wrong_instance_survivors.result.mode_result_json,
        "\"boss_instance_surviving_player_count\":4",
        "\"boss_instance_surviving_player_count\":0"
    );
    const auto wrong_instance_survivors_result = server.SubmitBattleResult(wrong_instance_survivors);
    CHECK_TRUE(!wrong_instance_survivors_result.ok);
    CHECK_EQ(
        wrong_instance_survivors_result.reason,
        std::string("boss_instance_surviving_player_count_mismatch")
    );

    auto wrong_instance_clear_credit = built.signed_result;
    wrong_instance_clear_credit.result.mode_result_json = ReplaceFirst(
        wrong_instance_clear_credit.result.mode_result_json,
        "\"boss_instance_clear_credit\":0",
        "\"boss_instance_clear_credit\":1"
    );
    const auto wrong_instance_clear_credit_result = server.SubmitBattleResult(wrong_instance_clear_credit);
    CHECK_TRUE(!wrong_instance_clear_credit_result.ok);
    CHECK_EQ(
        wrong_instance_clear_credit_result.reason,
        std::string("boss_instance_clear_credit_mismatch")
    );

    auto wrong_instance_result_state = built.signed_result;
    wrong_instance_result_state.result.mode_result_json = ReplaceJsonStringField(
        wrong_instance_result_state.result.mode_result_json,
        "boss_instance_result_state",
        "cleared"
    );
    const auto wrong_instance_result_state_result = server.SubmitBattleResult(wrong_instance_result_state);
    CHECK_TRUE(!wrong_instance_result_state_result.ok);
    CHECK_EQ(
        wrong_instance_result_state_result.reason,
        std::string("boss_instance_result_state_mismatch")
    );

    auto wrong_transfer_count = built.signed_result;
    wrong_transfer_count.result.mode_result_json = ReplaceFirst(
        wrong_transfer_count.result.mode_result_json,
        "\"transfer_card_count\":2",
        "\"transfer_card_count\":3"
    );
    const auto wrong_transfer_count_result = server.SubmitBattleResult(wrong_transfer_count);
    CHECK_TRUE(!wrong_transfer_count_result.ok);
    CHECK_EQ(wrong_transfer_count_result.reason, std::string("transfer_card_count_mismatch"));

    auto wrong_transfer_edges = built.signed_result;
    wrong_transfer_edges.result.mode_result_json = ReplaceJsonStringField(
        wrong_transfer_edges.result.mode_result_json,
        "transfer_card_edges_material",
        "instance-result-card-001:p1>p2:p1:1:1:1;instance-result-card-002:p2>p4:p2:1:1:1;"
    );
    const auto wrong_transfer_edges_result = server.SubmitBattleResult(wrong_transfer_edges);
    CHECK_TRUE(!wrong_transfer_edges_result.ok);
    CHECK_EQ(wrong_transfer_edges_result.reason, std::string("transfer_card_edges_mismatch"));

    auto wrong_transfer_instance = built.signed_result;
    wrong_transfer_instance.result.mode_result_json = ReplaceJsonStringField(
        wrong_transfer_instance.result.mode_result_json,
        "last_transfer_card_instance_id",
        "forged-card"
    );
    const auto wrong_transfer_instance_result = server.SubmitBattleResult(wrong_transfer_instance);
    CHECK_TRUE(!wrong_transfer_instance_result.ok);
    CHECK_EQ(wrong_transfer_instance_result.reason, std::string("transfer_card_instance_mismatch"));

    auto wrong_transfer_from = built.signed_result;
    wrong_transfer_from.result.mode_result_json = ReplaceJsonStringField(
        wrong_transfer_from.result.mode_result_json,
        "last_transfer_from_player_id",
        "p1"
    );
    const auto wrong_transfer_from_result = server.SubmitBattleResult(wrong_transfer_from);
    CHECK_TRUE(!wrong_transfer_from_result.ok);
    CHECK_EQ(wrong_transfer_from_result.reason, std::string("transfer_card_from_player_mismatch"));

    auto wrong_transfer_to = built.signed_result;
    wrong_transfer_to.result.mode_result_json = ReplaceJsonStringField(
        wrong_transfer_to.result.mode_result_json,
        "last_transfer_to_player_id",
        "p4"
    );
    const auto wrong_transfer_to_result = server.SubmitBattleResult(wrong_transfer_to);
    CHECK_TRUE(!wrong_transfer_to_result.ok);
    CHECK_EQ(wrong_transfer_to_result.reason, std::string("transfer_card_to_player_mismatch"));

    auto wrong_transfer_owner = built.signed_result;
    wrong_transfer_owner.result.mode_result_json = ReplaceJsonStringField(
        wrong_transfer_owner.result.mode_result_json,
        "last_transfer_authority_owner_player_id",
        "p1"
    );
    const auto wrong_transfer_owner_result = server.SubmitBattleResult(wrong_transfer_owner);
    CHECK_TRUE(!wrong_transfer_owner_result.ok);
    CHECK_EQ(wrong_transfer_owner_result.reason, std::string("transfer_card_authority_owner_mismatch"));

    auto wrong_transfer_mode_allowed = built.signed_result;
    wrong_transfer_mode_allowed.result.mode_result_json = ReplaceFirst(
        wrong_transfer_mode_allowed.result.mode_result_json,
        "\"last_transfer_authority_mode_allowed\":1",
        "\"last_transfer_authority_mode_allowed\":0"
    );
    const auto wrong_transfer_mode_allowed_result = server.SubmitBattleResult(wrong_transfer_mode_allowed);
    CHECK_TRUE(!wrong_transfer_mode_allowed_result.ok);
    CHECK_EQ(
        wrong_transfer_mode_allowed_result.reason,
        std::string("transfer_card_authority_mode_allowed_mismatch")
    );

    auto wrong_transfer_cost_paid = built.signed_result;
    wrong_transfer_cost_paid.result.mode_result_json = ReplaceFirst(
        wrong_transfer_cost_paid.result.mode_result_json,
        "\"last_transfer_authority_cost_paid\":1",
        "\"last_transfer_authority_cost_paid\":0"
    );
    const auto wrong_transfer_cost_paid_result = server.SubmitBattleResult(wrong_transfer_cost_paid);
    CHECK_TRUE(!wrong_transfer_cost_paid_result.ok);
    CHECK_EQ(
        wrong_transfer_cost_paid_result.reason,
        std::string("transfer_card_authority_cost_paid_mismatch")
    );

    auto wrong_transfer_cooldown = built.signed_result;
    wrong_transfer_cooldown.result.mode_result_json = ReplaceFirst(
        wrong_transfer_cooldown.result.mode_result_json,
        "\"last_transfer_authority_cooldown_ready\":1",
        "\"last_transfer_authority_cooldown_ready\":0"
    );
    const auto wrong_transfer_cooldown_result = server.SubmitBattleResult(wrong_transfer_cooldown);
    CHECK_TRUE(!wrong_transfer_cooldown_result.ok);
    CHECK_EQ(
        wrong_transfer_cooldown_result.reason,
        std::string("transfer_card_authority_cooldown_mismatch")
    );

    const auto accepted = server.SubmitBattleResult(built.signed_result);
    CHECK_TRUE(accepted.ok);
    CHECK_TRUE(!accepted.duplicate);
    return true;
}

bool TestTransferCardAuditIdsRejectEscapedStrings() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489645000;
    phk::battle::BattleServer server(config);
    CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
        "ticket-instance-escape-1",
        "user-instance-escape-1",
        "p1",
        "instance_boss",
        "00112233445566778899ea01"
    )).ok);
    CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
        "ticket-instance-escape-2",
        "user-instance-escape-2",
        "p2",
        "instance_boss",
        "00112233445566778899ea02"
    )).ok);
    CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
        "ticket-instance-escape-3",
        "user-instance-escape-3",
        "p3",
        "instance_boss",
        "00112233445566778899ea03"
    )).ok);
    CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
        "ticket-instance-escape-4",
        "user-instance-escape-4",
        "p4",
        "instance_boss",
        "00112233445566778899ea04"
    )).ok);

    phk::battle::TransferableCardState transfer_card;
    transfer_card.card_instance_id = "instance\\escaped-card-001";
    transfer_card.owner_player_id = "p1";
    CHECK_TRUE(!server.ConfigureTransferableCard("match-001", transfer_card));

    auto transfer = MakeModeAction(1);
    transfer.match_id = "match-001";
    transfer.player_id = "p1";
    transfer.tick = 1;
    transfer.seq = 1;
    transfer.action_id = "action-instance-escaped-transfer";
    transfer.action_type = "transfer_card";
    transfer.payload_json = R"({"target_player_id":"p2","card_instance_id":"instance\\escaped-card-001"})";
    const auto escaped_result = server.AcceptModeAction(transfer);
    CHECK_TRUE(!escaped_result.ok);
    CHECK_EQ(escaped_result.reason, std::string("transfer_card_instance_id_invalid"));
    return true;
}

bool TestBossModeResultRequiresStartableRoom() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489645000;
    phk::battle::BattleServer server(config);

    for (std::size_t index = 1; index <= 3; ++index) {
        CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
            "ticket-underfilled-boss-" + std::to_string(index),
            "user-underfilled-boss-" + std::to_string(index),
            "p" + std::to_string(index),
            "world_boss",
            "00112233445566778899fa0" + std::to_string(index)
        )).ok);
    }
    const auto underfilled_snapshot = server.MatchSnapshot("match-001");
    CHECK_EQ(underfilled_snapshot.players.size(), static_cast<std::size_t>(3));
    CHECK_EQ(underfilled_snapshot.mode_state.at("boss_start_ready"), std::string("0"));
    CHECK_EQ(underfilled_snapshot.mode_state.at("boss_ready_to_start"), std::string("0"));
    CHECK_EQ(underfilled_snapshot.mode_state.at("boss_lifecycle_state"), std::string("waiting_for_players"));

    const auto underfilled_result = server.BuildSignedBattleResult("match-001");
    CHECK_TRUE(!underfilled_result.ok);
    CHECK_EQ(underfilled_result.reason, std::string("boss_match_not_startable"));

    const auto underfilled_replay_record = server.BuildReplayRecord("match-001", "user-underfilled", "stage-boss");
    CHECK_TRUE(!underfilled_replay_record.ok);
    CHECK_EQ(underfilled_replay_record.reason, std::string("boss_match_not_startable"));

    phk::battle::SignedBattleResult forged_result;
    forged_result.result.match_id = "match-001";
    const auto forged_submit = server.SubmitBattleResult(forged_result);
    CHECK_TRUE(!forged_submit.ok);
    CHECK_EQ(forged_submit.reason, std::string("boss_match_not_startable"));

    CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
        "ticket-underfilled-boss-4",
        "user-underfilled-boss-4",
        "p4",
        "world_boss",
        "00112233445566778899fa04"
    )).ok);
    const auto startable_snapshot = server.MatchSnapshot("match-001");
    CHECK_EQ(startable_snapshot.players.size(), static_cast<std::size_t>(4));
    CHECK_EQ(startable_snapshot.mode_state.at("boss_start_ready"), std::string("1"));
    CHECK_EQ(startable_snapshot.mode_state.at("boss_ready_to_start"), std::string("0"));
    CHECK_EQ(startable_snapshot.mode_state.at("boss_lifecycle_state"), std::string("waiting_for_ready"));
    const auto not_ready_result = server.BuildSignedBattleResult("match-001");
    CHECK_TRUE(!not_ready_result.ok);
    CHECK_EQ(not_ready_result.reason, std::string("boss_match_not_startable"));

    for (std::size_t index = 1; index <= 4; ++index) {
        auto ready = MakeModeAction(index);
        ready.match_id = "match-001";
        ready.player_id = "p" + std::to_string(index);
        ready.tick = 1;
        ready.seq = 1;
        ready.action_id = "underfilled-boss-ready-" + std::to_string(index);
        ready.action_type = "ready";
        ready.payload_json = "{\"ready\":true}";
        CHECK_TRUE(server.AcceptModeAction(ready).ok);
    }
    const auto ready_snapshot = server.TickMatch("match-001");
    CHECK_EQ(ready_snapshot.mode_state.at("boss_ready_to_start"), std::string("1"));
    CHECK_EQ(ready_snapshot.mode_state.at("boss_lifecycle_state"), std::string("start_ready"));
    CHECK_TRUE(server.BuildSignedBattleResult("match-001").ok);
    return true;
}

bool TestBossRosterLocksAfterReadyToStart() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489646000;
    phk::battle::BattleServer server(config);

    for (std::size_t index = 1; index <= 4; ++index) {
        CHECK_TRUE(server.RegisterTicket(MakeModeTicket(
            "ticket-roster-lock-" + std::to_string(index),
            "user-roster-lock-" + std::to_string(index),
            "p" + std::to_string(index),
            "instance_boss",
            "00112233445566778899fb0" + std::to_string(index)
        )).ok);
        auto ready = MakeModeAction(index);
        ready.match_id = "match-001";
        ready.player_id = "p" + std::to_string(index);
        ready.tick = 1;
        ready.seq = 1;
        ready.action_id = "boss-roster-lock-ready-" + std::to_string(index);
        ready.action_type = "ready";
        ready.payload_json = "{\"ready\":true}";
        CHECK_TRUE(server.AcceptModeAction(ready).ok);
    }
    CHECK_EQ(server.TickMatch("match-001").mode_state.at("boss_ready_to_start"), std::string("1"));

    const auto late_join = server.RegisterTicket(MakeModeTicket(
        "ticket-roster-lock-late",
        "user-roster-lock-late",
        "p5",
        "instance_boss",
        "00112233445566778899fb05"
    ));
    CHECK_TRUE(!late_join.ok);
    CHECK_EQ(late_join.reason, std::string("boss_roster_locked"));
    CHECK_EQ(late_join.active_sessions_before, static_cast<std::size_t>(4));
    CHECK_EQ(late_join.active_sessions_after, static_cast<std::size_t>(4));
    CHECK_EQ(late_join.match_session_count_before, static_cast<std::size_t>(4));
    CHECK_EQ(late_join.match_session_count_after, static_cast<std::size_t>(4));

    CHECK_TRUE(server.SetPlayerConnected("match-001", "p4", false).ok);
    const auto disconnected_snapshot = server.MatchSnapshot("match-001");
    CHECK_EQ(disconnected_snapshot.mode_state.at("boss_ready_to_start"), std::string("0"));
    CHECK_EQ(disconnected_snapshot.mode_state.at("boss_lifecycle_state"), std::string("waiting_for_players"));
    const auto disconnected_late_join = server.RegisterTicket(MakeModeTicket(
        "ticket-roster-lock-disconnected-late",
        "user-roster-lock-disconnected-late",
        "p5",
        "instance_boss",
        "00112233445566778899fb06"
    ));
    CHECK_TRUE(!disconnected_late_join.ok);
    CHECK_EQ(disconnected_late_join.reason, std::string("boss_roster_locked"));
    CHECK_EQ(disconnected_late_join.active_sessions_after, static_cast<std::size_t>(4));
    CHECK_TRUE(server.SetPlayerConnected("match-001", "p4", true).ok);
    auto p4_ready_again = MakeModeAction(5);
    p4_ready_again.match_id = "match-001";
    p4_ready_again.player_id = "p4";
    p4_ready_again.tick = 2;
    p4_ready_again.seq = 2;
    p4_ready_again.action_id = "boss-roster-lock-ready-again";
    p4_ready_again.action_type = "ready";
    p4_ready_again.payload_json = "{\"ready\":true}";
    CHECK_TRUE(server.AcceptModeAction(p4_ready_again).ok);
    CHECK_EQ(server.TickMatch("match-001").mode_state.at("boss_ready_to_start"), std::string("1"));

    const auto snapshot = server.MatchSnapshot("match-001");
    CHECK_EQ(snapshot.players.size(), static_cast<std::size_t>(4));
    CHECK_EQ(snapshot.mode_state.at("boss_registered_player_count"), std::string("4"));
    CHECK_EQ(snapshot.mode_state.at("boss_ready_player_count"), std::string("4"));
    CHECK_EQ(snapshot.mode_state.at("boss_ready_to_start"), std::string("1"));
    CHECK_TRUE(server.BuildSignedBattleResult("match-001").ok);
    return true;
}

bool TestSettledMatchRetirementLifecycle() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489650000;
    phk::battle::BattleServer server(config);
    CHECK_TRUE(server.RegisterTicket(MakeTicket()).ok);
    CHECK_TRUE(server.RegisterTicket(MakeTicketForBob()).ok);
    CHECK_EQ(server.ActiveSessionCount(), static_cast<std::size_t>(2));
    CHECK_EQ(server.ActiveMatchCount(), static_cast<std::size_t>(1));

    const auto premature_retire = server.RetireMatch("match-001");
    CHECK_TRUE(!premature_retire.ok);
    CHECK_EQ(premature_retire.reason, std::string("match_not_settled"));
    CHECK_EQ(premature_retire.active_sessions_before, static_cast<std::size_t>(2));
    CHECK_EQ(premature_retire.active_matches_before, static_cast<std::size_t>(1));
    CHECK_EQ(premature_retire.active_sessions_after, static_cast<std::size_t>(2));
    CHECK_EQ(premature_retire.active_matches_after, static_cast<std::size_t>(1));
    CHECK_EQ(server.ActiveSessionCount(), static_cast<std::size_t>(2));
    CHECK_EQ(server.ActiveMatchCount(), static_cast<std::size_t>(1));

    CHECK_TRUE(server.AcceptInput(MakeInput("p1", 1, 1, 1u << 3)).ok);
    CHECK_TRUE(server.AcceptInput(MakeInput("p2", 1, 1, 1u << 2)).ok);
    CHECK_EQ(server.TickMatch("match-001").snapshot_tick, static_cast<std::uint64_t>(1));
    const auto built_result = server.BuildSignedBattleResult("match-001");
    CHECK_TRUE(built_result.ok);
    const auto submitted = server.SubmitBattleResult(built_result.signed_result);
    CHECK_TRUE(submitted.ok);
    CHECK_TRUE(!submitted.duplicate);
    const auto settled_summary = server.MatchReplaySummary("match-001");

    phk::battle::BattleHandshakeHello handshake_after_settle;
    handshake_after_settle.battle_ticket = MakeTicket();
    FillDistinctHandshakeBytes(handshake_after_settle);
    handshake_after_settle.supported_aead = {"CHACHA20_POLY1305"};
    const auto handshake_after_settle_result = server.AcceptHandshake(handshake_after_settle);
    CHECK_TRUE(!handshake_after_settle_result.ok);
    CHECK_EQ(handshake_after_settle_result.reason, std::string("match_settled"));

    const auto input_after_settle = server.AcceptInput(MakeInput("p1", 2, 2, 1u << 3));
    CHECK_TRUE(!input_after_settle.ok);
    CHECK_EQ(input_after_settle.reason, std::string("match_settled"));
    auto ready_after_settle = MakeModeAction(2);
    ready_after_settle.tick = 2;
    ready_after_settle.seq = 2;
    ready_after_settle.action_id = "ready-after-settle";
    ready_after_settle.action_type = "ready";
    ready_after_settle.payload_json = "{\"ready\":true}";
    const auto mode_action_after_settle = server.AcceptModeAction(ready_after_settle);
    CHECK_TRUE(!mode_action_after_settle.ok);
    CHECK_EQ(mode_action_after_settle.reason, std::string("match_settled"));
    const auto disconnect_after_settle = server.SetPlayerConnected("match-001", "p1", false);
    CHECK_TRUE(!disconnect_after_settle.ok);
    CHECK_EQ(disconnect_after_settle.reason, std::string("match_settled"));
    phk::battle::TransferableCardState card_after_settle;
    card_after_settle.card_instance_id = "card-after-settle";
    card_after_settle.owner_player_id = "p1";
    CHECK_TRUE(!server.ConfigureTransferableCard("match-001", card_after_settle));
    phk::battle::BattleEncryptedPacket encrypted_after_settle;
    encrypted_after_settle.header.match_id = "match-001";
    encrypted_after_settle.header.player_id = "p1";
    encrypted_after_settle.header.tick = 2;
    encrypted_after_settle.header.seq = 2;
    encrypted_after_settle.header.payload_type = phk::battle::BattlePayloadType::Input;
    FillEncryptedHeaderShape(encrypted_after_settle.header);
    encrypted_after_settle.ciphertext = {1, 2, 3};
    encrypted_after_settle.auth_tag = std::vector<std::uint8_t>(16, 7);
    const auto encrypted_dispatch_after_settle = server.DispatchEncrypted(encrypted_after_settle);
    CHECK_TRUE(!encrypted_dispatch_after_settle.ok);
    CHECK_EQ(encrypted_dispatch_after_settle.reason, std::string("match_settled"));

    phk::battle::BattlePacketHeader decoded_input_header;
    decoded_input_header.match_id = "match-001";
    decoded_input_header.player_id = "p1";
    decoded_input_header.tick = 2;
    decoded_input_header.seq = 2;
    decoded_input_header.payload_type = phk::battle::BattlePayloadType::Input;
    decoded_input_header.key_id = "client-to-server-after-settle";
    const auto decoded_input_after_settle = server.AcceptDecodedInput(
        decoded_input_header,
        MakeInput("p1", 2, 2, 1u << 3)
    );
    CHECK_TRUE(!decoded_input_after_settle.ok);
    CHECK_EQ(decoded_input_after_settle.reason, std::string("match_settled"));

    phk::battle::BattlePacketHeader decoded_action_header;
    decoded_action_header.match_id = "match-001";
    decoded_action_header.player_id = "p1";
    decoded_action_header.tick = 2;
    decoded_action_header.seq = 2;
    decoded_action_header.payload_type = phk::battle::BattlePayloadType::ModeAction;
    decoded_action_header.key_id = "client-to-server-after-settle";
    const auto decoded_action_after_settle = server.AcceptDecodedModeAction(
        decoded_action_header,
        ready_after_settle
    );
    CHECK_TRUE(!decoded_action_after_settle.ok);
    CHECK_EQ(decoded_action_after_settle.reason, std::string("match_settled"));

    phk::battle::BattlePacketHeader decoded_reconnect_header;
    decoded_reconnect_header.match_id = "match-001";
    decoded_reconnect_header.player_id = "p1";
    decoded_reconnect_header.tick = 2;
    decoded_reconnect_header.seq = 2;
    decoded_reconnect_header.ack = settled_summary.event_count;
    decoded_reconnect_header.payload_type = phk::battle::BattlePayloadType::Reconnect;
    decoded_reconnect_header.key_id = "client-to-server-after-settle";
    auto reconnect_after_settle = ready_after_settle;
    reconnect_after_settle.action_id = "reconnect-after-settle";
    reconnect_after_settle.action_type = "reconnect";
    reconnect_after_settle.payload_json =
        "{\"last_seen_event_cursor\":" + std::to_string(settled_summary.event_count) + "}";
    const auto decoded_reconnect_after_settle = server.AcceptDecodedReconnectModeAction(
        decoded_reconnect_header,
        reconnect_after_settle
    );
    CHECK_TRUE(!decoded_reconnect_after_settle.ok);
    CHECK_EQ(decoded_reconnect_after_settle.reason, std::string("match_settled"));
    phk::battle::BattlePacketHeader plaintext_after_settle;
    plaintext_after_settle.match_id = "match-001";
    plaintext_after_settle.player_id = "p1";
    plaintext_after_settle.tick = 2;
    plaintext_after_settle.seq = 2;
    plaintext_after_settle.payload_type = phk::battle::BattlePayloadType::Snapshot;
    const auto plaintext_dispatch_after_settle = server.Dispatch(plaintext_after_settle, {'s'});
    CHECK_TRUE(!plaintext_dispatch_after_settle.ok);
    CHECK_EQ(plaintext_dispatch_after_settle.reason, std::string("match_settled"));

    const auto tick_after_settle = server.TickMatch("match-001");
    CHECK_EQ(tick_after_settle.snapshot_kind, std::string("match_settled"));
    CHECK_EQ(tick_after_settle.snapshot_tick, settled_summary.final_tick);
    const auto summary_after_settle_mutations = server.MatchReplaySummary("match-001");
    CHECK_EQ(summary_after_settle_mutations.final_tick, settled_summary.final_tick);
    CHECK_EQ(summary_after_settle_mutations.input_count, settled_summary.input_count);
    CHECK_EQ(summary_after_settle_mutations.event_count, settled_summary.event_count);
    CHECK_EQ(summary_after_settle_mutations.final_state_hash, settled_summary.final_state_hash);

    const auto retired = server.RetireMatch("match-001");
    CHECK_TRUE(retired.ok);
    CHECK_EQ(retired.reason, std::string("ok"));
    CHECK_EQ(retired.match_id, std::string("match-001"));
    CHECK_EQ(retired.result_hash, built_result.signed_result.result.result_hash);
    CHECK_EQ(retired.input_stream_hash, settled_summary.input_stream_hash);
    CHECK_EQ(retired.event_stream_hash, settled_summary.event_stream_hash);
    CHECK_EQ(retired.final_state_hash, settled_summary.final_state_hash);
    CHECK_EQ(retired.final_tick, settled_summary.final_tick);
    CHECK_EQ(retired.input_count, settled_summary.input_count);
    CHECK_EQ(retired.event_count, settled_summary.event_count);
    CHECK_EQ(retired.active_sessions_before, static_cast<std::size_t>(2));
    CHECK_EQ(retired.active_matches_before, static_cast<std::size_t>(1));
    CHECK_EQ(retired.pending_boss_configs_before, static_cast<std::size_t>(0));
    CHECK_EQ(retired.removed_sessions, static_cast<std::size_t>(2));
    CHECK_EQ(retired.active_sessions_after, static_cast<std::size_t>(0));
    CHECK_EQ(retired.active_matches_after, static_cast<std::size_t>(0));
    CHECK_EQ(retired.pending_boss_configs_after, static_cast<std::size_t>(0));
    CHECK_TRUE(!retired.already_retired);
    CHECK_EQ(server.ActiveSessionCount(), static_cast<std::size_t>(0));
    CHECK_EQ(server.ActiveMatchCount(), static_cast<std::size_t>(0));

    const auto snapshot_after_retire = server.MatchSnapshot("match-001");
    CHECK_EQ(snapshot_after_retire.snapshot_kind, std::string("match_unknown"));
    const auto input_after_retire = server.AcceptInput(MakeInput("p1", 2, 2, 1u << 3));
    CHECK_TRUE(!input_after_retire.ok);
    CHECK_EQ(input_after_retire.reason, std::string("match_unknown"));
    const auto duplicate_result_after_retire = server.SubmitBattleResult(built_result.signed_result);
    CHECK_TRUE(!duplicate_result_after_retire.ok);
    CHECK_EQ(duplicate_result_after_retire.reason, std::string("match_unknown"));
    const auto handshake_after_retire_result = server.AcceptHandshake(handshake_after_settle);
    CHECK_TRUE(!handshake_after_retire_result.ok);
    CHECK_EQ(handshake_after_retire_result.reason, std::string("match_retired"));
    const auto plaintext_dispatch_after_retire = server.Dispatch(plaintext_after_settle, {'s'});
    CHECK_TRUE(!plaintext_dispatch_after_retire.ok);
    CHECK_EQ(plaintext_dispatch_after_retire.reason, std::string("match_retired"));
    const auto replay_ticket_after_retire = server.RegisterTicket(MakeTicket());
    CHECK_TRUE(!replay_ticket_after_retire.ok);
    CHECK_EQ(replay_ticket_after_retire.reason, std::string("match_retired"));
    CHECK_TRUE(!replay_ticket_after_retire.created_match);
    CHECK_EQ(replay_ticket_after_retire.active_sessions_before, static_cast<std::size_t>(0));
    CHECK_EQ(replay_ticket_after_retire.active_matches_before, static_cast<std::size_t>(0));
    CHECK_EQ(replay_ticket_after_retire.match_session_count_before, static_cast<std::size_t>(0));
    CHECK_EQ(replay_ticket_after_retire.active_sessions_after, static_cast<std::size_t>(0));
    CHECK_EQ(replay_ticket_after_retire.active_matches_after, static_cast<std::size_t>(0));
    CHECK_EQ(replay_ticket_after_retire.match_session_count_after, static_cast<std::size_t>(0));

    const auto retired_again = server.RetireMatch("match-001");
    CHECK_TRUE(retired_again.ok);
    CHECK_TRUE(retired_again.already_retired);
    CHECK_EQ(retired_again.active_sessions_before, static_cast<std::size_t>(0));
    CHECK_EQ(retired_again.active_matches_before, static_cast<std::size_t>(0));
    CHECK_EQ(retired_again.pending_boss_configs_before, static_cast<std::size_t>(0));
    CHECK_EQ(retired_again.removed_sessions, static_cast<std::size_t>(0));
    CHECK_EQ(retired_again.active_sessions_after, static_cast<std::size_t>(0));
    CHECK_EQ(retired_again.active_matches_after, static_cast<std::size_t>(0));
    CHECK_EQ(retired_again.pending_boss_configs_after, static_cast<std::size_t>(0));
    CHECK_EQ(retired_again.result_hash, built_result.signed_result.result.result_hash);

    const auto missing = server.RetireMatch("missing-match");
    CHECK_TRUE(!missing.ok);
    CHECK_EQ(missing.reason, std::string("match_not_settled"));
    CHECK_EQ(missing.active_sessions_before, static_cast<std::size_t>(0));
    CHECK_EQ(missing.active_matches_before, static_cast<std::size_t>(0));
    CHECK_EQ(missing.pending_boss_configs_before, static_cast<std::size_t>(0));
    CHECK_EQ(missing.active_sessions_after, static_cast<std::size_t>(0));
    CHECK_EQ(missing.active_matches_after, static_cast<std::size_t>(0));
    CHECK_EQ(missing.pending_boss_configs_after, static_cast<std::size_t>(0));
    return true;
}

bool TestUnsettledMatchCancellationLifecycle() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489650000;
    phk::battle::BattleServer server(config);

    phk::battle::BossMatchConfig boss_config;
    boss_config.match_id = "match-boss-cancel";
    boss_config.mode_id = "world_boss";
    boss_config.boss_instance_id = "world-boss-cancel-001";
    CHECK_TRUE(server.ConfigureBossMatch(boss_config).ok);
    const auto cancelled_pending = server.CancelMatch("match-boss-cancel");
    CHECK_TRUE(cancelled_pending.ok);
    CHECK_EQ(cancelled_pending.reason, std::string("ok"));
    CHECK_TRUE(cancelled_pending.removed_pending_boss_config);
    CHECK_TRUE(!cancelled_pending.removed_match);
    CHECK_EQ(cancelled_pending.removed_sessions, static_cast<std::size_t>(0));
    CHECK_EQ(cancelled_pending.active_sessions_before, static_cast<std::size_t>(0));
    CHECK_EQ(cancelled_pending.active_matches_before, static_cast<std::size_t>(0));
    CHECK_EQ(cancelled_pending.pending_boss_configs_before, static_cast<std::size_t>(1));
    CHECK_EQ(cancelled_pending.active_sessions_after, static_cast<std::size_t>(0));
    CHECK_EQ(cancelled_pending.active_matches_after, static_cast<std::size_t>(0));
    CHECK_EQ(cancelled_pending.pending_boss_configs_after, static_cast<std::size_t>(0));

    const auto reconfigure_cancelled = server.ConfigureBossMatch(boss_config);
    CHECK_TRUE(!reconfigure_cancelled.ok);
    CHECK_EQ(reconfigure_cancelled.reason, std::string("match_cancelled"));
    auto cancelled_boss_ticket = MakeModeTicket(
        "ticket-cancelled-boss",
        "user-cancelled-boss",
        "p1",
        "world_boss",
        "00112233445566778899ac01"
    );
    cancelled_boss_ticket.ticket.match_id = "match-boss-cancel";
    const auto cancelled_boss_register = server.RegisterTicket(cancelled_boss_ticket);
    CHECK_TRUE(!cancelled_boss_register.ok);
    CHECK_EQ(cancelled_boss_register.reason, std::string("match_cancelled"));

    CHECK_TRUE(server.RegisterTicket(MakeTicket()).ok);
    CHECK_TRUE(server.RegisterTicket(MakeTicketForBob()).ok);
    CHECK_TRUE(server.AcceptInput(MakeInput("p1", 1, 1, 1u << 3)).ok);
    CHECK_TRUE(server.AcceptInput(MakeInput("p2", 1, 1, 1u << 2)).ok);
    CHECK_EQ(server.TickMatch("match-001").snapshot_tick, static_cast<std::uint64_t>(1));
    CHECK_EQ(server.ActiveSessionCount(), static_cast<std::size_t>(2));
    CHECK_EQ(server.ActiveMatchCount(), static_cast<std::size_t>(1));

    const auto cancelled = server.CancelMatch("match-001");
    CHECK_TRUE(cancelled.ok);
    CHECK_EQ(cancelled.reason, std::string("ok"));
    CHECK_TRUE(cancelled.removed_match);
    CHECK_TRUE(!cancelled.removed_pending_boss_config);
    CHECK_TRUE(!cancelled.already_cancelled);
    CHECK_EQ(cancelled.removed_sessions, static_cast<std::size_t>(2));
    CHECK_EQ(cancelled.active_sessions_before, static_cast<std::size_t>(2));
    CHECK_EQ(cancelled.active_matches_before, static_cast<std::size_t>(1));
    CHECK_EQ(cancelled.pending_boss_configs_before, static_cast<std::size_t>(0));
    CHECK_EQ(cancelled.active_sessions_after, static_cast<std::size_t>(0));
    CHECK_EQ(cancelled.active_matches_after, static_cast<std::size_t>(0));
    CHECK_EQ(cancelled.pending_boss_configs_after, static_cast<std::size_t>(0));
    CHECK_EQ(server.ActiveSessionCount(), static_cast<std::size_t>(0));
    CHECK_EQ(server.ActiveMatchCount(), static_cast<std::size_t>(0));

    const auto cancelled_again = server.CancelMatch("match-001");
    CHECK_TRUE(cancelled_again.ok);
    CHECK_TRUE(cancelled_again.already_cancelled);
    CHECK_TRUE(!cancelled_again.removed_match);
    CHECK_EQ(cancelled_again.removed_sessions, static_cast<std::size_t>(0));
    CHECK_EQ(cancelled_again.reason, std::string("ok"));

    const auto snapshot_after_cancel = server.MatchSnapshot("match-001");
    CHECK_EQ(snapshot_after_cancel.snapshot_kind, std::string("match_unknown"));
    const auto ticket_after_cancel = server.RegisterTicket(MakeTicket());
    CHECK_TRUE(!ticket_after_cancel.ok);
    CHECK_EQ(ticket_after_cancel.reason, std::string("match_cancelled"));

    phk::battle::BattleHandshakeHello handshake_after_cancel;
    handshake_after_cancel.battle_ticket = MakeTicket();
    FillDistinctHandshakeBytes(handshake_after_cancel);
    handshake_after_cancel.supported_aead = {"CHACHA20_POLY1305"};
    const auto handshake_result = server.AcceptHandshake(handshake_after_cancel);
    CHECK_TRUE(!handshake_result.ok);
    CHECK_EQ(handshake_result.reason, std::string("match_cancelled"));

    phk::battle::BattlePacketHeader plaintext_after_cancel;
    plaintext_after_cancel.match_id = "match-001";
    plaintext_after_cancel.player_id = "p1";
    plaintext_after_cancel.tick = 2;
    plaintext_after_cancel.seq = 2;
    plaintext_after_cancel.payload_type = phk::battle::BattlePayloadType::Snapshot;
    const auto plaintext_result = server.Dispatch(plaintext_after_cancel, {'s'});
    CHECK_TRUE(!plaintext_result.ok);
    CHECK_EQ(plaintext_result.reason, std::string("match_cancelled"));

    phk::battle::BattleEncryptedPacket encrypted_after_cancel;
    encrypted_after_cancel.header.match_id = "match-001";
    encrypted_after_cancel.header.player_id = "p1";
    encrypted_after_cancel.header.tick = 2;
    encrypted_after_cancel.header.seq = 2;
    encrypted_after_cancel.header.payload_type = phk::battle::BattlePayloadType::Input;
    FillEncryptedHeaderShape(encrypted_after_cancel.header);
    encrypted_after_cancel.ciphertext = {1, 2, 3};
    encrypted_after_cancel.auth_tag = std::vector<std::uint8_t>(16, 7);
    const auto encrypted_result = server.DispatchEncrypted(encrypted_after_cancel);
    CHECK_TRUE(!encrypted_result.ok);
    CHECK_EQ(encrypted_result.reason, std::string("match_cancelled"));

    const auto input_after_cancel = server.AcceptInput(MakeInput("p1", 2, 2, 1u << 3));
    CHECK_TRUE(!input_after_cancel.ok);
    CHECK_EQ(input_after_cancel.reason, std::string("match_unknown"));

    const auto missing_cancel = server.CancelMatch("missing-match");
    CHECK_TRUE(!missing_cancel.ok);
    CHECK_EQ(missing_cancel.reason, std::string("match_unknown"));

    phk::battle::BattleServer settled_server(config);
    CHECK_TRUE(settled_server.RegisterTicket(MakeTicket()).ok);
    CHECK_TRUE(settled_server.RegisterTicket(MakeTicketForBob()).ok);
    CHECK_TRUE(settled_server.AcceptInput(MakeInput("p1", 1, 1, 1u << 3)).ok);
    CHECK_TRUE(settled_server.AcceptInput(MakeInput("p2", 1, 1, 1u << 2)).ok);
    CHECK_EQ(settled_server.TickMatch("match-001").snapshot_tick, static_cast<std::uint64_t>(1));
    const auto built_result = settled_server.BuildSignedBattleResult("match-001");
    CHECK_TRUE(built_result.ok);
    CHECK_TRUE(settled_server.SubmitBattleResult(built_result.signed_result).ok);
    const auto cancel_settled = settled_server.CancelMatch("match-001");
    CHECK_TRUE(!cancel_settled.ok);
    CHECK_EQ(cancel_settled.reason, std::string("match_settled"));
    CHECK_EQ(cancel_settled.active_sessions_after, static_cast<std::size_t>(2));
    CHECK_EQ(cancel_settled.active_matches_after, static_cast<std::size_t>(1));
    CHECK_TRUE(settled_server.RetireMatch("match-001").ok);
    return true;
}

bool TestAuthoritativeReplay60TickFixture() {
    phk::battle::SimulationConfig config = MakeAuthoritativeReplay60Config("match-replay-60");

    phk::battle::BattleSimulation first(config);
    phk::battle::BattleSimulation second(config);
    CHECK_TRUE(AddReplayFixturePlayers(first));
    CHECK_TRUE(AddReplayFixturePlayers(second));
    CHECK_TRUE(DriveAuthoritativeReplay60Ticks(first, &second));

    const auto first_summary = first.Summary();
    const auto second_summary = second.Summary();
    CHECK_EQ(first_summary.final_tick, static_cast<std::uint64_t>(60));
    CHECK_EQ(first_summary.match_seed, config.match_seed);
    CHECK_EQ(first_summary.input_count, static_cast<std::uint64_t>(120));
    CHECK_EQ(first_summary.fallback_input_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(first_summary.neutral_fallback_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(first_summary.held_input_fallback_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(first_summary.mode_action_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(first_summary.event_count, static_cast<std::uint64_t>(4));
    CHECK_EQ(first_summary.input_trace.size(), static_cast<std::size_t>(120));
    CHECK_EQ(first_summary.event_trace.size(), static_cast<std::size_t>(4));
    CHECK_TRUE(first_summary.input_trace == second_summary.input_trace);
    CHECK_TRUE(first_summary.event_trace == second_summary.event_trace);
    CHECK_TRUE(first_summary.input_trace.front().find("input|p1|tick=1|seq=1") != std::string::npos);
    CHECK_TRUE(first_summary.input_trace.back().find("input|p2|tick=60|seq=60") != std::string::npos);
    CHECK_TRUE(first_summary.event_trace.front().find("bullet_spawn|tick=15") != std::string::npos);
    CHECK_TRUE(first_summary.event_trace.back().find("bullet_spawn|tick=60") != std::string::npos);
    CHECK_EQ(first.BulletCount(), second.BulletCount());
    CHECK_EQ(first_summary.input_stream_hash, second_summary.input_stream_hash);
    CHECK_EQ(first_summary.event_stream_hash, second_summary.event_stream_hash);
    CHECK_EQ(first_summary.final_state_hash, second_summary.final_state_hash);
    CHECK_EQ(first_summary.input_stream_hash, std::string("fnv64:183370bd6f8c18e7"));
    CHECK_EQ(first_summary.event_stream_hash, std::string("fnv64:daa6853bacb4fdd3"));
    CHECK_EQ(first_summary.final_state_hash, std::string("fnv64:7c13fa803ae1b2dd"));
    CHECK_EQ(ExpectedDevResultHash(first_summary), std::string("sha256:dev-fnv64-70b0875fedb4037c"));
    CHECK_EQ(ExpectedDevReplayId(first_summary), std::string("battle-replay:match-replay-60:60"));
    CHECK_EQ(first.BulletCount(), static_cast<std::size_t>(10));
    CHECK_EQ(first.Snapshot().mode_state.at("tick_rate_hz"), std::string("60"));
    CHECK_EQ(first.Snapshot().mode_state.at("mode_id"), std::string("pvp_duel"));
    CHECK_EQ(first.Snapshot().mode_state.at("accepted_input_count"), std::string("120"));
    CHECK_EQ(first.Snapshot().mode_state.at("fallback_input_count"), std::string("0"));
    const auto reconnect_snapshot = first.ReconnectSnapshot("p1", first_summary.event_count - 1);
    CHECK_EQ(reconnect_snapshot.snapshot_kind, std::string("reconnect"));
    CHECK_EQ(reconnect_snapshot.snapshot_tick, static_cast<std::uint64_t>(60));
    CHECK_EQ(reconnect_snapshot.state_hash, first_summary.final_state_hash);
    CHECK_EQ(reconnect_snapshot.mode_state.at("missed_event_count"), std::string("1"));
    CHECK_EQ(reconnect_snapshot.event_cursor, first_summary.event_count);
    const auto cursor_ahead = first.ReconnectSnapshot("p1", first_summary.event_count + 1);
    CHECK_EQ(cursor_ahead.snapshot_kind, std::string("event_cursor_ahead"));
    CHECK_EQ(cursor_ahead.event_cursor, first_summary.event_count);
    return true;
}

bool TestReplayFixtureBoundary() {
    phk::battle::SimulationConfig config = MakeAuthoritativeReplay60Config("match-replay-fixture");

    phk::battle::BattleSimulation simulation(config);
    CHECK_TRUE(AddReplayFixturePlayers(simulation));
    CHECK_TRUE(DriveAuthoritativeReplay60Ticks(simulation));

    const auto fixture = simulation.BuildReplayFixture("user-alice");
    CHECK_EQ(fixture.replay_id, ExpectedDevReplayId(fixture.summary));
    CHECK_EQ(fixture.owner_user_id, std::string("user-alice"));
    CHECK_EQ(fixture.match_id, config.match_id);
    CHECK_EQ(fixture.mode_id, config.mode_id);
    CHECK_EQ(fixture.ruleset_version, std::string(phk::v1::kRulesetVersion));
    CHECK_EQ(fixture.result_hash, ExpectedDevResultHash(fixture.summary));
    CHECK_EQ(fixture.player_ids.size(), static_cast<std::size_t>(2));
    CHECK_EQ(fixture.player_ids[0], std::string("p1"));
    CHECK_EQ(fixture.player_ids[1], std::string("p2"));
    CHECK_TRUE(fixture.summary.player_ids == fixture.player_ids);
    CHECK_EQ(fixture.tick_rate_hz, phk::battle::kBattleTickRateHz);
    CHECK_EQ(fixture.match_seed, config.match_seed);
    CHECK_EQ(fixture.event_cursor, fixture.summary.event_count);
    CHECK_TRUE(fixture.server_authoritative);
    CHECK_EQ(fixture.replay_summary_record.version.protocol_version, phk::v1::kProtocolVersion);
    CHECK_EQ(fixture.replay_summary_record.version.business_api_version, std::string(phk::v1::kBusinessApiVersion));
    CHECK_EQ(fixture.replay_summary_record.version.battle_api_version, std::string(phk::v1::kBattleApiVersion));
    CHECK_EQ(fixture.replay_summary_record.version.ruleset_version, std::string(phk::v1::kRulesetVersion));
    CHECK_EQ(fixture.replay_summary_record.replay_id, fixture.replay_id);
    CHECK_EQ(fixture.replay_summary_record.owner_user_id, fixture.owner_user_id);
    CHECK_EQ(fixture.replay_summary_record.match_id, fixture.match_id);
    CHECK_EQ(fixture.replay_summary_record.input_count, fixture.summary.input_count);
    CHECK_EQ(fixture.replay_summary_record.event_count, fixture.summary.event_count);
    CHECK_EQ(fixture.replay_summary_record.input_stream_hash, fixture.summary.input_stream_hash);
    CHECK_EQ(fixture.replay_summary_record.event_stream_hash, fixture.summary.event_stream_hash);
    CHECK_EQ(fixture.replay_summary_record.final_state_hash, fixture.summary.final_state_hash);
    CHECK_EQ(fixture.replay_summary_record.match_seed, fixture.summary.match_seed);
    CHECK_EQ(fixture.replay_summary_record.final_tick, fixture.summary.final_tick);
    CHECK_EQ(fixture.summary.final_tick, static_cast<std::uint64_t>(60));
    CHECK_EQ(fixture.summary.match_seed, config.match_seed);
    CHECK_EQ(fixture.summary.input_count, static_cast<std::uint64_t>(120));
    CHECK_EQ(fixture.summary.fallback_input_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(fixture.summary.event_count, static_cast<std::uint64_t>(4));
    CHECK_EQ(fixture.summary.input_stream_hash, std::string("fnv64:a0b383d4a7be0bf7"));
    CHECK_EQ(fixture.summary.event_stream_hash, std::string("fnv64:daa6853bacb4fdd3"));
    CHECK_EQ(fixture.summary.final_state_hash, std::string("fnv64:8049946f03724f36"));
    CHECK_EQ(fixture.result_hash, std::string("sha256:dev-fnv64-a7c5633652ee3768"));
    CHECK_EQ(fixture.replay_id, std::string("battle-replay:match-replay-fixture:60"));
    CHECK_TRUE(fixture.input_trace == fixture.summary.input_trace);
    CHECK_TRUE(fixture.event_trace == fixture.summary.event_trace);
    CHECK_EQ(fixture.input_trace.size(), static_cast<std::size_t>(120));
    CHECK_EQ(fixture.event_trace.size(), static_cast<std::size_t>(4));
    CHECK_TRUE(fixture.input_trace.front().find("input|p1|tick=1|seq=1") != std::string::npos);
    CHECK_TRUE(fixture.event_trace.front().find("bullet_spawn|tick=15") != std::string::npos);
    auto tampered_summary = fixture.summary;
    tampered_summary.input_trace[0] += "|tampered";
    CHECK_TRUE(ExpectedDevResultHash(tampered_summary) != fixture.result_hash);
    auto tampered_summary_mode = fixture.summary;
    tampered_summary_mode.mode_id = "battle_royale";
    CHECK_TRUE(ExpectedDevResultHash(tampered_summary_mode) != fixture.result_hash);
    auto tampered_summary_ruleset = fixture.summary;
    tampered_summary_ruleset.ruleset_version = "ruleset-other";
    CHECK_TRUE(ExpectedDevResultHash(tampered_summary_ruleset) != fixture.result_hash);
    auto tampered_summary_event_trace = fixture.summary;
    tampered_summary_event_trace.event_trace.back() += "|tampered";
    CHECK_TRUE(ExpectedDevResultHash(tampered_summary_event_trace) != fixture.result_hash);
    auto tampered_summary_fallback = fixture.summary;
    tampered_summary_fallback.fallback_input_count += 1;
    CHECK_TRUE(ExpectedDevResultHash(tampered_summary_fallback) != fixture.result_hash);
    auto tampered_summary_players = fixture.summary;
    tampered_summary_players.player_ids = {"p1"};
    CHECK_TRUE(ExpectedDevResultHash(tampered_summary_players) != fixture.result_hash);
    CHECK_EQ(fixture.final_snapshot.snapshot_kind, std::string("replay_final"));
    CHECK_EQ(fixture.final_snapshot.snapshot_tick, static_cast<std::uint64_t>(60));
    CHECK_EQ(fixture.final_snapshot.state_hash, fixture.summary.final_state_hash);
    CHECK_EQ(fixture.final_snapshot.event_cursor, fixture.event_cursor);
    CHECK_EQ(fixture.final_snapshot.mode_state.at("mode_id"), config.mode_id);
    CHECK_EQ(fixture.final_snapshot.mode_state.at("tick_rate_hz"), std::string("60"));
    CHECK_EQ(fixture.final_snapshot.mode_state.at("match_seed"), std::to_string(config.match_seed));
    CHECK_EQ(fixture.final_snapshot.mode_state.at("accepted_input_count"), std::string("120"));
    CHECK_EQ(fixture.final_snapshot.mode_state.at("fallback_input_count"), std::string("0"));
    CHECK_EQ(fixture.final_snapshot.players.size(), static_cast<std::size_t>(2));
    CHECK_EQ(fixture.final_snapshot.bullets_delta.size(), static_cast<std::size_t>(10));
    CHECK_TRUE(fixture.result_hash.rfind("sha256:dev-fnv64-", 0) == 0);

    const auto summary_record = simulation.BuildReplayInputStreamSummary("user-alice");
    CHECK_EQ(summary_record.replay_id, fixture.replay_id);
    CHECK_EQ(summary_record.owner_user_id, std::string("user-alice"));
    CHECK_EQ(summary_record.match_id, config.match_id);
    CHECK_EQ(summary_record.input_count, static_cast<std::uint64_t>(120));
    CHECK_EQ(summary_record.event_count, static_cast<std::uint64_t>(4));
    CHECK_EQ(summary_record.input_stream_hash, fixture.summary.input_stream_hash);
    CHECK_EQ(summary_record.event_stream_hash, fixture.summary.event_stream_hash);
    CHECK_EQ(summary_record.final_state_hash, fixture.summary.final_state_hash);
    CHECK_EQ(summary_record.match_seed, config.match_seed);
    CHECK_EQ(summary_record.final_tick, static_cast<std::uint64_t>(60));
    CHECK_TRUE(
        phk::battle::CanonicalReplayInputStreamSummaryRecord(summary_record) ==
        "1|0.1.0-draft|0.1.0-draft|ruleset-local-s0|battle-replay:match-replay-fixture:60|"
        "user-alice|match-replay-fixture|120|4|fnv64:a0b383d4a7be0bf7|"
        "fnv64:daa6853bacb4fdd3|fnv64:8049946f03724f36|424242|60"
    );
    CHECK_EQ(
        phk::battle::DevReplayInputStreamSummaryHash(summary_record),
        std::string("sha256:dev-fnv64-28cdfb99face4a10")
    );
    auto tampered_record = summary_record;
    tampered_record.match_seed += 1;
    CHECK_TRUE(
        phk::battle::CanonicalReplayInputStreamSummaryRecord(tampered_record) !=
        phk::battle::CanonicalReplayInputStreamSummaryRecord(summary_record)
    );
    CHECK_TRUE(
        phk::battle::DevReplayInputStreamSummaryHash(tampered_record) !=
        phk::battle::DevReplayInputStreamSummaryHash(summary_record)
    );
    const auto canonical_fixture_payload = phk::battle::CanonicalReplayFixturePayload(fixture);
    CHECK_TRUE(canonical_fixture_payload.find("battle-replay:match-replay-fixture:60|user-alice") == 0);
    CHECK_TRUE(canonical_fixture_payload.find("|pvp_duel|ruleset-local-s0|") != std::string::npos);
    CHECK_TRUE(canonical_fixture_payload.find("|60|424242|4|1|") != std::string::npos);
    CHECK_TRUE(
        canonical_fixture_payload.find(
            "1|0.1.0-draft|0.1.0-draft|ruleset-local-s0|battle-replay:match-replay-fixture:60|"
            "user-alice|match-replay-fixture|120|4|fnv64:a0b383d4a7be0bf7|"
            "fnv64:daa6853bacb4fdd3|fnv64:8049946f03724f36|424242|60"
        ) != std::string::npos
    );
    CHECK_TRUE(canonical_fixture_payload.find("|match-replay-fixture|60|replay_final|fnv64:8049946f03724f36|4|") != std::string::npos);
    CHECK_TRUE(canonical_fixture_payload.find("|player=p1,92500,-90000,1,0;player=p2,-105000,90000,1,0;|") != std::string::npos);
    CHECK_TRUE(canonical_fixture_payload.find("bullet=b10,upsert,48000,2665,3000,0,4000,basic_radial,blue;") != std::string::npos);
    CHECK_TRUE(canonical_fixture_payload.find("mode=accepted_input_count=120;") != std::string::npos);
    CHECK_TRUE(canonical_fixture_payload.find("mode=mode_id=pvp_duel;") != std::string::npos);
    CHECK_TRUE(canonical_fixture_payload.find("|p1,p2,|") != std::string::npos);
    CHECK_TRUE(canonical_fixture_payload.find("input|p1|tick=1|seq=1") != std::string::npos);
    CHECK_TRUE(canonical_fixture_payload.find("bullet_spawn|tick=60") != std::string::npos);
    CHECK_EQ(
        phk::battle::DevReplayFixtureHash(fixture),
        std::string("sha256:dev-fnv64-31691f363321149c")
    );
    auto tampered_fixture_record = fixture;
    tampered_fixture_record.replay_summary_record.final_state_hash = "fnv64:0000000000000000";
    CHECK_TRUE(phk::battle::DevReplayFixtureHash(tampered_fixture_record) != phk::battle::DevReplayFixtureHash(fixture));
    auto tampered_fixture_snapshot = fixture;
    tampered_fixture_snapshot.final_snapshot.state_hash = "fnv64:0000000000000000";
    CHECK_TRUE(phk::battle::DevReplayFixtureHash(tampered_fixture_snapshot) != phk::battle::DevReplayFixtureHash(fixture));
    auto tampered_fixture_player = fixture;
    tampered_fixture_player.final_snapshot.players[0].x_milli += 1;
    CHECK_TRUE(phk::battle::DevReplayFixtureHash(tampered_fixture_player) != phk::battle::DevReplayFixtureHash(fixture));
    auto tampered_fixture_bullet = fixture;
    tampered_fixture_bullet.final_snapshot.bullets_delta[0].pattern_id = "tampered_pattern";
    CHECK_TRUE(phk::battle::DevReplayFixtureHash(tampered_fixture_bullet) != phk::battle::DevReplayFixtureHash(fixture));
    auto tampered_fixture_mode_state = fixture;
    tampered_fixture_mode_state.final_snapshot.mode_state["mode_id"] = "tampered_mode";
    CHECK_TRUE(phk::battle::DevReplayFixtureHash(tampered_fixture_mode_state) != phk::battle::DevReplayFixtureHash(fixture));
    auto tampered_fixture_trace = fixture;
    tampered_fixture_trace.input_trace.back() += "|tampered";
    CHECK_TRUE(phk::battle::DevReplayFixtureHash(tampered_fixture_trace) != phk::battle::DevReplayFixtureHash(fixture));
    auto tampered_fixture_seed = fixture;
    tampered_fixture_seed.match_seed += 1;
    CHECK_TRUE(phk::battle::DevReplayFixtureHash(tampered_fixture_seed) != phk::battle::DevReplayFixtureHash(fixture));
    auto tampered_fixture_authority = fixture;
    tampered_fixture_authority.server_authoritative = false;
    CHECK_TRUE(phk::battle::DevReplayFixtureHash(tampered_fixture_authority) != phk::battle::DevReplayFixtureHash(fixture));
    CHECK_TRUE(phk::v1::HasMessageField("ReplayInputStreamSummary", "replay_id"));
    CHECK_TRUE(phk::v1::HasMessageField("ReplayInputStreamSummary", "owner_user_id"));
    CHECK_TRUE(phk::v1::HasMessageField("ReplayInputStreamSummary", "event_stream_hash"));
    return true;
}

bool TestReplayRecordBridgeBoundary() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489640000;
    phk::battle::BattleServer server(config);
    CHECK_TRUE(server.RegisterTicket(MakeTicket()).ok);
    CHECK_TRUE(server.RegisterTicket(MakeTicketForBob()).ok);
    CHECK_TRUE(server.AcceptInput(MakeInput("p1", 1, 1, 1u << 3)).ok);
    CHECK_TRUE(server.AcceptInput(MakeInput("p2", 1, 1, 1u << 2)).ok);
    CHECK_EQ(server.TickMatch("match-001").snapshot_tick, static_cast<std::uint64_t>(1));

    const auto missing = server.BuildReplayRecord("match-missing", "user-alice", "stage-dev");
    CHECK_TRUE(!missing.ok);
    CHECK_EQ(missing.reason, std::string("match_unknown"));

    const auto built = server.BuildReplayRecord("match-001", "user-alice", "stage-dev");
    CHECK_TRUE(built.ok);
    CHECK_EQ(built.reason, std::string("ok"));
    CHECK_EQ(built.replay_record.replay_id, std::string("battle-replay:match-001:1"));
    CHECK_EQ(built.replay_record.match_id, std::string("match-001"));
    CHECK_EQ(built.replay_record.owner_user_id, std::string("user-alice"));
    CHECK_EQ(built.replay_record.mode_id, std::string("certification"));
    CHECK_EQ(built.replay_record.stage_id, std::string("stage-dev"));
    CHECK_EQ(built.replay_record.loadout.size(), static_cast<std::size_t>(2));
    CHECK_EQ(built.replay_record.loadout[0].user_id, std::string("user-alice"));
    CHECK_EQ(built.replay_record.loadout[0].player_id, std::string("p1"));
    CHECK_EQ(built.replay_record.loadout[0].stage_id, std::string("stage-dev"));
    CHECK_EQ(built.replay_record.loadout[0].deck_snapshot_hash, std::string("sha256:deck"));
    CHECK_EQ(built.replay_record.loadout[0].deck_ruleset_version, std::string(phk::v1::kRulesetVersion));
    CHECK_TRUE(built.replay_record.loadout[0].character_id.empty());
    CHECK_TRUE(built.replay_record.loadout[0].rating_code.empty());
    CHECK_TRUE(built.replay_record.loadout[0].deck_card_ids.empty());
    CHECK_EQ(built.replay_record.loadout[1].user_id, std::string("user-bob"));
    CHECK_EQ(built.replay_record.loadout[1].player_id, std::string("p2"));
    CHECK_EQ(built.replay_record.loadout[1].stage_id, std::string("stage-dev"));
    CHECK_EQ(built.replay_record.loadout[1].deck_snapshot_hash, std::string("sha256:deck"));
    CHECK_EQ(built.replay_record.loadout[1].deck_ruleset_version, std::string(phk::v1::kRulesetVersion));
    CHECK_TRUE(built.replay_record.server_authoritative);
    CHECK_EQ(built.replay_record.created_at_ms, config.now_ms);
    CHECK_EQ(built.replay_record.stream.replay_id, built.replay_record.replay_id);
    CHECK_EQ(built.replay_record.stream.owner_user_id, built.replay_record.owner_user_id);
    CHECK_EQ(built.replay_record.stream.match_id, built.replay_record.match_id);
    CHECK_EQ(built.replay_record.stream.input_count, static_cast<std::uint64_t>(2));
    CHECK_EQ(built.replay_record.stream.event_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(built.replay_record.stream.match_seed, static_cast<std::uint64_t>(16031087345790602692ull));
    CHECK_EQ(built.replay_record.stream.final_tick, static_cast<std::uint64_t>(1));
    CHECK_EQ(built.replay_record.stream.input_stream_hash, std::string("fnv64:6b09da7d62e0941e"));
    CHECK_EQ(built.replay_record.stream.event_stream_hash, std::string("fnv64:14650fb0739d0383"));
    CHECK_EQ(built.replay_record.stream.final_state_hash, std::string("fnv64:72a3385f1a7c7fe3"));
    CHECK_EQ(built.replay_record.settlement.result.match_id, built.replay_record.match_id);
    CHECK_EQ(built.replay_record.settlement.result.mode_id, built.replay_record.mode_id);
    CHECK_EQ(built.replay_record.settlement.result.replay_id, built.replay_record.replay_id);
    CHECK_EQ(
        built.replay_record.settlement.result.result_hash,
        std::string("sha256:dev-fnv64-19959dc47479580d")
    );
    CHECK_TRUE(built.replay_record.settlement.server_authoritative);
    CHECK_EQ(
        built.replay_record.settlement.signature_hex,
        phk::battle::DevBattleResultSignatureHex(
            built.replay_record.settlement.result,
            built.replay_record.settlement.key_id
        )
    );

    const auto canonical_record = phk::battle::CanonicalReplayRecordBridgePayload(built.replay_record);
    CHECK_TRUE(canonical_record.find("battle-replay:match-001:1|match-001|user-alice|certification|stage-dev|") != std::string::npos);
    CHECK_TRUE(
        phk::battle::CanonicalReplayLoadoutBridgePayload(built.replay_record.loadout) ==
        "loadout=user-alice,p1,,stage-dev,,sha256:deck,ruleset-local-s0,;"
        "loadout=user-bob,p2,,stage-dev,,sha256:deck,ruleset-local-s0,;"
    );
    CHECK_TRUE(
        canonical_record.find(
            "loadout=user-alice,p1,,stage-dev,,sha256:deck,ruleset-local-s0,;"
            "loadout=user-bob,p2,,stage-dev,,sha256:deck,ruleset-local-s0,;|"
        ) != std::string::npos
    );
    CHECK_TRUE(canonical_record.find("1|0.1.0-draft|0.1.0-draft|ruleset-local-s0|battle-replay:match-001:1|") != std::string::npos);
    CHECK_TRUE(canonical_record.find("user-alice|match-001|2|0|fnv64:6b09da7d62e0941e|") != std::string::npos);
    CHECK_TRUE(canonical_record.find("fnv64:14650fb0739d0383|fnv64:72a3385f1a7c7fe3|16031087345790602692|1") != std::string::npos);
    CHECK_TRUE(canonical_record.find("sha256:dev-fnv64-19959dc47479580d|battle-replay:match-001:1|") != std::string::npos);
    CHECK_TRUE(canonical_record.find("|ED25519|battle-local-1|") != std::string::npos);
    CHECK_EQ(
        built.replay_record_hash,
        phk::battle::DevReplayRecordBridgeHash(built.replay_record)
    );
    CHECK_EQ(
        built.replay_record_hash,
        std::string("sha256:dev-fnv64-08219ff97786a5bf")
    );

    auto tampered_stream = built.replay_record;
    tampered_stream.stream.final_state_hash = "fnv64:0000000000000000";
    CHECK_TRUE(phk::battle::DevReplayRecordBridgeHash(tampered_stream) != built.replay_record_hash);
    auto tampered_owner = built.replay_record;
    tampered_owner.owner_user_id = "user-eve";
    CHECK_TRUE(phk::battle::DevReplayRecordBridgeHash(tampered_owner) != built.replay_record_hash);
    auto tampered_loadout = built.replay_record;
    tampered_loadout.loadout[0].deck_snapshot_hash = "sha256:tampered-deck";
    CHECK_TRUE(phk::battle::DevReplayRecordBridgeHash(tampered_loadout) != built.replay_record_hash);
    auto tampered_settlement = built.replay_record;
    tampered_settlement.settlement.result.result_hash = "sha256:dev-fnv64-0000000000000000";
    CHECK_TRUE(phk::battle::DevReplayRecordBridgeHash(tampered_settlement) != built.replay_record_hash);
    auto tampered_settlement_match = built.replay_record;
    tampered_settlement_match.settlement.result.match_id = "match-other";
    CHECK_TRUE(phk::battle::DevReplayRecordBridgeHash(tampered_settlement_match) != built.replay_record_hash);
    auto tampered_settlement_mode = built.replay_record;
    tampered_settlement_mode.settlement.result.mode_id = "world_boss";
    CHECK_TRUE(phk::battle::DevReplayRecordBridgeHash(tampered_settlement_mode) != built.replay_record_hash);
    auto tampered_settlement_replay = built.replay_record;
    tampered_settlement_replay.settlement.result.replay_id = "battle-replay:match-other:1";
    CHECK_TRUE(phk::battle::DevReplayRecordBridgeHash(tampered_settlement_replay) != built.replay_record_hash);
    auto tampered_authority = built.replay_record;
    tampered_authority.server_authoritative = false;
    CHECK_TRUE(phk::battle::DevReplayRecordBridgeHash(tampered_authority) != built.replay_record_hash);
    auto tampered_signature = built.replay_record;
    tampered_signature.settlement.signature_hex = RepeatHex('d', 128);
    CHECK_TRUE(phk::battle::DevReplayRecordBridgeHash(tampered_signature) != built.replay_record_hash);
    CHECK_TRUE(phk::v1::HasMessageField("ReplayRecord", "stream"));
    CHECK_TRUE(phk::v1::HasMessageField("ReplayRecord", "settlement"));
    CHECK_TRUE(phk::v1::HasMessageField("ReplayRecord", "server_authoritative"));
    return true;
}

bool TestResultAndReplayRecordUseStablePlayerOrder() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489650000;
    phk::battle::BattleServer server(config);

    auto bob_first_ticket = MakeTicketForBob();
    bob_first_ticket.ticket.ticket_id = "ticket-000";
    bob_first_ticket.ticket.ticket_nonce_hex = "000000000000000000000001";
    auto alice_late_ticket = MakeTicket();
    alice_late_ticket.ticket.ticket_id = "ticket-999";
    alice_late_ticket.ticket.ticket_nonce_hex = "000000000000000000000002";

    CHECK_TRUE(server.RegisterTicket(bob_first_ticket).ok);
    CHECK_TRUE(server.RegisterTicket(alice_late_ticket).ok);

    const auto built = server.BuildSignedBattleResult("match-001");
    CHECK_TRUE(built.ok);
    CHECK_EQ(built.signed_result.result.player_ids.size(), static_cast<std::size_t>(2));
    CHECK_EQ(built.signed_result.result.player_ids[0], std::string("p1"));
    CHECK_EQ(built.signed_result.result.player_ids[1], std::string("p2"));
    CHECK_TRUE(
        phk::battle::CanonicalBattleResultPayload(built.signed_result.result).find("|p1,p2,|") !=
        std::string::npos
    );
    CHECK_EQ(
        built.signed_result.signature_hex,
        phk::battle::DevBattleResultSignatureHex(built.signed_result.result, config.server_id)
    );

    const auto replay_record = server.BuildReplayRecord("match-001", "user-alice", "stage-order");
    CHECK_TRUE(replay_record.ok);
    CHECK_EQ(replay_record.replay_record.loadout.size(), static_cast<std::size_t>(2));
    CHECK_EQ(replay_record.replay_record.loadout[0].player_id, std::string("p1"));
    CHECK_EQ(replay_record.replay_record.loadout[0].user_id, std::string("user-alice"));
    CHECK_EQ(replay_record.replay_record.loadout[1].player_id, std::string("p2"));
    CHECK_EQ(replay_record.replay_record.loadout[1].user_id, std::string("user-bob"));
    CHECK_EQ(
        replay_record.replay_record_hash,
        phk::battle::DevReplayRecordBridgeHash(replay_record.replay_record)
    );

    const auto submitted = server.SubmitBattleResult(built.signed_result);
    CHECK_TRUE(submitted.ok);
    CHECK_EQ(submitted.reason, std::string("ok"));
    CHECK_TRUE(!submitted.duplicate);
    return true;
}

bool TestServerAuthoritativeInputAndSnapshot() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489605000;
    phk::battle::BattleServer server(config);
    CHECK_TRUE(server.RegisterTicket(MakeTicket()).ok);
    CHECK_TRUE(server.RegisterTicket(MakeTicketForBob()).ok);

    CHECK_TRUE(server.AcceptInput(MakeInput("p1", 1, 1, 1u << 3)).ok);
    CHECK_TRUE(server.AcceptInput(MakeInput("p2", 1, 1, 1u << 2)).ok);
    const auto snapshot = server.TickMatch("match-001");
    CHECK_EQ(snapshot.snapshot_tick, static_cast<std::uint64_t>(1));
    CHECK_EQ(snapshot.players.size(), static_cast<std::size_t>(2));
    CHECK_TRUE(!snapshot.state_hash.empty());
    CHECK_EQ(snapshot.mode_state.at("mode_id"), std::string("certification"));
    CHECK_EQ(snapshot.mode_state.at("ruleset_version"), std::string(phk::v1::kRulesetVersion));
    CHECK_EQ(snapshot.mode_state.at("tick_rate_hz"), std::string("60"));

    const auto replay_summary = server.MatchReplaySummary("match-001");
    CHECK_EQ(replay_summary.match_id, std::string("match-001"));
    CHECK_EQ(replay_summary.mode_id, std::string("certification"));
    CHECK_EQ(replay_summary.ruleset_version, std::string(phk::v1::kRulesetVersion));
    CHECK_EQ(replay_summary.final_tick, static_cast<std::uint64_t>(1));
    CHECK_EQ(replay_summary.input_count, static_cast<std::uint64_t>(2));
    CHECK_EQ(replay_summary.fallback_input_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(replay_summary.final_state_hash, snapshot.state_hash);

    auto action = MakeModeAction();
    action.tick = 2;
    action.seq = 2;
    const auto mode_action = server.AcceptModeAction(action);
    CHECK_TRUE(mode_action.ok);
    auto duplicate_action_id = action;
    duplicate_action_id.seq = 3;
    const auto duplicate_action_id_result = server.AcceptModeAction(duplicate_action_id);
    CHECK_TRUE(!duplicate_action_id_result.ok);
    CHECK_EQ(duplicate_action_id_result.reason, std::string("mode_action_duplicate"));
    const auto queued_mode_action_summary = server.MatchReplaySummary("match-001");
    CHECK_EQ(queued_mode_action_summary.event_count, replay_summary.event_count);
    CHECK_EQ(queued_mode_action_summary.event_stream_hash, replay_summary.event_stream_hash);
    CHECK_EQ(queued_mode_action_summary.mode_action_count, static_cast<std::uint64_t>(0));
    const auto queued_action_snapshot = server.MatchSnapshot("match-001");
    CHECK_EQ(queued_action_snapshot.mode_state.at("mode_action_count"), std::string("0"));
    CHECK_TRUE(queued_action_snapshot.mode_state.find("last_mode_action_id") == queued_action_snapshot.mode_state.end());

    CHECK_TRUE(server.AcceptInput(MakeInput("p2", 2, 2, 1u << 2)).ok);
    const auto after_action_snapshot = server.TickMatch("match-001");
    CHECK_EQ(after_action_snapshot.snapshot_tick, static_cast<std::uint64_t>(2));
    const auto mode_action_summary = server.MatchReplaySummary("match-001");
    CHECK_EQ(mode_action_summary.event_count, replay_summary.event_count + 1);
    CHECK_TRUE(mode_action_summary.event_stream_hash != replay_summary.event_stream_hash);
    CHECK_EQ(mode_action_summary.last_mode_action_id, action.action_id);
    CHECK_EQ(mode_action_summary.last_mode_action_type, action.action_type);
    CHECK_EQ(mode_action_summary.last_mode_action_player_id, action.player_id);
    CHECK_EQ(mode_action_summary.last_mode_action_tick, action.tick);
    CHECK_EQ(mode_action_summary.last_mode_action_seq, action.seq);
    CHECK_EQ(mode_action_summary.mode_action_count, static_cast<std::uint64_t>(1));
    CHECK_TRUE(mode_action_summary.input_trace.size() >= 4);
    CHECK_TRUE(mode_action_summary.event_trace.size() >= 1);
    CHECK_TRUE(mode_action_summary.event_trace.back().find("mode_action|p1|tick=2|seq=2") != std::string::npos);
    CHECK_EQ(after_action_snapshot.mode_state.at("last_mode_action_id"), action.action_id);
    CHECK_EQ(after_action_snapshot.mode_state.at("last_mode_action_type"), action.action_type);
    CHECK_EQ(after_action_snapshot.mode_state.at("last_mode_action_player_id"), action.player_id);
    CHECK_EQ(after_action_snapshot.mode_state.at("last_mode_action_tick"), std::to_string(action.tick));
    CHECK_EQ(after_action_snapshot.mode_state.at("last_mode_action_seq"), std::to_string(action.seq));
    CHECK_EQ(after_action_snapshot.mode_state.at("mode_action_count"), std::string("1"));
    CHECK_EQ(after_action_snapshot.mode_state.at("connected_player_count"), std::string("2"));
    CHECK_EQ(after_action_snapshot.mode_state.at("disconnected_player_count"), std::string("0"));

    const auto disconnected = server.SetPlayerConnected("match-001", "p2", false);
    CHECK_TRUE(disconnected.ok);
    const auto disconnected_snapshot = server.MatchSnapshot("match-001");
    CHECK_EQ(disconnected_snapshot.mode_state.at("connected_player_count"), std::string("1"));
    CHECK_EQ(disconnected_snapshot.mode_state.at("disconnected_player_count"), std::string("1"));
    bool saw_p2_disconnected = false;
    for (const auto& player : disconnected_snapshot.players) {
        if (player.player_id == "p2") {
            saw_p2_disconnected = !player.connected;
        }
    }
    CHECK_TRUE(saw_p2_disconnected);
    const auto disconnected_input = server.AcceptInput(MakeInput("p2", 3, 3, 1u << 2));
    CHECK_TRUE(!disconnected_input.ok);
    CHECK_EQ(disconnected_input.reason, std::string("player_disconnected"));
    CHECK_EQ(server.MatchReplaySummary("match-001").event_count, mode_action_summary.event_count + 1);
    CHECK_TRUE(server.MatchReplaySummary("match-001").event_trace.back().find("connection|disconnected|p2") != std::string::npos);

    auto disconnected_card_action = MakeModeAction(3);
    disconnected_card_action.player_id = "p2";
    disconnected_card_action.tick = 3;
    disconnected_card_action.action_id = "action-card-while-disconnected";
    disconnected_card_action.action_type = "cast_card";
    const auto disconnected_card_result = server.AcceptModeAction(disconnected_card_action);
    CHECK_TRUE(!disconnected_card_result.ok);
    CHECK_EQ(disconnected_card_result.reason, std::string("player_disconnected"));

    auto reconnect_action = MakeModeAction(3);
    reconnect_action.player_id = "p2";
    reconnect_action.tick = 3;
    reconnect_action.action_id = "action-reconnect-p2";
    reconnect_action.action_type = "reconnect";

    auto missing_reconnect_cursor = reconnect_action;
    missing_reconnect_cursor.action_id = "action-reconnect-p2-missing-cursor";
    missing_reconnect_cursor.payload_json = "{\"source\":\"client-reconnect\"}";
    const auto missing_reconnect_cursor_result = server.AcceptModeAction(missing_reconnect_cursor);
    CHECK_TRUE(!missing_reconnect_cursor_result.ok);
    CHECK_EQ(missing_reconnect_cursor_result.reason, std::string("reconnect_cursor_missing"));

    auto negative_reconnect_cursor = reconnect_action;
    negative_reconnect_cursor.action_id = "action-reconnect-p2-negative-cursor";
    negative_reconnect_cursor.payload_json = "{\"last_seen_event_cursor\":-1}";
    const auto negative_reconnect_cursor_result = server.AcceptModeAction(negative_reconnect_cursor);
    CHECK_TRUE(!negative_reconnect_cursor_result.ok);
    CHECK_EQ(negative_reconnect_cursor_result.reason, std::string("reconnect_cursor_invalid"));

    auto ahead_reconnect_cursor = reconnect_action;
    ahead_reconnect_cursor.action_id = "action-reconnect-p2-ahead-cursor";
    ahead_reconnect_cursor.payload_json =
        "{\"last_seen_event_cursor\":" + std::to_string(server.MatchReplaySummary("match-001").event_count + 1) + "}";
    const auto ahead_reconnect_cursor_result = server.AcceptModeAction(ahead_reconnect_cursor);
    CHECK_TRUE(!ahead_reconnect_cursor_result.ok);
    CHECK_EQ(ahead_reconnect_cursor_result.reason, std::string("reconnect_cursor_ahead"));
    CHECK_EQ(server.MatchReplaySummary("match-001").mode_action_count, static_cast<std::uint64_t>(1));
    CHECK_TRUE(!server.IsPlayerConnected("match-001", "p2"));

    reconnect_action.payload_json = "{\"last_seen_event_cursor\":" + std::to_string(mode_action_summary.event_count) + "}";
    const auto reconnect_action_result = server.AcceptModeAction(reconnect_action);
    CHECK_TRUE(reconnect_action_result.ok);
    CHECK_TRUE(!server.MatchSnapshot("match-001").players.empty());
    CHECK_EQ(server.MatchSnapshot("match-001").mode_state.at("connected_player_count"), std::string("1"));
    CHECK_EQ(server.MatchReplaySummary("match-001").mode_action_count, static_cast<std::uint64_t>(1));
    auto duplicate_reconnect_action = MakeModeAction(4);
    duplicate_reconnect_action.player_id = "p2";
    duplicate_reconnect_action.tick = 4;
    duplicate_reconnect_action.action_id = "action-reconnect-p2-duplicate";
    duplicate_reconnect_action.action_type = "reconnect";
    duplicate_reconnect_action.payload_json =
        "{\"last_seen_event_cursor\":" + std::to_string(mode_action_summary.event_count) + "}";
    const auto duplicate_reconnect_action_result = server.AcceptModeAction(duplicate_reconnect_action);
    CHECK_TRUE(!duplicate_reconnect_action_result.ok);
    CHECK_EQ(duplicate_reconnect_action_result.reason, std::string("reconnect_already_pending"));
    CHECK_EQ(server.MatchReplaySummary("match-001").mode_action_count, static_cast<std::uint64_t>(1));
    CHECK_TRUE(server.AcceptInput(MakeInput("p1", 3, 3, 1u << 3)).ok);
    const auto reconnect_action_snapshot = server.TickMatch("match-001");
    CHECK_EQ(reconnect_action_snapshot.snapshot_tick, static_cast<std::uint64_t>(3));
    CHECK_EQ(reconnect_action_snapshot.mode_state.at("connected_player_count"), std::string("2"));
    CHECK_EQ(reconnect_action_snapshot.mode_state.at("disconnected_player_count"), std::string("0"));
    CHECK_EQ(reconnect_action_snapshot.mode_state.at("last_mode_action_id"), reconnect_action.action_id);
    CHECK_EQ(reconnect_action_snapshot.mode_state.at("last_mode_action_type"), std::string("reconnect"));
    CHECK_TRUE(server.MatchReplaySummary("match-001").event_trace[server.MatchReplaySummary("match-001").event_trace.size() - 2].find("connection|connected|p2") != std::string::npos);
    CHECK_TRUE(server.MatchReplaySummary("match-001").event_trace.back().find("mode_action|p2|tick=3|seq=3") != std::string::npos);

    const auto reconnect_snapshot = server.ReconnectSnapshot("match-001", "p2", mode_action_summary.event_count);
    CHECK_EQ(reconnect_snapshot.snapshot_kind, std::string("reconnect"));
    CHECK_EQ(reconnect_snapshot.mode_state.at("reconnect_player_id"), std::string("p2"));
    bool saw_immediate_reconnect_event = false;
    for (const auto& trace : server.MatchReplaySummary("match-001").event_trace) {
        if (trace.find("connection|connected|p2") != std::string::npos) {
            saw_immediate_reconnect_event = true;
        }
    }
    CHECK_TRUE(saw_immediate_reconnect_event);
    CHECK_EQ(reconnect_snapshot.mode_state.at("missed_event_count"), std::string("3"));

    auto connected_reconnect_action = MakeModeAction(4);
    connected_reconnect_action.player_id = "p2";
    connected_reconnect_action.tick = 4;
    connected_reconnect_action.action_id = "action-reconnect-p2-connected";
    connected_reconnect_action.action_type = "reconnect";
    connected_reconnect_action.payload_json =
        "{\"last_seen_event_cursor\":" + std::to_string(server.MatchReplaySummary("match-001").event_count) + "}";
    const auto connected_reconnect_action_result = server.AcceptModeAction(connected_reconnect_action);
    CHECK_TRUE(!connected_reconnect_action_result.ok);
    CHECK_EQ(connected_reconnect_action_result.reason, std::string("reconnect_player_connected"));
    CHECK_EQ(server.MatchReplaySummary("match-001").last_mode_action_id, reconnect_action.action_id);

    const auto cursor_ahead_snapshot = server.ReconnectSnapshot("match-001", "p2", 999);
    CHECK_EQ(cursor_ahead_snapshot.snapshot_kind, std::string("event_cursor_ahead"));
    CHECK_EQ(cursor_ahead_snapshot.mode_state.at("requested_event_cursor"), std::string("999"));
    const auto unknown_reconnect_snapshot = server.ReconnectSnapshot("match-001", "p3", 0);
    CHECK_EQ(unknown_reconnect_snapshot.snapshot_kind, std::string("player_unknown"));
    const auto reconnected_input = server.AcceptInput(MakeInput("p2", 4, 4, 1u << 2));
    CHECK_TRUE(reconnected_input.ok);
    const auto reconnected_snapshot = server.MatchSnapshot("match-001");
    CHECK_EQ(reconnected_snapshot.mode_state.at("connected_player_count"), std::string("2"));
    CHECK_EQ(reconnected_snapshot.mode_state.at("disconnected_player_count"), std::string("0"));

    auto unknown_player = MakeInput("p3", 2, 1, 0);
    const auto unknown_player_result = server.AcceptInput(unknown_player);
    CHECK_TRUE(!unknown_player_result.ok);
    CHECK_EQ(unknown_player_result.reason, std::string("player_unknown"));

    auto forged_action = MakeModeAction(4);
    forged_action.tick = 4;
    forged_action.client_result_authoritative = true;
    const auto forged_result = server.AcceptModeAction(forged_action);
    CHECK_TRUE(!forged_result.ok);
    CHECK_EQ(forged_result.reason, std::string("mode_action_client_result_forbidden"));
    CHECK_EQ(server.MatchReplaySummary("match-001").last_mode_action_id, reconnect_action.action_id);

    auto wrong_match = MakeInput("p1", 1, 1, 0);
    wrong_match.match_id = "missing-match";
    const auto missing = server.AcceptInput(wrong_match);
    CHECK_TRUE(!missing.ok);
    CHECK_EQ(missing.reason, std::string("match_unknown"));
    auto missing_action = MakeModeAction(4);
    missing_action.match_id = "missing-match";
    const auto missing_action_result = server.AcceptModeAction(missing_action);
    CHECK_TRUE(!missing_action_result.ok);
    CHECK_EQ(missing_action_result.reason, std::string("match_unknown"));
    return true;
}

bool TestDispatcher() {
	phk::battle::BattleDispatcher dispatcher;
    phk::battle::BattlePacketHeader ping;
    ping.match_id = "match-001";
    ping.player_id = "p1";
    ping.tick = 10;
    ping.seq = 1;
    ping.payload_type = phk::battle::BattlePayloadType::Ping;
    FillEncryptedHeaderShape(ping);

    const auto pong = dispatcher.Dispatch(ping, {'p', 'i', 'n', 'g'});
    CHECK_TRUE(pong.ok);
    CHECK_EQ(pong.response_kind, std::string("pong"));

    const auto replay = dispatcher.Dispatch(ping, {'p', 'i', 'n', 'g'});
    CHECK_TRUE(!replay.ok);
    CHECK_EQ(replay.reason, std::string("seq_replay"));

    phk::battle::BattlePacketHeader mode_action = ping;
    mode_action.match_id = std::string(phk::v1::kBattleModeActionMatchId);
    mode_action.player_id = std::string(phk::v1::kBattleModeActionPlayerId);
    mode_action.seq = phk::v1::kBattleModeActionSeq;
    mode_action.tick = phk::v1::kBattleModeActionTick;
    mode_action.payload_type = phk::battle::BattlePayloadType::ModeAction;
    RefreshDevAeadNonce(mode_action);
    phk::battle::BattleModeAction action = MakeModeAction();
    CHECK_TRUE(!action.client_result_authoritative);
    const auto mode_action_result = dispatcher.Dispatch(mode_action, {'{', '}'});
    CHECK_TRUE(mode_action_result.ok);
    CHECK_EQ(mode_action_result.response_kind, std::string("mode_action"));
    CHECK_EQ(phk::battle::PayloadTypeName(phk::battle::BattlePayloadType::ModeAction), std::string("mode_action"));

    phk::battle::BattlePacketHeader empty_mode_action = mode_action;
    empty_mode_action.seq = phk::v1::kBattleModeActionSeq + 1;
    empty_mode_action.tick = phk::v1::kBattleModeActionTick + 1;
    RefreshDevAeadNonce(empty_mode_action);
    const auto empty_mode_action_result = dispatcher.Dispatch(empty_mode_action, {});
    CHECK_TRUE(empty_mode_action_result.ok);
    CHECK_EQ(empty_mode_action_result.response_kind, std::string("mode_action_empty_payload"));

    phk::battle::BattlePacketHeader nonce_mismatch = ping;
    nonce_mismatch.player_id = "p2";
    nonce_mismatch.seq = 1;
    nonce_mismatch.nonce_hex = RepeatHex('f', 24);
    const auto nonce_mismatch_result = dispatcher.Dispatch(nonce_mismatch, {'p'});
    CHECK_TRUE(!nonce_mismatch_result.ok);
    CHECK_EQ(nonce_mismatch_result.reason, std::string("nonce_mismatch"));

    phk::battle::BattlePacketHeader forbidden = ping;
    forbidden.seq = phk::v1::kBattleModeActionSeq + 2;
    forbidden.payload_type = phk::battle::BattlePayloadType::Result;
    const auto forbidden_result = dispatcher.Dispatch(forbidden, {});
    CHECK_TRUE(!forbidden_result.ok);
    CHECK_EQ(forbidden_result.reason, std::string("client_result_forbidden"));

    phk::battle::BattlePacketHeader missing_key = ping;
    missing_key.player_id = "p2";
    missing_key.seq = 1;
    missing_key.key_id.clear();
    const auto missing_key_result = dispatcher.Dispatch(missing_key, {'p'});
    CHECK_TRUE(!missing_key_result.ok);
    CHECK_EQ(missing_key_result.reason, std::string("key_id_missing"));

    phk::battle::BattlePacketHeader bad_nonce = ping;
    bad_nonce.player_id = "p2";
    bad_nonce.seq = 2;
    bad_nonce.nonce_hex = "not-hex";
    const auto bad_nonce_result = dispatcher.Dispatch(bad_nonce, {'p'});
    CHECK_TRUE(!bad_nonce_result.ok);
    CHECK_EQ(bad_nonce_result.reason, std::string("nonce_invalid"));

    phk::battle::BattlePacketHeader long_nonce = ping;
    long_nonce.player_id = "p2";
    long_nonce.seq = 3;
    long_nonce.nonce_hex = RepeatHex('1', 26);
    const auto long_nonce_result = dispatcher.Dispatch(long_nonce, {'p'});
    CHECK_TRUE(!long_nonce_result.ok);
    CHECK_EQ(long_nonce_result.reason, std::string("nonce_invalid"));
    return true;
}

bool TestEncryptedPacketAdapterShape() {
    phk::battle::BattleDispatcher dispatcher;
    phk::battle::BattleEncryptedPacket packet;
    packet.header.match_id = "match-001";
    packet.header.player_id = "p1";
    packet.header.tick = 10;
    packet.header.seq = 1;
    packet.header.payload_type = phk::battle::BattlePayloadType::Input;
    FillEncryptedHeaderShape(packet.header);
    packet.ciphertext = {'c', 'i', 'p', 'h', 'e', 'r'};
    packet.auth_tag.assign(16, 0x7a);

    const auto accepted = dispatcher.DispatchEncrypted(packet);
    CHECK_TRUE(accepted.ok);
    CHECK_EQ(accepted.response_kind, std::string("input"));

    auto nonce_replay = packet;
    nonce_replay.header.seq = 2;
    const auto nonce_replay_result = dispatcher.DispatchEncrypted(nonce_replay);
    CHECK_TRUE(!nonce_replay_result.ok);
    CHECK_EQ(nonce_replay_result.reason, std::string("nonce_replay"));

    auto missing_ciphertext = packet;
    missing_ciphertext.header.player_id = "p2";
    missing_ciphertext.header.seq = 1;
    missing_ciphertext.ciphertext.clear();
    const auto missing_ciphertext_result = dispatcher.DispatchEncrypted(missing_ciphertext);
    CHECK_TRUE(!missing_ciphertext_result.ok);
    CHECK_EQ(missing_ciphertext_result.reason, std::string("ciphertext_missing"));

    auto bad_tag = packet;
    bad_tag.header.player_id = "p2";
    bad_tag.header.seq = 2;
    RefreshDevAeadNonce(bad_tag.header);
    bad_tag.auth_tag.assign(15, 0x7a);
    const auto bad_tag_result = dispatcher.DispatchEncrypted(bad_tag);
    CHECK_TRUE(!bad_tag_result.ok);
    CHECK_EQ(bad_tag_result.reason, std::string("auth_tag_invalid"));

    auto missing_key = packet;
    missing_key.header.player_id = "p2";
    missing_key.header.seq = 3;
    missing_key.header.key_id.clear();
    const auto missing_key_result = dispatcher.DispatchEncrypted(missing_key);
    CHECK_TRUE(!missing_key_result.ok);
    CHECK_EQ(missing_key_result.reason, std::string("key_id_missing"));

    auto long_nonce = packet;
    long_nonce.header.player_id = "p2";
    long_nonce.header.seq = 4;
    long_nonce.header.nonce_hex = RepeatHex('2', 26);
    const auto long_nonce_result = dispatcher.DispatchEncrypted(long_nonce);
    CHECK_TRUE(!long_nonce_result.ok);
    CHECK_EQ(long_nonce_result.reason, std::string("nonce_invalid"));

    auto result_packet = packet;
    result_packet.header.player_id = "p2";
    result_packet.header.seq = 5;
    result_packet.header.payload_type = phk::battle::BattlePayloadType::Result;
    RefreshDevAeadNonce(result_packet.header);
    const auto result_packet_result = dispatcher.DispatchEncrypted(result_packet);
    CHECK_TRUE(!result_packet_result.ok);
    CHECK_EQ(result_packet_result.reason, std::string("client_result_forbidden"));

    auto event_packet = packet;
    event_packet.header.player_id = "p2";
    event_packet.header.seq = 6;
    event_packet.header.payload_type = phk::battle::BattlePayloadType::Event;
    RefreshDevAeadNonce(event_packet.header);
    const auto event_packet_result = dispatcher.DispatchEncrypted(event_packet);
    CHECK_TRUE(!event_packet_result.ok);
    CHECK_EQ(event_packet_result.reason, std::string("encrypted_payload_type_invalid"));

    auto nonce_mismatch = packet;
    nonce_mismatch.header.player_id = "p2";
    nonce_mismatch.header.seq = 7;
    nonce_mismatch.header.nonce_hex = RepeatHex('e', 24);
    const auto nonce_mismatch_result = dispatcher.DispatchEncrypted(nonce_mismatch);
    CHECK_TRUE(!nonce_mismatch_result.ok);
    CHECK_EQ(nonce_mismatch_result.reason, std::string("nonce_mismatch"));
    return true;
}

bool TestServerEncryptedPacketSessionBoundary() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489605000;
    phk::battle::BattleServer server(config);
    CHECK_TRUE(server.RegisterTicket(MakeTicket()).ok);
    CHECK_TRUE(server.RegisterTicket(MakeTicketForBob()).ok);

    phk::battle::BattleEncryptedPacket packet;
    packet.header.match_id = "match-001";
    packet.header.player_id = "p1";
    packet.header.tick = 1;
    packet.header.seq = 1;
    packet.header.payload_type = phk::battle::BattlePayloadType::Input;
    FillEncryptedHeaderShape(packet.header);
    packet.ciphertext = {'i', 'n', 'p', 'u', 't'};
    packet.auth_tag.assign(16, 0x42);

    const auto before_handshake = server.DispatchEncrypted(packet);
    CHECK_TRUE(!before_handshake.ok);
    CHECK_EQ(before_handshake.reason, std::string("handshake_required"));

    const auto accept = AcceptDefaultHandshake(server);
    CHECK_TRUE(accept.ok);
    CHECK_EQ(accept.player_id, std::string("p1"));
    CHECK_EQ(accept.client_to_server_key_ref.rfind("hkdf-dev:client_to_server:", 0), static_cast<std::size_t>(0));
    CHECK_EQ(accept.server_to_client_key_ref.rfind("hkdf-dev:server_to_client:", 0), static_cast<std::size_t>(0));
    CHECK_TRUE(accept.client_to_server_key_ref != config.signing_key_id);
    CHECK_TRUE(accept.server_to_client_key_ref != config.signing_key_id);
    const auto bob_accept = AcceptHandshakeForTicket(server, MakeTicketForBob());
    CHECK_TRUE(bob_accept.ok);
    CHECK_EQ(bob_accept.player_id, std::string("p2"));
    CHECK_TRUE(bob_accept.client_to_server_key_ref != accept.client_to_server_key_ref);

    auto ticket_key_packet = packet;
    ticket_key_packet.header.key_id = config.signing_key_id;
    RefreshDevAeadNonce(ticket_key_packet.header);
    const auto ticket_key_result = server.DispatchEncrypted(ticket_key_packet);
    CHECK_TRUE(!ticket_key_result.ok);
    CHECK_EQ(ticket_key_result.reason, std::string("session_key_mismatch"));

    auto outbound_key_packet = packet;
    outbound_key_packet.header.key_id = accept.server_to_client_key_ref;
    RefreshDevAeadNonce(outbound_key_packet.header);
    const auto outbound_key_result = server.DispatchEncrypted(outbound_key_packet);
    CHECK_TRUE(!outbound_key_result.ok);
    CHECK_EQ(outbound_key_result.reason, std::string("session_key_mismatch"));

    packet.header.key_id = accept.client_to_server_key_ref;
    RefreshDevAeadNonce(packet.header);
    const auto accepted = server.DispatchEncrypted(packet);
    CHECK_TRUE(accepted.ok);
    CHECK_EQ(accepted.response_kind, std::string("input"));

    auto nonce_replay = packet;
    nonce_replay.header.seq = 2;
    const auto nonce_replay_result = server.DispatchEncrypted(nonce_replay);
    CHECK_TRUE(!nonce_replay_result.ok);
    CHECK_EQ(nonce_replay_result.reason, std::string("nonce_replay"));

    auto wrong_key = packet;
    wrong_key.header.player_id = "p2";
    wrong_key.header.seq = 1;
    wrong_key.header.key_id = "other-key";
    RefreshDevAeadNonce(wrong_key.header);
    const auto wrong_key_result = server.DispatchEncrypted(wrong_key);
    CHECK_TRUE(!wrong_key_result.ok);
    CHECK_EQ(wrong_key_result.reason, std::string("session_key_mismatch"));

    auto ack_ahead = packet;
    ack_ahead.header.player_id = "p2";
    ack_ahead.header.key_id = bob_accept.client_to_server_key_ref;
    ack_ahead.header.seq = 1;
    ack_ahead.header.ack = 1;
    RefreshDevAeadNonce(ack_ahead.header);
    const auto ack_ahead_result = server.DispatchEncrypted(ack_ahead);
    CHECK_TRUE(!ack_ahead_result.ok);
    CHECK_EQ(ack_ahead_result.reason, std::string("encrypted_ack_ahead"));

    auto far_future_tick = packet;
    far_future_tick.header.player_id = "p2";
    far_future_tick.header.key_id = bob_accept.client_to_server_key_ref;
    far_future_tick.header.seq = 1;
    far_future_tick.header.tick = 99;
    RefreshDevAeadNonce(far_future_tick.header);
    const auto far_future_tick_result = server.DispatchEncrypted(far_future_tick);
    CHECK_TRUE(!far_future_tick_result.ok);
    CHECK_EQ(far_future_tick_result.reason, std::string("encrypted_tick_too_far_ahead"));

    CHECK_TRUE(server.AcceptInput(MakeInput("p2", 1, 1, 1u << 2)).ok);
    CHECK_EQ(server.TickMatch("match-001").snapshot_tick, static_cast<std::uint64_t>(1));
    auto stale_tick = packet;
    stale_tick.header.player_id = "p2";
    stale_tick.header.key_id = bob_accept.client_to_server_key_ref;
    stale_tick.header.seq = 2;
    stale_tick.header.tick = 1;
    RefreshDevAeadNonce(stale_tick.header);
    const auto stale_tick_result = server.DispatchEncrypted(stale_tick);
    CHECK_TRUE(!stale_tick_result.ok);
    CHECK_EQ(stale_tick_result.reason, std::string("encrypted_tick_too_old"));

    auto current_ack = packet;
    current_ack.header.player_id = "p2";
    current_ack.header.key_id = bob_accept.client_to_server_key_ref;
    current_ack.header.seq = 2;
    current_ack.header.tick = 2;
    current_ack.header.ack = 1;
    RefreshDevAeadNonce(current_ack.header);
    const auto current_ack_result = server.DispatchEncrypted(current_ack);
    CHECK_TRUE(current_ack_result.ok);
    CHECK_EQ(current_ack_result.response_kind, std::string("input"));

    auto ping_ack_ahead = packet;
    ping_ack_ahead.header.payload_type = phk::battle::BattlePayloadType::Ping;
    ping_ack_ahead.header.player_id = "p2";
    ping_ack_ahead.header.key_id = bob_accept.client_to_server_key_ref;
    ping_ack_ahead.header.seq = 3;
    ping_ack_ahead.header.tick = 1;
    ping_ack_ahead.header.ack = 2;
    ping_ack_ahead.ciphertext = {'p', 'i', 'n', 'g'};
    RefreshDevAeadNonce(ping_ack_ahead.header);
    const auto ping_ack_ahead_result = server.DispatchEncrypted(ping_ack_ahead);
    CHECK_TRUE(!ping_ack_ahead_result.ok);
    CHECK_EQ(ping_ack_ahead_result.reason, std::string("encrypted_ack_ahead"));

    auto ping_current_ack = ping_ack_ahead;
    ping_current_ack.header.ack = 1;
    RefreshDevAeadNonce(ping_current_ack.header);
    const auto ping_current_ack_result = server.DispatchEncrypted(ping_current_ack);
    CHECK_TRUE(ping_current_ack_result.ok);
    CHECK_EQ(ping_current_ack_result.response_kind, std::string("pong"));

    CHECK_TRUE(server.SetPlayerConnected("match-001", "p2", false).ok);
    auto disconnected_input = packet;
    disconnected_input.header.player_id = "p2";
    disconnected_input.header.key_id = bob_accept.client_to_server_key_ref;
    disconnected_input.header.seq = 4;
    disconnected_input.header.tick = 2;
    disconnected_input.header.ack = 1;
    RefreshDevAeadNonce(disconnected_input.header);
    const auto disconnected_input_result = server.DispatchEncrypted(disconnected_input);
    CHECK_TRUE(!disconnected_input_result.ok);
    CHECK_EQ(disconnected_input_result.reason, std::string("encrypted_player_disconnected"));

    auto reconnect_cursor_ahead = packet;
    reconnect_cursor_ahead.header.payload_type = phk::battle::BattlePayloadType::Reconnect;
    reconnect_cursor_ahead.header.player_id = "p2";
    reconnect_cursor_ahead.header.key_id = bob_accept.client_to_server_key_ref;
    reconnect_cursor_ahead.header.seq = 5;
    reconnect_cursor_ahead.header.tick = 1;
    reconnect_cursor_ahead.header.ack = server.MatchReplaySummary("match-001").event_count + 1;
    RefreshDevAeadNonce(reconnect_cursor_ahead.header);
    const auto reconnect_cursor_ahead_result = server.DispatchEncrypted(reconnect_cursor_ahead);
    CHECK_TRUE(!reconnect_cursor_ahead_result.ok);
    CHECK_EQ(reconnect_cursor_ahead_result.reason, std::string("encrypted_event_cursor_ahead"));
    CHECK_TRUE(!server.IsPlayerConnected("match-001", "p2"));

    CHECK_TRUE(server.SetPlayerConnected("match-001", "p2", true).ok);

    auto reconnect_current_cursor = packet;
    reconnect_current_cursor.header.payload_type = phk::battle::BattlePayloadType::Reconnect;
    reconnect_current_cursor.header.player_id = "p2";
    reconnect_current_cursor.header.key_id = bob_accept.client_to_server_key_ref;
    reconnect_current_cursor.header.seq = 5;
    reconnect_current_cursor.header.tick = 1;
    reconnect_current_cursor.header.ack = server.MatchReplaySummary("match-001").event_count;
    RefreshDevAeadNonce(reconnect_current_cursor.header);
    const auto reconnect_current_cursor_result = server.DispatchEncrypted(reconnect_current_cursor);
    CHECK_TRUE(!reconnect_current_cursor_result.ok);
    CHECK_EQ(reconnect_current_cursor_result.reason, std::string("encrypted_reconnect_player_connected"));

    CHECK_TRUE(server.SetPlayerConnected("match-001", "p2", false).ok);
    const auto disconnected_reconnect_current_cursor_result = server.DispatchEncrypted(reconnect_current_cursor);
    CHECK_TRUE(disconnected_reconnect_current_cursor_result.ok);
    CHECK_EQ(disconnected_reconnect_current_cursor_result.response_kind, std::string("reconnect"));
    CHECK_TRUE(server.SetPlayerConnected("match-001", "p2", true).ok);

    auto unknown_player = packet;
    unknown_player.header.player_id = "p3";
    unknown_player.header.seq = 1;
    RefreshDevAeadNonce(unknown_player.header);
    const auto unknown_player_result = server.DispatchEncrypted(unknown_player);
    CHECK_TRUE(!unknown_player_result.ok);
    CHECK_EQ(unknown_player_result.reason, std::string("player_unknown"));

    auto unknown_match = packet;
    unknown_match.header.match_id = "match-missing";
    unknown_match.header.seq = 1;
    RefreshDevAeadNonce(unknown_match.header);
    const auto unknown_match_result = server.DispatchEncrypted(unknown_match);
    CHECK_TRUE(!unknown_match_result.ok);
    CHECK_EQ(unknown_match_result.reason, std::string("match_unknown"));

    auto result_packet = packet;
    result_packet.header.player_id = "p2";
    result_packet.header.seq = 1;
    result_packet.header.payload_type = phk::battle::BattlePayloadType::Result;
    RefreshDevAeadNonce(result_packet.header);
    const auto result_packet_result = server.DispatchEncrypted(result_packet);
    CHECK_TRUE(!result_packet_result.ok);
    CHECK_EQ(result_packet_result.reason, std::string("client_result_forbidden"));

    auto event_packet = packet;
    event_packet.header.payload_type = phk::battle::BattlePayloadType::Event;
    event_packet.header.player_id = "p2";
    event_packet.header.key_id = bob_accept.client_to_server_key_ref;
    event_packet.header.seq = 6;
    event_packet.header.tick = 2;
    RefreshDevAeadNonce(event_packet.header);
    const auto event_packet_result = server.DispatchEncrypted(event_packet);
    CHECK_TRUE(!event_packet_result.ok);
    CHECK_EQ(event_packet_result.reason, std::string("encrypted_payload_type_invalid"));

    auto snapshot_packet = event_packet;
    snapshot_packet.header.payload_type = phk::battle::BattlePayloadType::Snapshot;
    RefreshDevAeadNonce(snapshot_packet.header);
    const auto snapshot_packet_result = server.DispatchEncrypted(snapshot_packet);
    CHECK_TRUE(!snapshot_packet_result.ok);
    CHECK_EQ(snapshot_packet_result.reason, std::string("encrypted_payload_type_invalid"));

    auto valid_after_invalid_payload = packet;
    valid_after_invalid_payload.header.player_id = "p2";
    valid_after_invalid_payload.header.key_id = bob_accept.client_to_server_key_ref;
    valid_after_invalid_payload.header.seq = 6;
    valid_after_invalid_payload.header.tick = 2;
    valid_after_invalid_payload.header.ack = 1;
    RefreshDevAeadNonce(valid_after_invalid_payload.header);
    const auto valid_after_invalid_payload_result = server.DispatchEncrypted(valid_after_invalid_payload);
    CHECK_TRUE(valid_after_invalid_payload_result.ok);
    CHECK_EQ(valid_after_invalid_payload_result.response_kind, std::string("input"));
    return true;
}

bool TestDecodedPayloadHeaderBinding() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489605000;
    phk::battle::BattleServer server(config);
    CHECK_TRUE(server.RegisterTicket(MakeTicket()).ok);
    CHECK_TRUE(server.RegisterTicket(MakeTicketForBob()).ok);
    const auto accept = AcceptDefaultHandshake(server);
    CHECK_TRUE(accept.ok);
    const auto bob_accept = AcceptHandshakeForTicket(server, MakeTicketForBob());
    CHECK_TRUE(bob_accept.ok);

    phk::battle::BattlePacketHeader input_header;
    input_header.match_id = "match-001";
    input_header.player_id = "p1";
    input_header.tick = 1;
    input_header.seq = 1;
    input_header.payload_type = phk::battle::BattlePayloadType::Input;
    input_header.key_id = accept.client_to_server_key_ref;
    input_header.nonce_hex = RepeatHex('1', 24);

    auto input = MakeInput("p1", 1, 1, 1u << 3);
    const auto accepted_input = server.AcceptDecodedInput(input_header, input);
    CHECK_TRUE(accepted_input.ok);
    CHECK_EQ(server.MatchReplaySummary("match-001").input_count, static_cast<std::uint64_t>(1));

    auto wrong_player_input = MakeInput("p2", 2, 2, 1u << 2);
    auto mismatch_header = input_header;
    mismatch_header.tick = 2;
    mismatch_header.seq = 2;
    const auto wrong_player_result = server.AcceptDecodedInput(mismatch_header, wrong_player_input);
    CHECK_TRUE(!wrong_player_result.ok);
    CHECK_EQ(wrong_player_result.reason, std::string("decoded_input_header_mismatch"));
    CHECK_EQ(server.MatchReplaySummary("match-001").input_count, static_cast<std::uint64_t>(1));

    auto wrong_payload_header = mismatch_header;
    wrong_payload_header.player_id = "p2";
    wrong_payload_header.payload_type = phk::battle::BattlePayloadType::Ping;
    const auto wrong_payload_type = server.AcceptDecodedInput(wrong_payload_header, wrong_player_input);
    CHECK_TRUE(!wrong_payload_type.ok);
    CHECK_EQ(wrong_payload_type.reason, std::string("decoded_input_payload_type_mismatch"));
    CHECK_EQ(server.MatchReplaySummary("match-001").input_count, static_cast<std::uint64_t>(1));

    auto wrong_version_input = MakeInput("p2", 2, 2, 1u << 2);
    auto wrong_version_header = input_header;
    wrong_version_header.player_id = "p2";
    wrong_version_header.tick = 2;
    wrong_version_header.seq = 2;
    wrong_version_header.version.ruleset_version = "ruleset-other";
    const auto wrong_version_result = server.AcceptDecodedInput(wrong_version_header, wrong_version_input);
    CHECK_TRUE(!wrong_version_result.ok);
    CHECK_EQ(wrong_version_result.reason, std::string("decoded_input_header_mismatch"));
    CHECK_EQ(server.MatchReplaySummary("match-001").input_count, static_cast<std::uint64_t>(1));

    auto p2_input = MakeInput("p2", 1, 1, 1u << 2);
    auto p2_header = input_header;
    p2_header.player_id = "p2";
    p2_header.tick = 1;
    p2_header.seq = 1;
    p2_header.key_id = bob_accept.client_to_server_key_ref;

    auto wrong_key_p2_header = p2_header;
    wrong_key_p2_header.key_id = accept.client_to_server_key_ref;
    const auto wrong_key_p2_result = server.AcceptDecodedInput(wrong_key_p2_header, p2_input);
    CHECK_TRUE(!wrong_key_p2_result.ok);
    CHECK_EQ(wrong_key_p2_result.reason, std::string("session_key_mismatch"));
    CHECK_EQ(server.MatchReplaySummary("match-001").input_count, static_cast<std::uint64_t>(1));

    const auto accepted_p2_input = server.AcceptDecodedInput(p2_header, p2_input);
    CHECK_TRUE(accepted_p2_input.ok);
    CHECK_EQ(server.MatchReplaySummary("match-001").input_count, static_cast<std::uint64_t>(2));
    CHECK_EQ(server.TickMatch("match-001").snapshot_tick, static_cast<std::uint64_t>(1));

    auto ack_ahead_input = MakeInput("p2", 2, 2, 1u << 2);
    auto ack_ahead_header = p2_header;
    ack_ahead_header.tick = 2;
    ack_ahead_header.seq = 2;
    ack_ahead_header.ack = 2;
    const auto ack_ahead_result = server.AcceptDecodedInput(ack_ahead_header, ack_ahead_input);
    CHECK_TRUE(!ack_ahead_result.ok);
    CHECK_EQ(ack_ahead_result.reason, std::string("snapshot_ack_ahead"));

    auto action = MakeModeAction(2);
    action.tick = 2;
    phk::battle::BattlePacketHeader action_header;
    action_header.match_id = action.match_id;
    action_header.player_id = action.player_id;
    action_header.tick = action.tick;
    action_header.seq = action.seq;
    action_header.payload_type = phk::battle::BattlePayloadType::ModeAction;
    action_header.key_id = accept.client_to_server_key_ref;
    action_header.nonce_hex = RepeatHex('2', 24);
    const auto accepted_action = server.AcceptDecodedModeAction(action_header, action);
    CHECK_TRUE(accepted_action.ok);
    CHECK_EQ(server.MatchReplaySummary("match-001").mode_action_count, static_cast<std::uint64_t>(0));

    auto mismatched_action = action;
    mismatched_action.action_id = "action-header-mismatch";
    mismatched_action.seq = 3;
    auto mismatched_action_header = action_header;
    mismatched_action_header.seq = 4;
    const auto mismatched_action_result = server.AcceptDecodedModeAction(
        mismatched_action_header,
        mismatched_action
    );
    CHECK_TRUE(!mismatched_action_result.ok);
    CHECK_EQ(mismatched_action_result.reason, std::string("decoded_mode_action_header_mismatch"));

    auto wrong_action_type_header = action_header;
    wrong_action_type_header.seq = 3;
    wrong_action_type_header.payload_type = phk::battle::BattlePayloadType::Input;
    mismatched_action.seq = 3;
    const auto wrong_action_payload_type = server.AcceptDecodedModeAction(
        wrong_action_type_header,
        mismatched_action
    );
    CHECK_TRUE(!wrong_action_payload_type.ok);
    CHECK_EQ(wrong_action_payload_type.reason, std::string("decoded_mode_action_payload_type_mismatch"));

    CHECK_TRUE(server.AcceptInput(MakeInput("p2", 2, 2, 1u << 2)).ok);
    const auto action_snapshot = server.TickMatch("match-001");
    CHECK_EQ(action_snapshot.snapshot_tick, static_cast<std::uint64_t>(2));
    CHECK_EQ(server.MatchReplaySummary("match-001").mode_action_count, static_cast<std::uint64_t>(1));
    CHECK_EQ(server.MatchReplaySummary("match-001").last_mode_action_id, action.action_id);
    return true;
}

bool TestDecodedAdapterAckAheadDoesNotConsumeSeq() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489605000;
    phk::battle::BattleServer server(config);
    CHECK_TRUE(server.RegisterTicket(MakeTicket()).ok);
    const auto accept = AcceptDefaultHandshake(server);
    CHECK_TRUE(accept.ok);

    phk::battle::DecodedBattlePacket packet;
    packet.encrypted_packet.header.match_id = "match-001";
    packet.encrypted_packet.header.player_id = "p1";
    packet.encrypted_packet.header.tick = 1;
    packet.encrypted_packet.header.seq = 1;
    packet.encrypted_packet.header.ack = 1;
    packet.encrypted_packet.header.payload_type = phk::battle::BattlePayloadType::Input;
    packet.encrypted_packet.header.key_id = accept.client_to_server_key_ref;
    RefreshDevAeadNonce(packet.encrypted_packet.header);
    packet.encrypted_packet.ciphertext = {'p', 'b', ':', 'i', 'n', 'p', 'u', 't'};
    packet.encrypted_packet.auth_tag.assign(16, 0x34);
    packet.decoded_payload_kind = phk::battle::DecodedBattlePayloadKind::Input;
    packet.decoded_input = MakeInput("p1", 1, 1, 1u << 3);

    phk::battle::DecodedBattlePacketAdapter adapter(server);
    const auto ack_ahead = adapter.AcceptDecodedPacket(packet);
    CHECK_TRUE(!ack_ahead.ok);
    CHECK_TRUE(!ack_ahead.encrypted_dispatch_accepted);
    CHECK_EQ(ack_ahead.reason, std::string("encrypted_ack_ahead"));
    CHECK_EQ(server.MatchReplaySummary("match-001").input_count, static_cast<std::uint64_t>(0));

    auto corrected = packet;
    corrected.encrypted_packet.header.ack = 0;
    RefreshDevAeadNonce(corrected.encrypted_packet.header);
    const auto accepted = adapter.AcceptDecodedPacket(corrected);
    CHECK_TRUE(accepted.ok);
    CHECK_TRUE(accepted.encrypted_dispatch_accepted);
    CHECK_EQ(accepted.dispatch.response_kind, std::string("input"));
    CHECK_EQ(accepted.decoded.reason, std::string("ok"));
    CHECK_EQ(server.MatchReplaySummary("match-001").input_count, static_cast<std::uint64_t>(1));
    return true;
}

bool TestDecodedBattlePacketAdapterBoundary() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489605000;
    phk::battle::BattleServer server(config);
    CHECK_TRUE(server.RegisterTicket(MakeTicket()).ok);
    CHECK_TRUE(server.RegisterTicket(MakeTicketForBob()).ok);
    const auto accept = AcceptDefaultHandshake(server);
    CHECK_TRUE(accept.ok);
    const auto bob_accept = AcceptHandshakeForTicket(server, MakeTicketForBob());
    CHECK_TRUE(bob_accept.ok);

    phk::battle::DecodedBattlePacketAdapter adapter(server);
    phk::battle::DecodedBattlePacket input_packet;
    input_packet.encrypted_packet.header.match_id = "match-001";
    input_packet.encrypted_packet.header.player_id = "p1";
    input_packet.encrypted_packet.header.tick = 1;
    input_packet.encrypted_packet.header.seq = 1;
    input_packet.encrypted_packet.header.ack = 0;
    input_packet.encrypted_packet.header.payload_type = phk::battle::BattlePayloadType::Input;
    input_packet.encrypted_packet.header.key_id = accept.client_to_server_key_ref;
    RefreshDevAeadNonce(input_packet.encrypted_packet.header);
    input_packet.encrypted_packet.ciphertext = {'p', 'b', ':', 'i', 'n', 'p', 'u', 't'};
    input_packet.encrypted_packet.auth_tag.assign(16, 0x34);
    input_packet.decoded_payload_kind = phk::battle::DecodedBattlePayloadKind::Input;
    input_packet.decoded_input = MakeInput("p1", 1, 1, 1u << 3);

    const auto accepted_input = adapter.AcceptDecodedPacket(input_packet);
    CHECK_TRUE(accepted_input.ok);
    CHECK_TRUE(accepted_input.encrypted_dispatch_accepted);
    CHECK_EQ(accepted_input.dispatch.response_kind, std::string("input"));
    CHECK_EQ(accepted_input.decoded.reason, std::string("ok"));
    CHECK_EQ(server.MatchReplaySummary("match-001").input_count, static_cast<std::uint64_t>(1));

    auto missing_decoded_input = input_packet;
    missing_decoded_input.encrypted_packet.header.seq = 2;
    missing_decoded_input.encrypted_packet.header.tick = 2;
    RefreshDevAeadNonce(missing_decoded_input.encrypted_packet.header);
    missing_decoded_input.decoded_payload_kind = phk::battle::DecodedBattlePayloadKind::None;
    const auto missing_decoded_input_result = adapter.AcceptDecodedPacket(missing_decoded_input);
    CHECK_TRUE(!missing_decoded_input_result.ok);
    CHECK_TRUE(!missing_decoded_input_result.dispatch.ok);
    CHECK_TRUE(!missing_decoded_input_result.encrypted_dispatch_accepted);
    CHECK_EQ(missing_decoded_input_result.reason, std::string("decoded_packet_input_missing"));
    CHECK_EQ(server.MatchReplaySummary("match-001").input_count, static_cast<std::uint64_t>(1));

    auto retried_decoded_input = missing_decoded_input;
    retried_decoded_input.decoded_payload_kind = phk::battle::DecodedBattlePayloadKind::Input;
    retried_decoded_input.decoded_input = MakeInput("p1", 2, 2, 1u << 0);
    const auto retried_decoded_input_result = adapter.AcceptDecodedPacket(retried_decoded_input);
    CHECK_TRUE(retried_decoded_input_result.ok);
    CHECK_TRUE(retried_decoded_input_result.encrypted_dispatch_accepted);
    CHECK_EQ(retried_decoded_input_result.dispatch.response_kind, std::string("input"));
    CHECK_EQ(server.MatchReplaySummary("match-001").input_count, static_cast<std::uint64_t>(2));

    auto wrong_payload = input_packet;
    wrong_payload.encrypted_packet.header.player_id = "p2";
    wrong_payload.encrypted_packet.header.key_id = bob_accept.client_to_server_key_ref;
    wrong_payload.encrypted_packet.header.seq = 1;
    wrong_payload.encrypted_packet.header.tick = 1;
    RefreshDevAeadNonce(wrong_payload.encrypted_packet.header);
    wrong_payload.decoded_payload_kind = phk::battle::DecodedBattlePayloadKind::Input;
    wrong_payload.decoded_input = MakeInput("p1", 1, 1, 1u << 3);
    const auto wrong_payload_result = adapter.AcceptDecodedPacket(wrong_payload);
    CHECK_TRUE(!wrong_payload_result.ok);
    CHECK_TRUE(!wrong_payload_result.dispatch.ok);
    CHECK_TRUE(!wrong_payload_result.encrypted_dispatch_accepted);
    CHECK_EQ(wrong_payload_result.reason, std::string("decoded_input_header_mismatch"));
    CHECK_EQ(server.MatchReplaySummary("match-001").input_count, static_cast<std::uint64_t>(2));

    auto p2_input = input_packet;
    p2_input.encrypted_packet.header.player_id = "p2";
    p2_input.encrypted_packet.header.key_id = bob_accept.client_to_server_key_ref;
    p2_input.encrypted_packet.header.seq = 1;
    p2_input.encrypted_packet.header.tick = 1;
    RefreshDevAeadNonce(p2_input.encrypted_packet.header);
    p2_input.decoded_payload_kind = phk::battle::DecodedBattlePayloadKind::Input;
    p2_input.decoded_input = MakeInput("p2", 1, 1, 1u << 2);
    const auto accepted_p2_input = adapter.AcceptDecodedPacket(p2_input);
    CHECK_TRUE(accepted_p2_input.ok);
    CHECK_TRUE(accepted_p2_input.encrypted_dispatch_accepted);
    CHECK_EQ(server.MatchReplaySummary("match-001").input_count, static_cast<std::uint64_t>(3));
    CHECK_EQ(server.TickMatch("match-001").snapshot_tick, static_cast<std::uint64_t>(1));

    phk::battle::DecodedBattlePacket action_packet;
    action_packet.encrypted_packet.header.match_id = "match-001";
    action_packet.encrypted_packet.header.player_id = "p1";
    action_packet.encrypted_packet.header.tick = 2;
    action_packet.encrypted_packet.header.seq = 3;
    action_packet.encrypted_packet.header.ack = 1;
    action_packet.encrypted_packet.header.payload_type = phk::battle::BattlePayloadType::ModeAction;
    action_packet.encrypted_packet.header.key_id = accept.client_to_server_key_ref;
    RefreshDevAeadNonce(action_packet.encrypted_packet.header);
    action_packet.encrypted_packet.ciphertext = {'p', 'b', ':', 'm', 'o', 'd', 'e'};
    action_packet.encrypted_packet.auth_tag.assign(16, 0x35);
    action_packet.decoded_payload_kind = phk::battle::DecodedBattlePayloadKind::ModeAction;
    action_packet.decoded_mode_action = MakeModeAction(3);
    action_packet.decoded_mode_action.tick = 2;
    const auto accepted_action = adapter.AcceptDecodedPacket(action_packet);
    CHECK_TRUE(accepted_action.ok);
    CHECK_TRUE(accepted_action.encrypted_dispatch_accepted);
    CHECK_EQ(accepted_action.dispatch.response_kind, std::string("mode_action"));
    CHECK_EQ(server.MatchReplaySummary("match-001").mode_action_count, static_cast<std::uint64_t>(0));

    auto missing_decoded_action = action_packet;
    missing_decoded_action.encrypted_packet.header.seq = 4;
    RefreshDevAeadNonce(missing_decoded_action.encrypted_packet.header);
    missing_decoded_action.decoded_payload_kind = phk::battle::DecodedBattlePayloadKind::None;
    const auto missing_decoded_action_result = adapter.AcceptDecodedPacket(missing_decoded_action);
    CHECK_TRUE(!missing_decoded_action_result.ok);
    CHECK_TRUE(!missing_decoded_action_result.dispatch.ok);
    CHECK_TRUE(!missing_decoded_action_result.encrypted_dispatch_accepted);
    CHECK_EQ(missing_decoded_action_result.reason, std::string("decoded_packet_mode_action_missing"));
    CHECK_EQ(server.MatchReplaySummary("match-001").mode_action_count, static_cast<std::uint64_t>(0));

    auto retried_decoded_action = missing_decoded_action;
    retried_decoded_action.decoded_payload_kind = phk::battle::DecodedBattlePayloadKind::ModeAction;
    retried_decoded_action.decoded_mode_action = MakeModeAction(4);
    retried_decoded_action.decoded_mode_action.tick = 2;
    retried_decoded_action.decoded_mode_action.action_id = "action-retried-decoded";
    const auto retried_decoded_action_result = adapter.AcceptDecodedPacket(retried_decoded_action);
    CHECK_TRUE(retried_decoded_action_result.ok);
    CHECK_TRUE(retried_decoded_action_result.encrypted_dispatch_accepted);
    CHECK_EQ(retried_decoded_action_result.dispatch.response_kind, std::string("mode_action"));
    CHECK_EQ(server.MatchReplaySummary("match-001").mode_action_count, static_cast<std::uint64_t>(0));

    auto ping_packet = input_packet;
    ping_packet.encrypted_packet.header.player_id = "p2";
    ping_packet.encrypted_packet.header.key_id = bob_accept.client_to_server_key_ref;
    ping_packet.encrypted_packet.header.payload_type = phk::battle::BattlePayloadType::Ping;
    ping_packet.encrypted_packet.header.seq = 3;
    ping_packet.encrypted_packet.header.tick = 1;
    ping_packet.encrypted_packet.header.ack = 1;
    RefreshDevAeadNonce(ping_packet.encrypted_packet.header);
    ping_packet.decoded_payload_kind = phk::battle::DecodedBattlePayloadKind::Input;
    const auto ping_decode_result = adapter.AcceptDecodedPacket(ping_packet);
    CHECK_TRUE(!ping_decode_result.ok);
    CHECK_TRUE(!ping_decode_result.dispatch.ok);
    CHECK_TRUE(!ping_decode_result.encrypted_dispatch_accepted);
    CHECK_EQ(ping_decode_result.reason, std::string("decoded_packet_payload_type_unsupported"));

    CHECK_TRUE(server.SetPlayerConnected("match-001", "p2", false).ok);
    const auto reconnect_base_event_count = server.MatchReplaySummary("match-001").event_count;
    phk::battle::DecodedBattlePacket reconnect_packet;
    reconnect_packet.encrypted_packet.header.match_id = "match-001";
    reconnect_packet.encrypted_packet.header.player_id = "p2";
    reconnect_packet.encrypted_packet.header.tick = 2;
    reconnect_packet.encrypted_packet.header.seq = 3;
    reconnect_packet.encrypted_packet.header.ack = reconnect_base_event_count;
    reconnect_packet.encrypted_packet.header.payload_type = phk::battle::BattlePayloadType::Reconnect;
    reconnect_packet.encrypted_packet.header.key_id = bob_accept.client_to_server_key_ref;
    RefreshDevAeadNonce(reconnect_packet.encrypted_packet.header);
    reconnect_packet.encrypted_packet.ciphertext = {'p', 'b', ':', 'r', 'e', 'c', 'o', 'n'};
    reconnect_packet.encrypted_packet.auth_tag.assign(16, 0x36);
    reconnect_packet.decoded_payload_kind = phk::battle::DecodedBattlePayloadKind::ModeAction;
    reconnect_packet.decoded_mode_action = MakeModeAction(3);
    reconnect_packet.decoded_mode_action.player_id = "p2";
    reconnect_packet.decoded_mode_action.tick = 2;
    reconnect_packet.decoded_mode_action.action_id = "action-decoded-reconnect-p2";
    reconnect_packet.decoded_mode_action.action_type = "reconnect";
    reconnect_packet.decoded_mode_action.payload_json =
        "{\"last_seen_event_cursor\":" + std::to_string(reconnect_base_event_count) + "}";

    auto missing_decoded_reconnect = reconnect_packet;
    missing_decoded_reconnect.encrypted_packet.header.seq = 4;
    RefreshDevAeadNonce(missing_decoded_reconnect.encrypted_packet.header);
    missing_decoded_reconnect.decoded_payload_kind = phk::battle::DecodedBattlePayloadKind::None;
    const auto missing_decoded_reconnect_result = adapter.AcceptDecodedPacket(missing_decoded_reconnect);
    CHECK_TRUE(!missing_decoded_reconnect_result.ok);
    CHECK_TRUE(!missing_decoded_reconnect_result.dispatch.ok);
    CHECK_TRUE(!missing_decoded_reconnect_result.encrypted_dispatch_accepted);
    CHECK_EQ(missing_decoded_reconnect_result.reason, std::string("decoded_packet_reconnect_missing"));
    CHECK_TRUE(!server.IsPlayerConnected("match-001", "p2"));

    auto mismatched_reconnect = reconnect_packet;
    mismatched_reconnect.encrypted_packet.header.seq = 4;
    RefreshDevAeadNonce(mismatched_reconnect.encrypted_packet.header);
    mismatched_reconnect.decoded_mode_action.seq = 5;
    const auto mismatched_reconnect_result = adapter.AcceptDecodedPacket(mismatched_reconnect);
    CHECK_TRUE(!mismatched_reconnect_result.ok);
    CHECK_TRUE(!mismatched_reconnect_result.dispatch.ok);
    CHECK_TRUE(!mismatched_reconnect_result.encrypted_dispatch_accepted);
    CHECK_EQ(mismatched_reconnect_result.reason, std::string("decoded_reconnect_header_mismatch"));
    CHECK_TRUE(!server.IsPlayerConnected("match-001", "p2"));

    auto wrong_reconnect_type = reconnect_packet;
    wrong_reconnect_type.encrypted_packet.header.seq = 4;
    RefreshDevAeadNonce(wrong_reconnect_type.encrypted_packet.header);
    wrong_reconnect_type.decoded_mode_action.seq = 4;
    wrong_reconnect_type.decoded_mode_action.action_type = "ready";
    const auto wrong_reconnect_type_result = adapter.AcceptDecodedPacket(wrong_reconnect_type);
    CHECK_TRUE(!wrong_reconnect_type_result.ok);
    CHECK_TRUE(!wrong_reconnect_type_result.dispatch.ok);
    CHECK_TRUE(!wrong_reconnect_type_result.encrypted_dispatch_accepted);
    CHECK_EQ(wrong_reconnect_type_result.reason, std::string("decoded_reconnect_header_mismatch"));
    CHECK_TRUE(!server.IsPlayerConnected("match-001", "p2"));

    auto missing_reconnect_cursor = reconnect_packet;
    missing_reconnect_cursor.encrypted_packet.header.seq = 4;
    RefreshDevAeadNonce(missing_reconnect_cursor.encrypted_packet.header);
    missing_reconnect_cursor.decoded_mode_action.seq = 4;
    missing_reconnect_cursor.decoded_mode_action.payload_json = "{\"source\":\"client-reconnect\"}";
    const auto missing_reconnect_cursor_result = adapter.AcceptDecodedPacket(missing_reconnect_cursor);
    CHECK_TRUE(!missing_reconnect_cursor_result.ok);
    CHECK_TRUE(!missing_reconnect_cursor_result.dispatch.ok);
    CHECK_TRUE(!missing_reconnect_cursor_result.encrypted_dispatch_accepted);
    CHECK_EQ(missing_reconnect_cursor_result.reason, std::string("decoded_reconnect_cursor_missing"));
    CHECK_TRUE(!server.IsPlayerConnected("match-001", "p2"));

    auto mismatched_reconnect_cursor = reconnect_packet;
    mismatched_reconnect_cursor.encrypted_packet.header.seq = 4;
    RefreshDevAeadNonce(mismatched_reconnect_cursor.encrypted_packet.header);
    mismatched_reconnect_cursor.decoded_mode_action.seq = 4;
    mismatched_reconnect_cursor.decoded_mode_action.payload_json =
        "{\"last_seen_event_cursor\":" + std::to_string(reconnect_base_event_count + 1) + "}";
    const auto mismatched_reconnect_cursor_result = adapter.AcceptDecodedPacket(mismatched_reconnect_cursor);
    CHECK_TRUE(!mismatched_reconnect_cursor_result.ok);
    CHECK_TRUE(!mismatched_reconnect_cursor_result.dispatch.ok);
    CHECK_TRUE(!mismatched_reconnect_cursor_result.encrypted_dispatch_accepted);
    CHECK_EQ(mismatched_reconnect_cursor_result.reason, std::string("decoded_reconnect_cursor_mismatch"));
    CHECK_TRUE(!server.IsPlayerConnected("match-001", "p2"));

    auto reconnect_cursor_ahead = reconnect_packet;
    reconnect_cursor_ahead.encrypted_packet.header.seq = 4;
    reconnect_cursor_ahead.encrypted_packet.header.ack = reconnect_base_event_count + 1;
    RefreshDevAeadNonce(reconnect_cursor_ahead.encrypted_packet.header);
    reconnect_cursor_ahead.decoded_mode_action.seq = 4;
    reconnect_cursor_ahead.decoded_mode_action.payload_json =
        "{\"last_seen_event_cursor\":" + std::to_string(reconnect_base_event_count + 1) + "}";
    const auto reconnect_cursor_ahead_result = adapter.AcceptDecodedPacket(reconnect_cursor_ahead);
    CHECK_TRUE(!reconnect_cursor_ahead_result.ok);
    CHECK_TRUE(!reconnect_cursor_ahead_result.encrypted_dispatch_accepted);
    CHECK_EQ(reconnect_cursor_ahead_result.reason, std::string("encrypted_event_cursor_ahead"));
    CHECK_TRUE(!server.IsPlayerConnected("match-001", "p2"));

    auto accepted_reconnect = reconnect_packet;
    accepted_reconnect.encrypted_packet.header.seq = 4;
    RefreshDevAeadNonce(accepted_reconnect.encrypted_packet.header);
    accepted_reconnect.decoded_mode_action.seq = 4;
    const auto accepted_reconnect_result = adapter.AcceptDecodedPacket(accepted_reconnect);
    CHECK_TRUE(accepted_reconnect_result.ok);
    CHECK_TRUE(accepted_reconnect_result.encrypted_dispatch_accepted);
    CHECK_EQ(accepted_reconnect_result.dispatch.response_kind, std::string("reconnect"));
    CHECK_EQ(accepted_reconnect_result.decoded.reason, std::string("ok"));
    CHECK_TRUE(server.IsPlayerConnected("match-001", "p2"));
    const auto reconnect_snapshot = server.ReconnectSnapshot("match-001", "p2", reconnect_base_event_count);
    CHECK_EQ(reconnect_snapshot.snapshot_kind, std::string("reconnect"));
    CHECK_EQ(reconnect_snapshot.mode_state.at("reconnect_player_id"), std::string("p2"));

    auto connected_reconnect = reconnect_packet;
    connected_reconnect.encrypted_packet.header.seq = 5;
    RefreshDevAeadNonce(connected_reconnect.encrypted_packet.header);
    connected_reconnect.decoded_mode_action.seq = 5;
    connected_reconnect.decoded_mode_action.action_id = "action-decoded-reconnect-p2-connected";
    const auto connected_reconnect_result = adapter.AcceptDecodedPacket(connected_reconnect);
    CHECK_TRUE(!connected_reconnect_result.ok);
    CHECK_TRUE(!connected_reconnect_result.encrypted_dispatch_accepted);
    CHECK_EQ(connected_reconnect_result.reason, std::string("encrypted_reconnect_player_connected"));
    CHECK_TRUE(server.IsPlayerConnected("match-001", "p2"));

    auto invalid_dispatch = input_packet;
    invalid_dispatch.encrypted_packet.header.player_id = "p2";
    invalid_dispatch.encrypted_packet.header.key_id = "wrong-session-key";
    invalid_dispatch.encrypted_packet.header.seq = 5;
    invalid_dispatch.encrypted_packet.header.tick = 2;
    RefreshDevAeadNonce(invalid_dispatch.encrypted_packet.header);
    invalid_dispatch.decoded_payload_kind = phk::battle::DecodedBattlePayloadKind::Input;
    invalid_dispatch.decoded_input = MakeInput("p2", 2, 5, 1u << 2);
    const auto invalid_dispatch_result = adapter.AcceptDecodedPacket(invalid_dispatch);
    CHECK_TRUE(!invalid_dispatch_result.ok);
    CHECK_TRUE(!invalid_dispatch_result.dispatch.ok);
    CHECK_TRUE(!invalid_dispatch_result.encrypted_dispatch_accepted);
    CHECK_EQ(invalid_dispatch_result.reason, std::string("session_key_mismatch"));
    CHECK_EQ(server.MatchReplaySummary("match-001").input_count, static_cast<std::uint64_t>(3));

    phk::battle::BattleServer no_handshake_server(config);
    CHECK_TRUE(no_handshake_server.RegisterTicket(MakeTicket()).ok);
    phk::battle::DecodedBattlePacketAdapter no_handshake_adapter(no_handshake_server);
    auto no_handshake_packet = input_packet;
    no_handshake_packet.encrypted_packet.header.key_id = config.signing_key_id;
    RefreshDevAeadNonce(no_handshake_packet.encrypted_packet.header);
    const auto no_handshake_result = no_handshake_adapter.AcceptDecodedPacket(no_handshake_packet);
    CHECK_TRUE(!no_handshake_result.ok);
    CHECK_TRUE(!no_handshake_result.dispatch.ok);
    CHECK_TRUE(!no_handshake_result.encrypted_dispatch_accepted);
    CHECK_EQ(no_handshake_result.reason, std::string("handshake_required"));
    CHECK_EQ(no_handshake_server.MatchReplaySummary("match-001").input_count, static_cast<std::uint64_t>(0));

    const auto action_snapshot = server.TickMatch("match-001");
    CHECK_EQ(action_snapshot.snapshot_tick, static_cast<std::uint64_t>(2));
    CHECK_TRUE(server.IsPlayerConnected("match-001", "p2"));
    CHECK_EQ(server.MatchReplaySummary("match-001").mode_action_count, static_cast<std::uint64_t>(3));
    CHECK_EQ(server.MatchReplaySummary("match-001").last_mode_action_id, accepted_reconnect.decoded_mode_action.action_id);
    CHECK_EQ(server.MatchReplaySummary("match-001").last_mode_action_type, std::string("reconnect"));
    return true;
}

bool TestKcpPlaceholder() {
    phk::battle::KcpEchoEndpoint endpoint;
    phk::battle::UdpDatagram datagram;
    datagram.remote_endpoint = "127.0.0.1:52000";
    datagram.payload = {'h', 'e', 'l', 'l', 'o'};
    const auto replies = endpoint.ProcessDatagram(datagram);
    CHECK_EQ(replies.size(), static_cast<std::size_t>(1));
    CHECK_EQ(replies[0].remote_endpoint, datagram.remote_endpoint);
    CHECK_TRUE(replies[0].payload.size() > datagram.payload.size());
    CHECK_EQ(endpoint.Stats().datagrams_in, static_cast<std::uint64_t>(1));
    CHECK_EQ(endpoint.Stats().datagrams_out, static_cast<std::uint64_t>(1));
    return true;
}

bool TestKcpAeadPacketAdapterBoundary() {
    phk::battle::BattleServerConfig config;
    config.now_ms = 1782489605000;
    phk::battle::BattleServer server(config);
    CHECK_TRUE(server.RegisterTicket(MakeTicket()).ok);
    const auto accept = AcceptDefaultHandshake(server);
    CHECK_TRUE(accept.ok);

    phk::battle::KcpEchoEndpoint endpoint;
    phk::battle::KcpAeadPacketAdapter adapter(server, endpoint);

    phk::battle::BattleEncryptedPacket packet;
    packet.header.match_id = "match-001";
    packet.header.player_id = "p1";
    packet.header.tick = 1;
    packet.header.seq = 1;
    packet.header.payload_type = phk::battle::BattlePayloadType::Input;
    packet.header.key_id = accept.client_to_server_key_ref;
    RefreshDevAeadNonce(packet.header);
    packet.ciphertext = {'i', 'n', 'p', 'u', 't'};
    packet.auth_tag.assign(16, 0x33);

    phk::battle::UdpDatagram datagram;
    datagram.remote_endpoint = "127.0.0.1:52001";
    datagram.payload = {'k', 'c', 'p', ':', 'i', 'n', 'p', 'u', 't'};

    auto wrong_key = packet;
    wrong_key.header.key_id = "wrong-key";
    RefreshDevAeadNonce(wrong_key.header);
    const auto rejected = adapter.ProcessEncryptedDatagram(wrong_key, datagram);
    CHECK_TRUE(!rejected.ok);
    CHECK_EQ(rejected.reason, std::string("session_key_mismatch"));
    CHECK_TRUE(rejected.replies.empty());
    CHECK_EQ(adapter.Stats().accepted_datagrams, static_cast<std::uint64_t>(0));
    CHECK_EQ(adapter.Stats().rejected_datagrams, static_cast<std::uint64_t>(1));
    CHECK_EQ(adapter.Stats().malformed_datagrams, static_cast<std::uint64_t>(0));
    CHECK_EQ(adapter.Stats().remote_endpoint_mismatches, static_cast<std::uint64_t>(0));
    CHECK_EQ(adapter.Stats().remote_endpoint_rebinds, static_cast<std::uint64_t>(0));
    CHECK_EQ(adapter.Stats().bound_sessions, static_cast<std::uint64_t>(0));
    CHECK_EQ(endpoint.Stats().datagrams_in, static_cast<std::uint64_t>(0));
    CHECK_EQ(endpoint.Stats().datagrams_out, static_cast<std::uint64_t>(0));

    auto missing_remote = datagram;
    missing_remote.remote_endpoint.clear();
    const auto missing_remote_result = adapter.ProcessEncryptedDatagram(packet, missing_remote);
    CHECK_TRUE(!missing_remote_result.ok);
    CHECK_EQ(missing_remote_result.reason, std::string("remote_endpoint_missing"));
    CHECK_TRUE(missing_remote_result.replies.empty());
    CHECK_EQ(adapter.Stats().accepted_datagrams, static_cast<std::uint64_t>(0));
    CHECK_EQ(adapter.Stats().rejected_datagrams, static_cast<std::uint64_t>(2));
    CHECK_EQ(adapter.Stats().malformed_datagrams, static_cast<std::uint64_t>(1));
    CHECK_EQ(adapter.Stats().bound_sessions, static_cast<std::uint64_t>(0));
    CHECK_EQ(endpoint.Stats().datagrams_in, static_cast<std::uint64_t>(0));
    CHECK_EQ(endpoint.Stats().datagrams_out, static_cast<std::uint64_t>(0));

    auto missing_payload = datagram;
    missing_payload.payload.clear();
    const auto missing_payload_result = adapter.ProcessEncryptedDatagram(packet, missing_payload);
    CHECK_TRUE(!missing_payload_result.ok);
    CHECK_EQ(missing_payload_result.reason, std::string("datagram_payload_missing"));
    CHECK_TRUE(missing_payload_result.replies.empty());
    CHECK_EQ(adapter.Stats().accepted_datagrams, static_cast<std::uint64_t>(0));
    CHECK_EQ(adapter.Stats().rejected_datagrams, static_cast<std::uint64_t>(3));
    CHECK_EQ(adapter.Stats().malformed_datagrams, static_cast<std::uint64_t>(2));
    CHECK_EQ(adapter.Stats().bound_sessions, static_cast<std::uint64_t>(0));
    CHECK_EQ(endpoint.Stats().datagrams_in, static_cast<std::uint64_t>(0));
    CHECK_EQ(endpoint.Stats().datagrams_out, static_cast<std::uint64_t>(0));

    const auto accepted = adapter.ProcessEncryptedDatagram(packet, datagram);
    CHECK_TRUE(accepted.ok);
    CHECK_EQ(accepted.dispatch.response_kind, std::string("input"));
    CHECK_EQ(accepted.reason, std::string("input"));
    CHECK_EQ(accepted.replies.size(), static_cast<std::size_t>(1));
    CHECK_EQ(accepted.replies[0].remote_endpoint, datagram.remote_endpoint);
    CHECK_TRUE(accepted.replies[0].payload.size() > datagram.payload.size());
    CHECK_EQ(adapter.Stats().accepted_datagrams, static_cast<std::uint64_t>(1));
    CHECK_EQ(adapter.Stats().rejected_datagrams, static_cast<std::uint64_t>(3));
    CHECK_EQ(adapter.Stats().malformed_datagrams, static_cast<std::uint64_t>(2));
    CHECK_EQ(adapter.Stats().bound_sessions, static_cast<std::uint64_t>(1));
    CHECK_EQ(endpoint.Stats().datagrams_in, static_cast<std::uint64_t>(1));
    CHECK_EQ(endpoint.Stats().datagrams_out, static_cast<std::uint64_t>(1));

    auto remote_mismatch = packet;
    remote_mismatch.header.seq = 2;
    RefreshDevAeadNonce(remote_mismatch.header);
    auto other_remote = datagram;
    other_remote.remote_endpoint = "127.0.0.1:52002";
    const auto remote_rejected = adapter.ProcessEncryptedDatagram(remote_mismatch, other_remote);
    CHECK_TRUE(!remote_rejected.ok);
    CHECK_EQ(remote_rejected.reason, std::string("remote_endpoint_mismatch"));
    CHECK_TRUE(remote_rejected.replies.empty());
    CHECK_EQ(adapter.Stats().accepted_datagrams, static_cast<std::uint64_t>(1));
    CHECK_EQ(adapter.Stats().rejected_datagrams, static_cast<std::uint64_t>(4));
    CHECK_EQ(adapter.Stats().malformed_datagrams, static_cast<std::uint64_t>(2));
    CHECK_EQ(adapter.Stats().remote_endpoint_mismatches, static_cast<std::uint64_t>(1));
    CHECK_EQ(adapter.Stats().remote_endpoint_rebinds, static_cast<std::uint64_t>(0));
    CHECK_EQ(endpoint.Stats().datagrams_in, static_cast<std::uint64_t>(1));
    CHECK_EQ(endpoint.Stats().datagrams_out, static_cast<std::uint64_t>(1));

    const auto remote_retry = adapter.ProcessEncryptedDatagram(remote_mismatch, datagram);
    CHECK_TRUE(remote_retry.ok);
    CHECK_EQ(remote_retry.reason, std::string("input"));
    CHECK_EQ(endpoint.Stats().datagrams_in, static_cast<std::uint64_t>(2));
    CHECK_EQ(endpoint.Stats().datagrams_out, static_cast<std::uint64_t>(2));

    auto reconnect = packet;
    reconnect.header.payload_type = phk::battle::BattlePayloadType::Reconnect;
    reconnect.header.seq = 3;
    RefreshDevAeadNonce(reconnect.header);
    const auto connected_rebind_result = adapter.ProcessEncryptedDatagram(reconnect, other_remote);
    CHECK_TRUE(!connected_rebind_result.ok);
    CHECK_EQ(connected_rebind_result.reason, std::string("remote_rebind_player_connected"));
    CHECK_TRUE(connected_rebind_result.replies.empty());
    CHECK_EQ(adapter.Stats().accepted_datagrams, static_cast<std::uint64_t>(2));
    CHECK_EQ(adapter.Stats().rejected_datagrams, static_cast<std::uint64_t>(5));
    CHECK_EQ(adapter.Stats().malformed_datagrams, static_cast<std::uint64_t>(2));
    CHECK_EQ(adapter.Stats().remote_endpoint_mismatches, static_cast<std::uint64_t>(2));
    CHECK_EQ(adapter.Stats().remote_endpoint_rebinds, static_cast<std::uint64_t>(0));
    CHECK_EQ(adapter.Stats().bound_sessions, static_cast<std::uint64_t>(1));
    CHECK_EQ(endpoint.Stats().datagrams_in, static_cast<std::uint64_t>(2));
    CHECK_EQ(endpoint.Stats().datagrams_out, static_cast<std::uint64_t>(2));

    CHECK_TRUE(server.SetPlayerConnected("match-001", "p1", false).ok);
    const auto reconnect_result = adapter.ProcessEncryptedDatagram(reconnect, other_remote);
    CHECK_TRUE(reconnect_result.ok);
    CHECK_EQ(reconnect_result.reason, std::string("reconnect"));
    CHECK_EQ(adapter.Stats().accepted_datagrams, static_cast<std::uint64_t>(3));
    CHECK_EQ(adapter.Stats().rejected_datagrams, static_cast<std::uint64_t>(5));
    CHECK_EQ(adapter.Stats().malformed_datagrams, static_cast<std::uint64_t>(2));
    CHECK_EQ(adapter.Stats().remote_endpoint_mismatches, static_cast<std::uint64_t>(2));
    CHECK_EQ(adapter.Stats().remote_endpoint_rebinds, static_cast<std::uint64_t>(1));
    CHECK_EQ(adapter.Stats().bound_sessions, static_cast<std::uint64_t>(1));
    CHECK_EQ(endpoint.Stats().datagrams_in, static_cast<std::uint64_t>(3));
    CHECK_EQ(endpoint.Stats().datagrams_out, static_cast<std::uint64_t>(3));

    CHECK_TRUE(server.SetPlayerConnected("match-001", "p1", true).ok);
    auto old_remote_after_reconnect = packet;
    old_remote_after_reconnect.header.seq = 4;
    RefreshDevAeadNonce(old_remote_after_reconnect.header);
    const auto old_remote_result = adapter.ProcessEncryptedDatagram(old_remote_after_reconnect, datagram);
    CHECK_TRUE(!old_remote_result.ok);
    CHECK_EQ(old_remote_result.reason, std::string("remote_endpoint_mismatch"));
    CHECK_TRUE(old_remote_result.replies.empty());
    CHECK_EQ(adapter.Stats().accepted_datagrams, static_cast<std::uint64_t>(3));
    CHECK_EQ(adapter.Stats().rejected_datagrams, static_cast<std::uint64_t>(6));
    CHECK_EQ(adapter.Stats().malformed_datagrams, static_cast<std::uint64_t>(2));
    CHECK_EQ(adapter.Stats().remote_endpoint_mismatches, static_cast<std::uint64_t>(3));
    CHECK_EQ(adapter.Stats().remote_endpoint_rebinds, static_cast<std::uint64_t>(1));
    CHECK_EQ(endpoint.Stats().datagrams_in, static_cast<std::uint64_t>(3));
    CHECK_EQ(endpoint.Stats().datagrams_out, static_cast<std::uint64_t>(3));

    const auto new_remote_result = adapter.ProcessEncryptedDatagram(old_remote_after_reconnect, other_remote);
    CHECK_TRUE(new_remote_result.ok);
    CHECK_EQ(new_remote_result.reason, std::string("input"));
    CHECK_EQ(adapter.Stats().accepted_datagrams, static_cast<std::uint64_t>(4));
    CHECK_EQ(adapter.Stats().rejected_datagrams, static_cast<std::uint64_t>(6));
    CHECK_EQ(adapter.Stats().malformed_datagrams, static_cast<std::uint64_t>(2));
    CHECK_EQ(endpoint.Stats().datagrams_in, static_cast<std::uint64_t>(4));
    CHECK_EQ(endpoint.Stats().datagrams_out, static_cast<std::uint64_t>(4));

    auto nonce_replay = old_remote_after_reconnect;
    nonce_replay.header.seq = 5;
    const auto replay = adapter.ProcessEncryptedDatagram(nonce_replay, other_remote);
    CHECK_TRUE(!replay.ok);
    CHECK_EQ(replay.reason, std::string("nonce_replay"));
    CHECK_TRUE(replay.replies.empty());
    CHECK_EQ(adapter.Stats().accepted_datagrams, static_cast<std::uint64_t>(4));
    CHECK_EQ(adapter.Stats().rejected_datagrams, static_cast<std::uint64_t>(7));
    CHECK_EQ(adapter.Stats().malformed_datagrams, static_cast<std::uint64_t>(2));
    CHECK_EQ(adapter.Stats().remote_endpoint_mismatches, static_cast<std::uint64_t>(3));
    CHECK_EQ(adapter.Stats().remote_endpoint_rebinds, static_cast<std::uint64_t>(1));
    CHECK_EQ(adapter.Stats().bound_sessions, static_cast<std::uint64_t>(1));
    CHECK_EQ(endpoint.Stats().datagrams_in, static_cast<std::uint64_t>(4));
    CHECK_EQ(endpoint.Stats().datagrams_out, static_cast<std::uint64_t>(4));
    return true;
}

}  // namespace

int main() {
	const std::vector<std::pair<std::string, bool (*)()>> tests = {
		{"ProtocolManifest", TestProtocolManifest},
		{"SharedSnapshotEventFixtures", TestSharedSnapshotEventFixtures},
		{"GoldenReplaySummaryFixture", TestGoldenReplaySummaryFixture},
		{"TicketVerifier", TestTicketVerifier},
		{"ServerAndHandshake", TestServerAndHandshake},
        {"HandshakeTranscriptBindsFullClientMaterial", TestHandshakeTranscriptBindsFullClientMaterial},
        {"RoomCapacityGuard", TestRoomCapacityGuard},
		{"BattleResultSubmission", TestBattleResultSubmission},
		{"BuildSignedBattleResultCallback", TestBuildSignedBattleResultCallback},
		{"SimulationDeterminism", TestSimulationDeterminism},
		{"FallbackInputReplayAudit", TestFallbackInputReplayAudit},
        {"ReadyModeActionLifecycleState", TestReadyModeActionLifecycleState},
        {"BattleRoyaleSelectRoundCardPayloadBoundary", TestBattleRoyaleSelectRoundCardPayloadBoundary},
        {"ModeActionPayloadSizeLimit", TestModeActionPayloadSizeLimit},
		{"BossTransferCardValidation", TestBossTransferCardValidation},
		{"BossModeSpawnLayout", TestBossModeSpawnLayout},
        {"BossMatchPreconfiguration", TestBossMatchPreconfiguration},
        {"BossModeCapacityGuard", TestBossModeCapacityGuard},
        {"BossSimulationRejectsNinthPlayer", TestBossSimulationRejectsNinthPlayer},
        {"BossStartReadinessTracksConnectedPlayers", TestBossStartReadinessTracksConnectedPlayers},
        {"BossReadyToStartRequiresAllReadyPlayers", TestBossReadyToStartRequiresAllReadyPlayers},
		{"BossModeBulletPattern", TestBossModeBulletPattern},
        {"BossModeAuthoritativeDamageState", TestBossModeAuthoritativeDamageState},
        {"BossModeResultProjection", TestBossModeResultProjection},
        {"InstanceBossResultStateMutualExclusion", TestInstanceBossResultStateMutualExclusion},
        {"BossModeResultSubmissionRequiresBossProjection", TestBossModeResultSubmissionRequiresBossProjection},
        {"TransferCardAuditIdsRejectEscapedStrings", TestTransferCardAuditIdsRejectEscapedStrings},
        {"BossModeResultRequiresStartableRoom", TestBossModeResultRequiresStartableRoom},
        {"BossRosterLocksAfterReadyToStart", TestBossRosterLocksAfterReadyToStart},
        {"SettledMatchRetirementLifecycle", TestSettledMatchRetirementLifecycle},
        {"UnsettledMatchCancellationLifecycle", TestUnsettledMatchCancellationLifecycle},
		{"AuthoritativeReplay60TickFixture", TestAuthoritativeReplay60TickFixture},
		{"ReplayFixtureBoundary", TestReplayFixtureBoundary},
		{"ReplayRecordBridgeBoundary", TestReplayRecordBridgeBoundary},
        {"ResultAndReplayRecordUseStablePlayerOrder", TestResultAndReplayRecordUseStablePlayerOrder},
		{"ServerAuthoritativeInputAndSnapshot", TestServerAuthoritativeInputAndSnapshot},
		{"Dispatcher", TestDispatcher},
		{"EncryptedPacketAdapterShape", TestEncryptedPacketAdapterShape},
		{"ServerEncryptedPacketSessionBoundary", TestServerEncryptedPacketSessionBoundary},
		{"DecodedPayloadHeaderBinding", TestDecodedPayloadHeaderBinding},
        {"DecodedAdapterAckAheadDoesNotConsumeSeq", TestDecodedAdapterAckAheadDoesNotConsumeSeq},
		{"DecodedBattlePacketAdapterBoundary", TestDecodedBattlePacketAdapterBoundary},
		{"KcpPlaceholder", TestKcpPlaceholder},
		{"KcpAeadPacketAdapterBoundary", TestKcpAeadPacketAdapterBoundary},
	};

    for (const auto& test : tests) {
        if (!test.second()) {
            std::cerr << "FAILED " << test.first << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "ok " << test.first << '\n';
    }
    return EXIT_SUCCESS;
}
