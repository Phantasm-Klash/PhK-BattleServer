#include <cstdlib>
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

std::string ModeResultJsonForSummary(const phk::battle::ReplaySummary& summary) {
    return "{\"battle_result_owner\":\"cpp\",\"event_cursor\":" +
        std::to_string(summary.event_count) +
        ",\"final_tick\":" +
        std::to_string(summary.final_tick) +
        ",\"input_count\":" +
        std::to_string(summary.input_count) +
        ",\"fallback_input_count\":" +
        std::to_string(summary.fallback_input_count) +
        ",\"neutral_fallback_count\":" +
        std::to_string(summary.neutral_fallback_count) +
        ",\"held_input_fallback_count\":" +
        std::to_string(summary.held_input_fallback_count) +
        ",\"mode_action_count\":" +
        std::to_string(summary.mode_action_count) +
        ",\"input_trace_count\":" +
        std::to_string(summary.input_trace.size()) +
        ",\"event_trace_count\":" +
        std::to_string(summary.event_trace.size()) +
        ",\"input_stream_hash\":\"" +
        summary.input_stream_hash +
        "\",\"event_stream_hash\":\"" +
        summary.event_stream_hash +
        "\",\"final_state_hash\":\"" +
        summary.final_state_hash +
        "\"}";
}

