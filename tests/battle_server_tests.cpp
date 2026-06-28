#include <cstdlib>
#include <iostream>
#include <string>
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
    CHECK_TRUE(phk::v1::HasMessageField("BattleInput", "direction_bits"));
    CHECK_TRUE(phk::v1::HasMessageField("BattleResult", "result_hash"));
    CHECK_TRUE(phk::v1::MessageFieldCount("BattleInput") >= 10);
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
    phk::battle::BattleSimulation simulation(config);
    CHECK_TRUE(simulation.Summary().input_stream_hash.rfind("fnv64:", 0) == 0);
    CHECK_TRUE(simulation.Summary().event_stream_hash.rfind("fnv64:", 0) == 0);
    CHECK_TRUE(!simulation.Summary().final_state_hash.empty());
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

	const auto registered_bob = server.RegisterTicket(MakeTicketForBob());
	CHECK_TRUE(registered_bob.ok);
	CHECK_EQ(server.ActiveSessionCount(), static_cast<std::size_t>(2));

	phk::battle::BattleHandshakeHello hello;
	hello.battle_ticket = ticket;
    hello.supported_aead = {"AES_256_GCM", "CHACHA20_POLY1305"};
    const auto accept = server.AcceptHandshake(hello);
    CHECK_EQ(accept.match_id, std::string("match-001"));
    CHECK_EQ(accept.player_id, std::string("p1"));
    CHECK_EQ(accept.selected_aead, std::string("CHACHA20_POLY1305"));
    CHECK_TRUE(!accept.transcript_hash_hex.empty());
	CHECK_TRUE(!accept.dev_session_id.empty());
	return true;
}