std::string ReplaceFirst(std::string value, const std::string& old_value, const std::string& new_value) {
    const auto offset = value.find(old_value);
    if (offset != std::string::npos) {
        value.replace(offset, old_value.size(), new_value);
    }
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

phk::battle::SignedBattleResult MakeBattleResult() {
	phk::battle::SignedBattleResult signed_result;
	signed_result.result.match_id = std::string(phk::v1::kBattleResultCallbackMatchId);
	signed_result.result.mode_id = std::string(phk::v1::kBattleResultCallbackModeId);
	signed_result.result.result_hash = std::string(phk::v1::kBattleResultCallbackResultHash);
	signed_result.result.replay_id = std::string(phk::v1::kBattleResultCallbackReplayId);
	signed_result.result.player_ids = {"p1", "p2"};
	signed_result.result.reward_projection_json = std::string(phk::v1::kBattleResultCallbackRewardProjectionJson);
	signed_result.result.mode_result_json = std::string(phk::v1::kBattleResultCallbackModeResultJson);
	signed_result.result.settled_at_ms = phk::v1::kBattleResultCallbackSettledAtMs;
	signed_result.key_id = std::string(phk::v1::kBattleResultCallbackKeyId);
	signed_result.public_key_hex = std::string(phk::v1::kBattleResultCallbackPublicKeyHex);
	signed_result.signature_hex = std::string(phk::v1::kBattleResultCallbackSignatureHex);
	signed_result.server_authoritative = true;
	return signed_result;
}

phk::battle::SignedBattleResult MakeBattleResultForSummary(const phk::battle::ReplaySummary& summary) {
    auto signed_result = MakeBattleResult();
    signed_result.result.result_hash = ExpectedDevResultHash(summary);
    signed_result.result.replay_id = ExpectedDevReplayId(summary);
    signed_result.result.mode_result_json = ModeResultJsonForSummary(summary);
    return signed_result;
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
    header.nonce_hex = RepeatHex('1', 24);
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
    return true;
}

bool TestProtocolManifest() {
    phk::battle::VersionStamp version;
    CHECK_EQ(version.protocol_version, phk::v1::kProtocolVersion);
    CHECK_EQ(version.business_api_version, std::string(phk::v1::kBusinessApiVersion));
    CHECK_EQ(version.battle_api_version, std::string(phk::v1::kBattleApiVersion));
    CHECK_EQ(version.ruleset_version, std::string(phk::v1::kRulesetVersion));
    CHECK_EQ(std::string(phk::battle::kRulesetHash), std::string(phk::v1::kRulesetHash));
    CHECK_TRUE(phk::v1::HasMessageField("BattleTicket", "ruleset_version"));
    CHECK_TRUE(phk::v1::HasMessageField("BattlePacketHeader", "seq"));
    CHECK_TRUE(phk::v1::HasMessageField("BattlePacketHeader", "nonce"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleHandshakeHello", "client_x25519_pub"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleHandshakeAccept", "server_signature"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleEncryptedPacket", "auth_tag"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleInput", "direction_bits"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleModeAction", "client_result_authoritative"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleSnapshot", "mode_state"));
    CHECK_TRUE(phk::v1::HasMessageField("SignedBattleTicket", "signature"));
    CHECK_TRUE(phk::v1::HasMessageField("SignedBattleResult", "signature"));
    CHECK_TRUE(phk::v1::HasMessageField("ReplayInputStreamSummary", "final_state_hash"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleResult", "result_hash"));
    CHECK_TRUE(phk::v1::MessageFieldCount("BattleHandshakeHello") >= 5);
    CHECK_TRUE(phk::v1::MessageFieldCount("BattleHandshakeAccept") >= 10);
    CHECK_TRUE(phk::v1::MessageFieldCount("BattleInput") >= 10);
    CHECK_TRUE(phk::v1::MessageFieldCount("BattleResult") >= 9);
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
	CHECK_EQ(server.ActiveSessionCount(), static_cast<std::size_t>(1));
	CHECK_EQ(registered.session.kcp_conv, phk::battle::DeriveDevKcpConv("match-001", "p1"));

	const auto replay = server.RegisterTicket(ticket);
	CHECK_TRUE(!replay.ok);
	CHECK_EQ(replay.reason, std::string("ticket_replay"));

	auto duplicate_player = MakeTicket();
	duplicate_player.ticket.ticket_id = "ticket-duplicate-player";
	duplicate_player.ticket.ticket_nonce_hex = "1234567890abcdef12345678";
	const auto duplicate_player_result = server.RegisterTicket(duplicate_player);
	CHECK_TRUE(!duplicate_player_result.ok);
	CHECK_EQ(duplicate_player_result.reason, std::string("player_session_replay"));

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
    CHECK_EQ(server.ActiveSessionCount(), static_cast<std::size_t>(1));

	const auto registered_bob = server.RegisterTicket(MakeTicketForBob());
	CHECK_TRUE(registered_bob.ok);
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

	auto wrong_players = MakeBattleResultForSummary(summary);
	wrong_players.result.player_ids = {"p1"};
	const auto wrong_players_result = server.SubmitBattleResult(wrong_players);
	CHECK_TRUE(!wrong_players_result.ok);
	CHECK_EQ(wrong_players_result.reason, std::string("player_ids_mismatch"));

    auto wrong_mode = MakeBattleResultForSummary(summary);
    wrong_mode.result.mode_id = "battle_royale";
    const auto wrong_mode_result = server.SubmitBattleResult(wrong_mode);
    CHECK_TRUE(!wrong_mode_result.ok);
    CHECK_EQ(wrong_mode_result.reason, std::string("mode_mismatch"));

    auto wrong_ruleset = MakeBattleResultForSummary(summary);
    wrong_ruleset.result.version.ruleset_version = "ruleset-other";
    const auto wrong_ruleset_result = server.SubmitBattleResult(wrong_ruleset);
    CHECK_TRUE(!wrong_ruleset_result.ok);
    CHECK_EQ(wrong_ruleset_result.reason, std::string("ruleset_version_mismatch"));

    auto wrong_hash = MakeBattleResultForSummary(summary);
    wrong_hash.result.result_hash = "sha256:wrong";
    const auto wrong_hash_result = server.SubmitBattleResult(wrong_hash);
    CHECK_TRUE(!wrong_hash_result.ok);
    CHECK_EQ(wrong_hash_result.reason, std::string("result_hash_mismatch"));

    auto wrong_replay = MakeBattleResultForSummary(summary);
    wrong_replay.result.replay_id = "battle-replay:wrong";
    const auto wrong_replay_result = server.SubmitBattleResult(wrong_replay);
    CHECK_TRUE(!wrong_replay_result.ok);
    CHECK_EQ(wrong_replay_result.reason, std::string("replay_id_mismatch"));

    auto wrong_cursor = MakeBattleResultForSummary(summary);
    wrong_cursor.result.mode_result_json = ReplaceFirst(
        ModeResultJsonForSummary(summary),
        "\"event_cursor\":" + std::to_string(summary.event_count),
        "\"event_cursor\":999"
    );
    const auto wrong_cursor_result = server.SubmitBattleResult(wrong_cursor);
    CHECK_TRUE(!wrong_cursor_result.ok);
    CHECK_EQ(wrong_cursor_result.reason, std::string("event_cursor_mismatch"));

    auto missing_replay_counts = MakeBattleResultForSummary(summary);
    missing_replay_counts.result.mode_result_json = "{\"battle_result_owner\":\"cpp\",\"event_cursor\":" +
        std::to_string(summary.event_count) + "}";
    const auto missing_replay_counts_result = server.SubmitBattleResult(missing_replay_counts);
    CHECK_TRUE(!missing_replay_counts_result.ok);
    CHECK_EQ(missing_replay_counts_result.reason, std::string("final_tick_mismatch"));

    auto wrong_final_tick = MakeBattleResultForSummary(summary);
    wrong_final_tick.result.mode_result_json = ReplaceFirst(
        ModeResultJsonForSummary(summary),
        "\"final_tick\":" + std::to_string(summary.final_tick),
        "\"final_tick\":999"
    );
    const auto wrong_final_tick_result = server.SubmitBattleResult(wrong_final_tick);
    CHECK_TRUE(!wrong_final_tick_result.ok);
    CHECK_EQ(wrong_final_tick_result.reason, std::string("final_tick_mismatch"));

    auto wrong_mode_action_count = MakeBattleResultForSummary(summary);
    wrong_mode_action_count.result.mode_result_json = ReplaceFirst(
        ModeResultJsonForSummary(summary),
        "\"mode_action_count\":" + std::to_string(summary.mode_action_count),
        "\"mode_action_count\":999"
    );
    const auto wrong_mode_action_count_result = server.SubmitBattleResult(wrong_mode_action_count);
    CHECK_TRUE(!wrong_mode_action_count_result.ok);
    CHECK_EQ(wrong_mode_action_count_result.reason, std::string("mode_action_count_mismatch"));

    auto wrong_event_trace_count = MakeBattleResultForSummary(summary);
    wrong_event_trace_count.result.mode_result_json = ReplaceFirst(
        ModeResultJsonForSummary(summary),
        "\"event_trace_count\":" + std::to_string(summary.event_trace.size()),
        "\"event_trace_count\":999"
    );
    const auto wrong_event_trace_count_result = server.SubmitBattleResult(wrong_event_trace_count);
    CHECK_TRUE(!wrong_event_trace_count_result.ok);
    CHECK_EQ(wrong_event_trace_count_result.reason, std::string("event_trace_count_mismatch"));

    auto wrong_input_stream_hash = MakeBattleResultForSummary(summary);
    wrong_input_stream_hash.result.mode_result_json = ReplaceFirst(
        ModeResultJsonForSummary(summary),
        "\"input_stream_hash\":\"" + summary.input_stream_hash + "\"",
        "\"input_stream_hash\":\"fnv64:0000000000000000\""
    );
    const auto wrong_input_stream_hash_result = server.SubmitBattleResult(wrong_input_stream_hash);
    CHECK_TRUE(!wrong_input_stream_hash_result.ok);
    CHECK_EQ(wrong_input_stream_hash_result.reason, std::string("input_stream_hash_mismatch"));

    auto wrong_event_stream_hash = MakeBattleResultForSummary(summary);
    wrong_event_stream_hash.result.mode_result_json = ReplaceFirst(
        ModeResultJsonForSummary(summary),
        "\"event_stream_hash\":\"" + summary.event_stream_hash + "\"",
        "\"event_stream_hash\":\"fnv64:0000000000000000\""
    );
    const auto wrong_event_stream_hash_result = server.SubmitBattleResult(wrong_event_stream_hash);
    CHECK_TRUE(!wrong_event_stream_hash_result.ok);
    CHECK_EQ(wrong_event_stream_hash_result.reason, std::string("event_stream_hash_mismatch"));

    auto wrong_final_state_hash = MakeBattleResultForSummary(summary);
    wrong_final_state_hash.result.mode_result_json = ReplaceFirst(
        ModeResultJsonForSummary(summary),
        "\"final_state_hash\":\"" + summary.final_state_hash + "\"",
        "\"final_state_hash\":\"fnv64:0000000000000000\""
    );
    const auto wrong_final_state_hash_result = server.SubmitBattleResult(wrong_final_state_hash);
    CHECK_TRUE(!wrong_final_state_hash_result.ok);
    CHECK_EQ(wrong_final_state_hash_result.reason, std::string("final_state_hash_mismatch"));

    auto mutating_projection = MakeBattleResultForSummary(summary);
    mutating_projection.result.reward_projection_json = "{\"source\":\"battle-server\",\"grant_currency\":100}";
    const auto mutating_projection_result = server.SubmitBattleResult(mutating_projection);
    CHECK_TRUE(!mutating_projection_result.ok);
    CHECK_EQ(mutating_projection_result.reason, std::string("reward_projection_mutation_forbidden"));

	const auto accepted = server.SubmitBattleResult(MakeBattleResultForSummary(summary));
	CHECK_TRUE(accepted.ok);
	CHECK_EQ(accepted.reason, std::string("ok"));
	CHECK_EQ(accepted.settlement_key, std::string(phk::v1::kBattleResultCallbackSettlementKey));
	CHECK_TRUE(!accepted.verification.warnings.empty());

	const auto duplicate = server.SubmitBattleResult(MakeBattleResultForSummary(summary));
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
    CHECK_EQ(built.signed_result.result.result_hash, std::string("sha256:dev-fnv64-7cd25aafda3bc356"));
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
        std::string(
            "5c980f233dd972e07b92d62c48c8bd019a8d9d3553b80722b988643e5ea75143"
            "d8832b4769969b64f77df2507485e5851678b9597f752fa6357380628a6479c7"
        )
    );
    CHECK_TRUE(built.signed_result.server_authoritative);
    CHECK_EQ(
        phk::battle::CanonicalBattleResultPayload(built.signed_result.result),
        std::string(
            "1|0.1.0-draft|0.1.0-draft|ruleset-local-s0|match-001|certification|"
            "sha256:dev-fnv64-7cd25aafda3bc356|battle-replay:match-001:1|p1,p2,|"
            "{\"source\":\"phk-battle-server\",\"projection_only\":true,\"settlement_authority\":\"nakama-go\"}|"
            "{\"battle_result_owner\":\"cpp\",\"event_cursor\":0,\"final_tick\":1,\"input_count\":2,"
            "\"fallback_input_count\":0,\"neutral_fallback_count\":0,\"held_input_fallback_count\":0,"
            "\"mode_action_count\":0,\"input_trace_count\":2,\"event_trace_count\":0,"
            "\"input_stream_hash\":\"fnv64:6b09da7d62e0941e\","
            "\"event_stream_hash\":\"fnv64:14650fb0739d0383\","
            "\"final_state_hash\":\"fnv64:72a3385f1a7c7fe3\"}|1782489630000"
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
    CHECK_TRUE(
        built.signed_result.result.mode_result_json.find(
            "\"final_tick\":" + std::to_string(built.replay_summary.final_tick)
        ) != std::string::npos
    );
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
    action.action_type = std::string(phk::v1::kBattleModeActionActionType);
    action.payload_json = std::string(phk::v1::kBattleModeActionPayloadJson);
    action.client_result_authoritative = false;
    return action;
}

bool TestSimulationDeterminism() {
    phk::battle::SimulationConfig config;
    config.match_id = "match-001";
    config.mode_id = "certification";
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

    phk::battle::BattleSnapshot first_snapshot;
    phk::battle::BattleSnapshot second_snapshot;
    for (int i = 0; i < 3; ++i) {
        first_snapshot = first.Tick();
        second_snapshot = second.Tick();
    }

    CHECK_EQ(first_snapshot.snapshot_tick, static_cast<std::uint64_t>(3));
    CHECK_EQ(first_snapshot.players.size(), static_cast<std::size_t>(2));
    CHECK_TRUE(first_snapshot.bullets_delta.size() >= 4);
    CHECK_EQ(first_snapshot.mode_state.at("mode_id"), std::string("certification"));
    CHECK_EQ(first_snapshot.mode_state.at("ruleset_version"), std::string(phk::v1::kRulesetVersion));
    CHECK_EQ(first_snapshot.state_hash, second_snapshot.state_hash);
    CHECK_EQ(first.Summary().input_stream_hash, second.Summary().input_stream_hash);
    CHECK_EQ(first.Summary().event_stream_hash, second.Summary().event_stream_hash);
    CHECK_EQ(first.Summary().final_state_hash, first_snapshot.state_hash);
    CHECK_EQ(first.Summary().fallback_input_count, static_cast<std::uint64_t>(4));
    CHECK_EQ(first.Summary().neutral_fallback_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(first.Summary().held_input_fallback_count, static_cast<std::uint64_t>(4));
    CHECK_EQ(first.Summary().mode_action_count, static_cast<std::uint64_t>(1));
    CHECK_EQ(first.Summary().event_count, static_cast<std::uint64_t>(2));
    CHECK_EQ(first.Summary().last_mode_action_id, action.action_id);
    CHECK_EQ(first.Summary().last_mode_action_type, action.action_type);
    CHECK_EQ(first.Summary().last_mode_action_player_id, action.player_id);
    CHECK_EQ(first.Summary().last_mode_action_tick, action.tick);
    CHECK_EQ(first.Summary().last_mode_action_seq, action.seq);
    CHECK_EQ(first.Summary().last_mode_action_id, second.Summary().last_mode_action_id);
    CHECK_TRUE(first.Summary().input_trace == second.Summary().input_trace);
    CHECK_TRUE(first.Summary().event_trace == second.Summary().event_trace);
    CHECK_EQ(first.Summary().input_trace.size(), static_cast<std::size_t>(6));
    CHECK_EQ(first.Summary().event_trace.size(), static_cast<std::size_t>(2));
    CHECK_TRUE(first.Summary().input_trace[0].find("input|p1|tick=1|seq=1") != std::string::npos);
    CHECK_TRUE(first.Summary().input_trace[2].find("fallback|held|p1|tick=2") != std::string::npos);
    CHECK_TRUE(first.Summary().event_trace[0].find("mode_action|p1|tick=2|seq=2") != std::string::npos);
    CHECK_TRUE(first.Summary().event_trace[1].find("bullet_spawn|tick=2") != std::string::npos);
    CHECK_EQ(first_snapshot.mode_state.at("last_mode_action_id"), action.action_id);
    CHECK_EQ(first_snapshot.mode_state.at("last_mode_action_type"), action.action_type);
    CHECK_EQ(first_snapshot.mode_state.at("last_mode_action_player_id"), action.player_id);
    CHECK_EQ(first_snapshot.mode_state.at("last_mode_action_tick"), std::to_string(action.tick));
    CHECK_EQ(first_snapshot.mode_state.at("last_mode_action_seq"), std::to_string(action.seq));
    CHECK_EQ(first_snapshot.mode_state.at("accepted_input_count"), std::string("2"));
    CHECK_EQ(first_snapshot.mode_state.at("fallback_input_count"), std::string("4"));
    CHECK_EQ(first_snapshot.mode_state.at("neutral_fallback_count"), std::string("0"));
    CHECK_EQ(first_snapshot.mode_state.at("held_input_fallback_count"), std::string("4"));
    CHECK_EQ(first_snapshot.mode_state.at("mode_action_count"), std::string("1"));
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
    CHECK_EQ(ExpectedDevResultHash(first_summary), std::string("sha256:dev-fnv64-eb5d3d3884abf76a"));
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
    CHECK_EQ(fixture.tick_rate_hz, phk::battle::kBattleTickRateHz);
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
    CHECK_EQ(fixture.replay_summary_record.final_tick, fixture.summary.final_tick);
    CHECK_EQ(fixture.summary.final_tick, static_cast<std::uint64_t>(60));
    CHECK_EQ(fixture.summary.input_count, static_cast<std::uint64_t>(120));
    CHECK_EQ(fixture.summary.fallback_input_count, static_cast<std::uint64_t>(0));
    CHECK_EQ(fixture.summary.event_count, static_cast<std::uint64_t>(4));
    CHECK_EQ(fixture.summary.input_stream_hash, std::string("fnv64:a0b383d4a7be0bf7"));
    CHECK_EQ(fixture.summary.event_stream_hash, std::string("fnv64:daa6853bacb4fdd3"));
    CHECK_EQ(fixture.summary.final_state_hash, std::string("fnv64:8049946f03724f36"));
    CHECK_EQ(fixture.result_hash, std::string("sha256:dev-fnv64-a7519545ad65902e"));
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
    CHECK_EQ(fixture.final_snapshot.snapshot_kind, std::string("replay_final"));
    CHECK_EQ(fixture.final_snapshot.snapshot_tick, static_cast<std::uint64_t>(60));
    CHECK_EQ(fixture.final_snapshot.state_hash, fixture.summary.final_state_hash);
    CHECK_EQ(fixture.final_snapshot.event_cursor, fixture.event_cursor);
    CHECK_EQ(fixture.final_snapshot.mode_state.at("mode_id"), config.mode_id);
    CHECK_EQ(fixture.final_snapshot.mode_state.at("tick_rate_hz"), std::string("60"));
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
    CHECK_EQ(summary_record.final_tick, static_cast<std::uint64_t>(60));
    CHECK_TRUE(
        phk::battle::CanonicalReplayInputStreamSummaryRecord(summary_record) ==
        "1|0.1.0-draft|0.1.0-draft|ruleset-local-s0|battle-replay:match-replay-fixture:60|"
        "user-alice|match-replay-fixture|120|4|fnv64:a0b383d4a7be0bf7|"
        "fnv64:daa6853bacb4fdd3|fnv64:8049946f03724f36|60"
    );
    auto tampered_record = summary_record;
    tampered_record.final_tick = 61;
    CHECK_TRUE(
        phk::battle::CanonicalReplayInputStreamSummaryRecord(tampered_record) !=
        phk::battle::CanonicalReplayInputStreamSummaryRecord(summary_record)
    );
    const auto canonical_fixture_payload = phk::battle::CanonicalReplayFixturePayload(fixture);
    CHECK_TRUE(canonical_fixture_payload.find("battle-replay:match-replay-fixture:60|user-alice") == 0);
    CHECK_TRUE(canonical_fixture_payload.find("|pvp_duel|ruleset-local-s0|") != std::string::npos);
    CHECK_TRUE(canonical_fixture_payload.find("|60|4|1|") != std::string::npos);
    CHECK_TRUE(
        canonical_fixture_payload.find(
            "1|0.1.0-draft|0.1.0-draft|ruleset-local-s0|battle-replay:match-replay-fixture:60|"
            "user-alice|match-replay-fixture|120|4|fnv64:a0b383d4a7be0bf7|"
            "fnv64:daa6853bacb4fdd3|fnv64:8049946f03724f36|60"
        ) != std::string::npos
    );
    CHECK_TRUE(canonical_fixture_payload.find("|replay_final|60|fnv64:8049946f03724f36|4|") != std::string::npos);
    CHECK_TRUE(canonical_fixture_payload.find("|p1,p2,|") != std::string::npos);
    CHECK_TRUE(canonical_fixture_payload.find("input|p1|tick=1|seq=1") != std::string::npos);
    CHECK_TRUE(canonical_fixture_payload.find("bullet_spawn|tick=60") != std::string::npos);
    CHECK_EQ(
        phk::battle::DevReplayFixtureHash(fixture),
        std::string("sha256:dev-fnv64-54919460e75ba83d")
    );
    auto tampered_fixture_record = fixture;
    tampered_fixture_record.replay_summary_record.final_state_hash = "fnv64:0000000000000000";
    CHECK_TRUE(phk::battle::DevReplayFixtureHash(tampered_fixture_record) != phk::battle::DevReplayFixtureHash(fixture));
    auto tampered_fixture_snapshot = fixture;
    tampered_fixture_snapshot.final_snapshot.state_hash = "fnv64:0000000000000000";
    CHECK_TRUE(phk::battle::DevReplayFixtureHash(tampered_fixture_snapshot) != phk::battle::DevReplayFixtureHash(fixture));
    auto tampered_fixture_trace = fixture;
    tampered_fixture_trace.input_trace.back() += "|tampered";
    CHECK_TRUE(phk::battle::DevReplayFixtureHash(tampered_fixture_trace) != phk::battle::DevReplayFixtureHash(fixture));
    auto tampered_fixture_authority = fixture;
    tampered_fixture_authority.server_authoritative = false;
    CHECK_TRUE(phk::battle::DevReplayFixtureHash(tampered_fixture_authority) != phk::battle::DevReplayFixtureHash(fixture));
    CHECK_TRUE(phk::v1::HasMessageField("ReplayInputStreamSummary", "replay_id"));
    CHECK_TRUE(phk::v1::HasMessageField("ReplayInputStreamSummary", "owner_user_id"));
    CHECK_TRUE(phk::v1::HasMessageField("ReplayInputStreamSummary", "event_stream_hash"));
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

    const auto reconnected = server.SetPlayerConnected("match-001", "p2", true);
    CHECK_TRUE(reconnected.ok);
    const auto reconnect_snapshot = server.ReconnectSnapshot("match-001", "p2", mode_action_summary.event_count);
    CHECK_EQ(reconnect_snapshot.snapshot_kind, std::string("reconnect"));
    CHECK_EQ(reconnect_snapshot.mode_state.at("reconnect_player_id"), std::string("p2"));
    CHECK_EQ(reconnect_snapshot.mode_state.at("missed_event_count"), std::string("2"));
    CHECK_TRUE(server.MatchReplaySummary("match-001").event_trace.back().find("connection|connected|p2") != std::string::npos);
    const auto cursor_ahead_snapshot = server.ReconnectSnapshot("match-001", "p2", 999);
    CHECK_EQ(cursor_ahead_snapshot.snapshot_kind, std::string("event_cursor_ahead"));
    CHECK_EQ(cursor_ahead_snapshot.mode_state.at("requested_event_cursor"), std::string("999"));
    const auto unknown_reconnect_snapshot = server.ReconnectSnapshot("match-001", "p3", 0);
    CHECK_EQ(unknown_reconnect_snapshot.snapshot_kind, std::string("player_unknown"));
    const auto reconnected_input = server.AcceptInput(MakeInput("p2", 3, 3, 1u << 2));
    CHECK_TRUE(reconnected_input.ok);
    const auto reconnected_snapshot = server.MatchSnapshot("match-001");
    CHECK_EQ(reconnected_snapshot.mode_state.at("connected_player_count"), std::string("2"));
    CHECK_EQ(reconnected_snapshot.mode_state.at("disconnected_player_count"), std::string("0"));

    auto unknown_player = MakeInput("p3", 2, 1, 0);
    const auto unknown_player_result = server.AcceptInput(unknown_player);
    CHECK_TRUE(!unknown_player_result.ok);
    CHECK_EQ(unknown_player_result.reason, std::string("player_unknown"));

    auto forged_action = MakeModeAction(3);
    forged_action.tick = 3;
    forged_action.client_result_authoritative = true;
    const auto forged_result = server.AcceptModeAction(forged_action);
    CHECK_TRUE(!forged_result.ok);
    CHECK_EQ(forged_result.reason, std::string("mode_action_client_result_forbidden"));
    CHECK_EQ(server.MatchReplaySummary("match-001").last_mode_action_id, action.action_id);

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
    phk::battle::BattleModeAction action = MakeModeAction();
    CHECK_TRUE(!action.client_result_authoritative);
    const auto mode_action_result = dispatcher.Dispatch(mode_action, {'{', '}'});
    CHECK_TRUE(mode_action_result.ok);
    CHECK_EQ(mode_action_result.response_kind, std::string("mode_action"));
    CHECK_EQ(phk::battle::PayloadTypeName(phk::battle::BattlePayloadType::ModeAction), std::string("mode_action"));

    phk::battle::BattlePacketHeader empty_mode_action = mode_action;
    empty_mode_action.seq = phk::v1::kBattleModeActionSeq + 1;
    empty_mode_action.tick = phk::v1::kBattleModeActionTick + 1;
    const auto empty_mode_action_result = dispatcher.Dispatch(empty_mode_action, {});
    CHECK_TRUE(empty_mode_action_result.ok);
    CHECK_EQ(empty_mode_action_result.response_kind, std::string("mode_action_empty_payload"));

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
    const auto result_packet_result = dispatcher.DispatchEncrypted(result_packet);
    CHECK_TRUE(!result_packet_result.ok);
    CHECK_EQ(result_packet_result.reason, std::string("client_result_forbidden"));

    auto event_packet = packet;
    event_packet.header.player_id = "p2";
    event_packet.header.seq = 6;
    event_packet.header.payload_type = phk::battle::BattlePayloadType::Event;
    const auto event_packet_result = dispatcher.DispatchEncrypted(event_packet);
    CHECK_TRUE(!event_packet_result.ok);
    CHECK_EQ(event_packet_result.reason, std::string("encrypted_payload_type_invalid"));
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
    const auto ticket_key_result = server.DispatchEncrypted(ticket_key_packet);
    CHECK_TRUE(!ticket_key_result.ok);
    CHECK_EQ(ticket_key_result.reason, std::string("session_key_mismatch"));

    packet.header.key_id = accept.client_to_server_key_ref;
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
    wrong_key.header.nonce_hex = RepeatHex('2', 24);
    wrong_key.header.key_id = "other-key";
    const auto wrong_key_result = server.DispatchEncrypted(wrong_key);
    CHECK_TRUE(!wrong_key_result.ok);
    CHECK_EQ(wrong_key_result.reason, std::string("session_key_mismatch"));

    auto ack_ahead = packet;
    ack_ahead.header.player_id = "p2";
    ack_ahead.header.key_id = bob_accept.client_to_server_key_ref;
    ack_ahead.header.seq = 1;
    ack_ahead.header.ack = 1;
    ack_ahead.header.nonce_hex = RepeatHex('8', 24);
    const auto ack_ahead_result = server.DispatchEncrypted(ack_ahead);
    CHECK_TRUE(!ack_ahead_result.ok);
    CHECK_EQ(ack_ahead_result.reason, std::string("encrypted_ack_ahead"));

    auto far_future_tick = packet;
    far_future_tick.header.player_id = "p2";
    far_future_tick.header.key_id = bob_accept.client_to_server_key_ref;
    far_future_tick.header.seq = 1;
    far_future_tick.header.tick = 99;
    far_future_tick.header.nonce_hex = RepeatHex('6', 24);
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
    stale_tick.header.nonce_hex = RepeatHex('7', 24);
    const auto stale_tick_result = server.DispatchEncrypted(stale_tick);
    CHECK_TRUE(!stale_tick_result.ok);
    CHECK_EQ(stale_tick_result.reason, std::string("encrypted_tick_too_old"));

    auto current_ack = packet;
    current_ack.header.player_id = "p2";
    current_ack.header.key_id = bob_accept.client_to_server_key_ref;
    current_ack.header.seq = 2;
    current_ack.header.tick = 2;
    current_ack.header.ack = 1;
    current_ack.header.nonce_hex = RepeatHex('9', 24);
    const auto current_ack_result = server.DispatchEncrypted(current_ack);
    CHECK_TRUE(current_ack_result.ok);
    CHECK_EQ(current_ack_result.response_kind, std::string("input"));

    CHECK_TRUE(server.SetPlayerConnected("match-001", "p2", false).ok);
    auto disconnected_input = packet;
    disconnected_input.header.player_id = "p2";
    disconnected_input.header.key_id = bob_accept.client_to_server_key_ref;
    disconnected_input.header.seq = 3;
    disconnected_input.header.tick = 2;
    disconnected_input.header.ack = 1;
    disconnected_input.header.nonce_hex = RepeatHex('a', 24);
    const auto disconnected_input_result = server.DispatchEncrypted(disconnected_input);
    CHECK_TRUE(!disconnected_input_result.ok);
    CHECK_EQ(disconnected_input_result.reason, std::string("encrypted_player_disconnected"));
    CHECK_TRUE(server.SetPlayerConnected("match-001", "p2", true).ok);

    auto reconnect_cursor_ahead = packet;
    reconnect_cursor_ahead.header.payload_type = phk::battle::BattlePayloadType::Reconnect;
    reconnect_cursor_ahead.header.player_id = "p2";
    reconnect_cursor_ahead.header.key_id = bob_accept.client_to_server_key_ref;
    reconnect_cursor_ahead.header.seq = 4;
    reconnect_cursor_ahead.header.tick = 1;
    reconnect_cursor_ahead.header.ack = server.MatchReplaySummary("match-001").event_count + 1;
    reconnect_cursor_ahead.header.nonce_hex = RepeatHex('b', 24);
    const auto reconnect_cursor_ahead_result = server.DispatchEncrypted(reconnect_cursor_ahead);
    CHECK_TRUE(!reconnect_cursor_ahead_result.ok);
    CHECK_EQ(reconnect_cursor_ahead_result.reason, std::string("encrypted_event_cursor_ahead"));

    auto reconnect_current_cursor = packet;
    reconnect_current_cursor.header.payload_type = phk::battle::BattlePayloadType::Reconnect;
    reconnect_current_cursor.header.player_id = "p2";
    reconnect_current_cursor.header.key_id = bob_accept.client_to_server_key_ref;
    reconnect_current_cursor.header.seq = 4;
    reconnect_current_cursor.header.tick = 1;
    reconnect_current_cursor.header.ack = server.MatchReplaySummary("match-001").event_count;
    reconnect_current_cursor.header.nonce_hex = RepeatHex('c', 24);
    const auto reconnect_current_cursor_result = server.DispatchEncrypted(reconnect_current_cursor);
    CHECK_TRUE(reconnect_current_cursor_result.ok);
    CHECK_EQ(reconnect_current_cursor_result.response_kind, std::string("reconnect"));

    auto unknown_player = packet;
    unknown_player.header.player_id = "p3";
    unknown_player.header.seq = 1;
    unknown_player.header.nonce_hex = RepeatHex('3', 24);
    const auto unknown_player_result = server.DispatchEncrypted(unknown_player);
    CHECK_TRUE(!unknown_player_result.ok);
    CHECK_EQ(unknown_player_result.reason, std::string("player_unknown"));

    auto unknown_match = packet;
    unknown_match.header.match_id = "match-missing";
    unknown_match.header.seq = 1;
    unknown_match.header.nonce_hex = RepeatHex('4', 24);
    const auto unknown_match_result = server.DispatchEncrypted(unknown_match);
    CHECK_TRUE(!unknown_match_result.ok);
    CHECK_EQ(unknown_match_result.reason, std::string("match_unknown"));

    auto result_packet = packet;
    result_packet.header.player_id = "p2";
    result_packet.header.seq = 1;
    result_packet.header.nonce_hex = RepeatHex('5', 24);
    result_packet.header.payload_type = phk::battle::BattlePayloadType::Result;
    const auto result_packet_result = server.DispatchEncrypted(result_packet);
    CHECK_TRUE(!result_packet_result.ok);
    CHECK_EQ(result_packet_result.reason, std::string("client_result_forbidden"));
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
    packet.header.nonce_hex = RepeatHex('c', 24);
    packet.ciphertext = {'i', 'n', 'p', 'u', 't'};
    packet.auth_tag.assign(16, 0x33);

    phk::battle::UdpDatagram datagram;
    datagram.remote_endpoint = "127.0.0.1:52001";
    datagram.payload = {'k', 'c', 'p', ':', 'i', 'n', 'p', 'u', 't'};

    auto wrong_key = packet;
    wrong_key.header.key_id = "wrong-key";
    wrong_key.header.nonce_hex = RepeatHex('d', 24);
    const auto rejected = adapter.ProcessEncryptedDatagram(wrong_key, datagram);
    CHECK_TRUE(!rejected.ok);
    CHECK_EQ(rejected.reason, std::string("session_key_mismatch"));
    CHECK_TRUE(rejected.replies.empty());
    CHECK_EQ(endpoint.Stats().datagrams_in, static_cast<std::uint64_t>(0));
    CHECK_EQ(endpoint.Stats().datagrams_out, static_cast<std::uint64_t>(0));

    const auto accepted = adapter.ProcessEncryptedDatagram(packet, datagram);
    CHECK_TRUE(accepted.ok);
    CHECK_EQ(accepted.dispatch.response_kind, std::string("input"));
    CHECK_EQ(accepted.reason, std::string("input"));
    CHECK_EQ(accepted.replies.size(), static_cast<std::size_t>(1));
    CHECK_EQ(accepted.replies[0].remote_endpoint, datagram.remote_endpoint);
    CHECK_TRUE(accepted.replies[0].payload.size() > datagram.payload.size());
    CHECK_EQ(endpoint.Stats().datagrams_in, static_cast<std::uint64_t>(1));
    CHECK_EQ(endpoint.Stats().datagrams_out, static_cast<std::uint64_t>(1));

    auto nonce_replay = packet;
    nonce_replay.header.seq = 2;
    const auto replay = adapter.ProcessEncryptedDatagram(nonce_replay, datagram);
    CHECK_TRUE(!replay.ok);
    CHECK_EQ(replay.reason, std::string("nonce_replay"));
    CHECK_TRUE(replay.replies.empty());
    CHECK_EQ(endpoint.Stats().datagrams_in, static_cast<std::uint64_t>(1));
    CHECK_EQ(endpoint.Stats().datagrams_out, static_cast<std::uint64_t>(1));
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
		{"AuthoritativeReplay60TickFixture", TestAuthoritativeReplay60TickFixture},
		{"ReplayFixtureBoundary", TestReplayFixtureBoundary},
		{"ServerAuthoritativeInputAndSnapshot", TestServerAuthoritativeInputAndSnapshot},
		{"Dispatcher", TestDispatcher},
		{"EncryptedPacketAdapterShape", TestEncryptedPacketAdapterShape},
		{"ServerEncryptedPacketSessionBoundary", TestServerEncryptedPacketSessionBoundary},
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