bool TestBattleResultSubmission() {
	phk::battle::BattleServerConfig config;
	config.now_ms = 1782489620000;
	phk::battle::BattleServer server(config);
	CHECK_TRUE(server.RegisterTicket(MakeTicket()).ok);
	CHECK_TRUE(server.RegisterTicket(MakeTicketForBob()).ok);

	auto wrong_players = MakeBattleResult();
	wrong_players.result.player_ids = {"p1"};
	const auto wrong_players_result = server.SubmitBattleResult(wrong_players);
	CHECK_TRUE(!wrong_players_result.ok);
	CHECK_EQ(wrong_players_result.reason, std::string("player_ids_mismatch"));

	const auto accepted = server.SubmitBattleResult(MakeBattleResult());
	CHECK_TRUE(accepted.ok);
	CHECK_EQ(accepted.reason, std::string("ok"));
	CHECK_EQ(accepted.settlement_key, std::string(phk::v1::kBattleResultCallbackSettlementKey));
	CHECK_TRUE(!accepted.verification.warnings.empty());

	const auto duplicate = server.SubmitBattleResult(MakeBattleResult());
	CHECK_TRUE(duplicate.ok);
	CHECK_TRUE(duplicate.duplicate);
	CHECK_EQ(duplicate.reason, std::string("ok"));
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
    config.match_seed = 12345;
    config.spawn_period_ticks = 2;
    phk::battle::BattleSimulation first(config);
    phk::battle::BattleSimulation second(config);
    CHECK_EQ(first.Config().tick_rate_hz, phk::battle::kBattleTickRateHz);
    CHECK_TRUE(first.AddPlayer("p1", -20000, 0));
    CHECK_TRUE(first.AddPlayer("p2", 20000, 0));
    CHECK_TRUE(second.AddPlayer("p1", -20000, 0));
    CHECK_TRUE(second.AddPlayer("p2", 20000, 0));

    const auto accepted = first.AcceptInput(MakeInput("p1", 1, 1, 1u << 3));
    CHECK_TRUE(accepted.ok);
    const auto replay = first.AcceptInput(MakeInput("p1", 2, 1, 1u << 3));
    CHECK_TRUE(!replay.ok);
    CHECK_EQ(replay.reason, std::string("seq_replay"));
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

    phk::battle::BattleSnapshot first_snapshot;
    phk::battle::BattleSnapshot second_snapshot;
    for (int i = 0; i < 3; ++i) {
        first_snapshot = first.Tick();
        second_snapshot = second.Tick();
    }

    CHECK_EQ(first_snapshot.snapshot_tick, static_cast<std::uint64_t>(3));
    CHECK_EQ(first_snapshot.players.size(), static_cast<std::size_t>(2));
    CHECK_TRUE(first_snapshot.bullets_delta.size() >= 4);
    CHECK_EQ(first_snapshot.state_hash, second_snapshot.state_hash);
    CHECK_EQ(first.Summary().input_stream_hash, second.Summary().input_stream_hash);
    CHECK_EQ(first.Summary().event_stream_hash, second.Summary().event_stream_hash);
    CHECK_EQ(first.Summary().final_state_hash, first_snapshot.state_hash);
    CHECK_EQ(first.Summary().event_count, static_cast<std::uint64_t>(2));
    CHECK_EQ(first.Summary().last_mode_action_id, action.action_id);
    CHECK_EQ(first.Summary().last_mode_action_type, action.action_type);
    CHECK_EQ(first.Summary().last_mode_action_player_id, action.player_id);
    CHECK_EQ(first.Summary().last_mode_action_tick, action.tick);
    CHECK_EQ(first.Summary().last_mode_action_seq, action.seq);
    CHECK_EQ(first.Summary().last_mode_action_id, second.Summary().last_mode_action_id);
    CHECK_EQ(first_snapshot.mode_state.at("last_mode_action_id"), action.action_id);
    CHECK_EQ(first_snapshot.mode_state.at("last_mode_action_type"), action.action_type);
    CHECK_EQ(first_snapshot.mode_state.at("last_mode_action_player_id"), action.player_id);
    CHECK_EQ(first_snapshot.mode_state.at("last_mode_action_tick"), std::to_string(action.tick));
    CHECK_EQ(first_snapshot.mode_state.at("last_mode_action_seq"), std::to_string(action.seq));
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
    CHECK_EQ(snapshot.mode_state.at("tick_rate_hz"), std::string("60"));

    const auto replay_summary = server.MatchReplaySummary("match-001");
    CHECK_EQ(replay_summary.match_id, std::string("match-001"));
    CHECK_EQ(replay_summary.final_tick, static_cast<std::uint64_t>(1));
    CHECK_EQ(replay_summary.input_count, static_cast<std::uint64_t>(2));
    CHECK_EQ(replay_summary.final_state_hash, snapshot.state_hash);

    auto action = MakeModeAction();
    action.tick = 2;
    action.seq = 2;
    const auto mode_action = server.AcceptModeAction(action);
    CHECK_TRUE(mode_action.ok);
    const auto mode_action_summary = server.MatchReplaySummary("match-001");
    CHECK_EQ(mode_action_summary.event_count, replay_summary.event_count + 1);
    CHECK_TRUE(mode_action_summary.event_stream_hash != replay_summary.event_stream_hash);
    CHECK_EQ(mode_action_summary.last_mode_action_id, action.action_id);
    CHECK_EQ(mode_action_summary.last_mode_action_type, action.action_type);
    CHECK_EQ(mode_action_summary.last_mode_action_player_id, action.player_id);
    CHECK_EQ(mode_action_summary.last_mode_action_tick, action.tick);
    CHECK_EQ(mode_action_summary.last_mode_action_seq, action.seq);
    const auto after_action_snapshot = server.MatchSnapshot("match-001");
    CHECK_EQ(after_action_snapshot.mode_state.at("last_mode_action_id"), action.action_id);
    CHECK_EQ(after_action_snapshot.mode_state.at("last_mode_action_type"), action.action_type);
    CHECK_EQ(after_action_snapshot.mode_state.at("last_mode_action_player_id"), action.player_id);
    CHECK_EQ(after_action_snapshot.mode_state.at("last_mode_action_tick"), std::to_string(action.tick));
    CHECK_EQ(after_action_snapshot.mode_state.at("last_mode_action_seq"), std::to_string(action.seq));

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

}  // namespace

int main() {
	const std::vector<std::pair<std::string, bool (*)()>> tests = {
		{"ProtocolManifest", TestProtocolManifest},
		{"SharedSnapshotEventFixtures", TestSharedSnapshotEventFixtures},
		{"GoldenReplaySummaryFixture", TestGoldenReplaySummaryFixture},
		{"TicketVerifier", TestTicketVerifier},
		{"ServerAndHandshake", TestServerAndHandshake},
		{"BattleResultSubmission", TestBattleResultSubmission},
		{"SimulationDeterminism", TestSimulationDeterminism},
		{"ServerAuthoritativeInputAndSnapshot", TestServerAuthoritativeInputAndSnapshot},
		{"Dispatcher", TestDispatcher},
		{"KcpPlaceholder", TestKcpPlaceholder},
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
