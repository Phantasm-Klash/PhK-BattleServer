#include "phk/battle/server.hpp"

#include <iomanip>
#include <sstream>
#include <utility>

namespace phk::battle {

namespace {

bool SessionExistsForPlayer(
    const std::map<std::string, BattleSessionRecord>& sessions,
    const std::string& match_id,
    const std::string& player_id
) {
    for (const auto& item : sessions) {
        const BattleSessionRecord& session = item.second;
        if (session.match_id == match_id && session.player_id == player_id) {
            return true;
        }
    }
    return false;
}

const BattleSessionRecord* SessionForPlayer(
    const std::map<std::string, BattleSessionRecord>& sessions,
    const std::string& match_id,
    const std::string& player_id
) {
    for (const auto& item : sessions) {
        const BattleSessionRecord& session = item.second;
        if (session.match_id == match_id && session.player_id == player_id) {
            return &session;
        }
    }
    return nullptr;
}

bool IsInputWindowBoundPayload(BattlePayloadType payload_type) {
    return payload_type == BattlePayloadType::Input ||
        payload_type == BattlePayloadType::ModeAction;
}

InputValidationResult UnknownPlayerResult() {
    InputValidationResult result;
    result.code = InputValidationCode::PlayerUnknown;
    result.reason = "player_unknown";
    return result;
}

std::uint64_t HashAppend(std::uint64_t hash, const std::string& value) {
    for (const char ch : value) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t HashAppend(std::uint64_t hash, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        hash ^= static_cast<unsigned char>((value >> shift) & 0xffu);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string DevSha256RefFromSummary(const ReplaySummary& summary) {
    std::uint64_t hash = 1469598103934665603ull;
    hash = HashAppend(hash, summary.match_id);
    hash = HashAppend(hash, summary.mode_id);
    hash = HashAppend(hash, summary.ruleset_version);
    hash = HashAppend(hash, summary.input_stream_hash);
    hash = HashAppend(hash, summary.event_stream_hash);
    hash = HashAppend(hash, summary.final_state_hash);
    hash = HashAppend(hash, summary.final_tick);
    hash = HashAppend(hash, summary.input_count);
    hash = HashAppend(hash, summary.event_count);

    std::ostringstream out;
    out << "sha256:dev-fnv64-" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

std::string DevReplayIdFromSummary(const ReplaySummary& summary) {
    return "battle-replay:" + summary.match_id + ":" + std::to_string(summary.final_tick);
}

}  // namespace

BattleServer::BattleServer(BattleServerConfig config)
    : config_(std::move(config)) {}

const BattleServerConfig& BattleServer::Config() const {
    return config_;
}

std::size_t BattleServer::ActiveSessionCount() const {
    return sessions_by_ticket_.size();
}

RegisterTicketResult BattleServer::RegisterTicket(const SignedBattleTicket& signed_ticket) {
    RegisterTicketResult result;
    if (sessions_by_ticket_.find(signed_ticket.ticket.ticket_id) != sessions_by_ticket_.end()) {
        result.reason = "ticket_replay";
        return result;
    }

    TicketVerificationOptions options;
    options.now_ms = config_.now_ms;
    options.required_battle_server_id = config_.server_id;
    options.required_endpoint = config_.endpoint;
    options.required_key_id = config_.signing_key_id;
    result.verification = ticket_verifier_.Verify(signed_ticket, options);
    if (!result.verification.ok) {
        result.reason = result.verification.reason;
        return result;
    }
    for (const auto& item : sessions_by_ticket_) {
        const BattleSessionRecord& existing = item.second;
        if (existing.match_id == signed_ticket.ticket.match_id &&
            existing.player_id == signed_ticket.ticket.player_id) {
            result.reason = "player_session_replay";
            return result;
        }
    }
    std::size_t match_session_count = 0;
    for (const auto& item : sessions_by_ticket_) {
        const BattleSessionRecord& existing = item.second;
        if (existing.match_id == signed_ticket.ticket.match_id) {
            ++match_session_count;
        }
    }
    if (match_session_count >= config_.max_players) {
        result.reason = "match_full";
        return result;
    }

    BattleSessionRecord session;
    session.ticket_id = signed_ticket.ticket.ticket_id;
    session.match_id = signed_ticket.ticket.match_id;
    session.player_id = signed_ticket.ticket.player_id;
    session.mode_id = signed_ticket.ticket.mode_id;
    session.kcp_conv = DeriveDevKcpConv(session.match_id, session.player_id);
    session.key_id = config_.signing_key_id;
    session.session_id = session.match_id + ":" + session.player_id + ":" + session.ticket_id;
    sessions_by_ticket_[session.ticket_id] = session;

    auto simulation_it = simulations_by_match_.find(session.match_id);
    if (simulation_it == simulations_by_match_.end()) {
        SimulationConfig simulation_config;
        simulation_config.match_id = session.match_id;
        simulation_config.mode_id = session.mode_id;
        simulation_config.ruleset_version = signed_ticket.ticket.ruleset_version;
        simulation_config.match_seed = DeriveMatchSeed(session.match_id);
        simulation_config.tick_rate_hz = kBattleTickRateHz;
        simulation_it = simulations_by_match_.emplace(session.match_id, BattleSimulation(simulation_config)).first;
    } else if (
        simulation_it->second.Config().mode_id != session.mode_id ||
        simulation_it->second.Config().ruleset_version != signed_ticket.ticket.ruleset_version
    ) {
        sessions_by_ticket_.erase(session.ticket_id);
        result.reason = "match_mode_ruleset_mismatch";
        return result;
    }
    simulation_it->second.AddPlayer(
        session.player_id,
        InitialPlayerX(simulation_it->second.PlayerCount()),
        0
    );

    result.ok = true;
    result.reason = "ok";
    result.session = session;
    return result;
}

BattleHandshakeAccept BattleServer::AcceptHandshake(const BattleHandshakeHello& hello) const {
    BattleHandshakeAccept rejected;
    TicketVerificationOptions options;
    options.now_ms = config_.now_ms;
    options.required_battle_server_id = config_.server_id;
    options.required_endpoint = config_.endpoint;
    options.required_key_id = config_.signing_key_id;
    const auto verification = ticket_verifier_.Verify(hello.battle_ticket, options);
    if (!verification.ok) {
        rejected.reason = verification.reason;
        return rejected;
    }
    const auto ticket_it = sessions_by_ticket_.find(hello.battle_ticket.ticket.ticket_id);
    if (ticket_it == sessions_by_ticket_.end()) {
        rejected.reason = "ticket_not_registered";
        return rejected;
    }
    const BattleSessionRecord& session = ticket_it->second;
    if (
        session.match_id != hello.battle_ticket.ticket.match_id ||
        session.player_id != hello.battle_ticket.ticket.player_id ||
        session.mode_id != hello.battle_ticket.ticket.mode_id
    ) {
        rejected.reason = "session_ticket_mismatch";
        return rejected;
    }
    return handshake_manager_.Accept(hello, hello.battle_ticket.ticket, config_.signing_key_id);
}

DispatchResult BattleServer::Dispatch(
    const BattlePacketHeader& header,
    const std::vector<std::uint8_t>& plaintext_payload
) {
    return dispatcher_.Dispatch(header, plaintext_payload);
}

DispatchResult BattleServer::DispatchEncrypted(const BattleEncryptedPacket& packet) {
    DispatchResult result;
    result.payload_type = packet.header.payload_type;

    if (packet.ciphertext.empty()) {
        result.reason = "ciphertext_missing";
        return result;
    }
    if (packet.auth_tag.size() != 16) {
        result.reason = "auth_tag_invalid";
        return result;
    }
    if (packet.header.payload_type == BattlePayloadType::Result) {
        result.reason = "client_result_forbidden";
        return result;
    }
    if (packet.header.match_id.empty() || packet.header.player_id.empty()) {
        result.reason = "identity_missing";
        return result;
    }
    const auto simulation_it = simulations_by_match_.find(packet.header.match_id);
    if (simulation_it == simulations_by_match_.end()) {
        result.reason = "match_unknown";
        return result;
    }
    const BattleSessionRecord* session = SessionForPlayer(
        sessions_by_ticket_,
        packet.header.match_id,
        packet.header.player_id
    );
    if (session == nullptr) {
        result.reason = "player_unknown";
        return result;
    }
    if (packet.header.key_id != session->key_id) {
        result.reason = "session_key_mismatch";
        return result;
    }
    if (IsInputWindowBoundPayload(packet.header.payload_type)) {
        const BattleSimulation& simulation = simulation_it->second;
        if (packet.header.tick <= simulation.CurrentTick()) {
            result.reason = "encrypted_tick_too_old";
            return result;
        }
        if (packet.header.tick > simulation.CurrentTick() + simulation.Config().max_input_ahead_ticks) {
            result.reason = "encrypted_tick_too_far_ahead";
            return result;
        }
    }
    return dispatcher_.DispatchEncrypted(packet);
}

InputValidationResult BattleServer::AcceptInput(const BattleInput& input) {
    const auto simulation_it = simulations_by_match_.find(input.match_id);
    if (simulation_it == simulations_by_match_.end()) {
        InputValidationResult result;
        result.code = InputValidationCode::MatchUnknown;
        result.reason = "match_unknown";
        return result;
    }
    if (!SessionExistsForPlayer(sessions_by_ticket_, input.match_id, input.player_id)) {
        return UnknownPlayerResult();
    }
    return simulation_it->second.AcceptInput(input);
}

InputValidationResult BattleServer::AcceptModeAction(const BattleModeAction& action) {
    const auto simulation_it = simulations_by_match_.find(action.match_id);
    if (simulation_it == simulations_by_match_.end()) {
        InputValidationResult result;
        result.code = InputValidationCode::MatchUnknown;
        result.reason = "match_unknown";
        return result;
    }
    if (!SessionExistsForPlayer(sessions_by_ticket_, action.match_id, action.player_id)) {
        return UnknownPlayerResult();
    }
    return simulation_it->second.AcceptModeAction(action);
}

InputValidationResult BattleServer::SetPlayerConnected(
    const std::string& match_id,
    const std::string& player_id,
    bool connected
) {
    const auto simulation_it = simulations_by_match_.find(match_id);
    if (simulation_it == simulations_by_match_.end()) {
        InputValidationResult result;
        result.code = InputValidationCode::MatchUnknown;
        result.reason = "match_unknown";
        return result;
    }
    return simulation_it->second.SetPlayerConnected(player_id, connected);
}

BattleSnapshot BattleServer::TickMatch(const std::string& match_id) {
    const auto simulation_it = simulations_by_match_.find(match_id);
    if (simulation_it == simulations_by_match_.end()) {
        BattleSnapshot snapshot;
        snapshot.match_id = match_id;
        snapshot.snapshot_kind = "match_unknown";
        return snapshot;
    }
    return simulation_it->second.Tick();
}

BattleSnapshot BattleServer::MatchSnapshot(const std::string& match_id) const {
    const auto simulation_it = simulations_by_match_.find(match_id);
    if (simulation_it == simulations_by_match_.end()) {
        BattleSnapshot snapshot;
        snapshot.match_id = match_id;
        snapshot.snapshot_kind = "match_unknown";
        return snapshot;
    }
    return simulation_it->second.Snapshot();
}

BattleSnapshot BattleServer::ReconnectSnapshot(
    const std::string& match_id,
    const std::string& player_id,
    std::uint64_t last_seen_event_cursor
) const {
    const auto simulation_it = simulations_by_match_.find(match_id);
    if (simulation_it == simulations_by_match_.end()) {
        BattleSnapshot snapshot;
        snapshot.match_id = match_id;
        snapshot.snapshot_kind = "match_unknown";
        return snapshot;
    }
    if (!SessionExistsForPlayer(sessions_by_ticket_, match_id, player_id)) {
        BattleSnapshot snapshot;
        snapshot.match_id = match_id;
        snapshot.snapshot_tick = simulation_it->second.CurrentTick();
        snapshot.snapshot_kind = "player_unknown";
        snapshot.event_cursor = simulation_it->second.Summary().event_count;
        return snapshot;
    }
    return simulation_it->second.ReconnectSnapshot(player_id, last_seen_event_cursor);
}

ReplaySummary BattleServer::MatchReplaySummary(const std::string& match_id) const {
    const auto simulation_it = simulations_by_match_.find(match_id);
    if (simulation_it == simulations_by_match_.end()) {
        ReplaySummary summary;
        summary.match_id = match_id;
        return summary;
    }
    return simulation_it->second.Summary();
}

SubmitBattleResultResult BattleServer::SubmitBattleResult(const SignedBattleResult& signed_result) {
    SubmitBattleResultResult result;
    const auto simulation_it = simulations_by_match_.find(signed_result.result.match_id);
    if (simulation_it == simulations_by_match_.end()) {
        result.reason = "match_unknown";
        return result;
    }

    BattleResultVerificationOptions options;
    options.required_match_id = signed_result.result.match_id;
    options.required_mode_id = simulation_it->second.Config().mode_id;
    options.required_ruleset_version = simulation_it->second.Config().ruleset_version;
    options.required_key_id = config_.server_id;
    options.now_ms = config_.now_ms;
    const ReplaySummary summary = simulation_it->second.Summary();
    options.required_result_hash = DevSha256RefFromSummary(summary);
    options.required_replay_id = DevReplayIdFromSummary(summary);
    options.required_event_cursor = summary.event_count;
    for (const auto& item : sessions_by_ticket_) {
        const BattleSessionRecord& session = item.second;
        if (session.match_id == signed_result.result.match_id) {
            options.required_player_ids.push_back(session.player_id);
        }
    }
    result.verification = result_verifier_.Verify(signed_result, options);
    if (!result.verification.ok) {
        result.reason = result.verification.reason;
        return result;
    }

    const auto existing = result_hash_by_match_.find(signed_result.result.match_id);
    if (existing != result_hash_by_match_.end()) {
        if (existing->second == signed_result.result.result_hash) {
            result.ok = true;
            result.reason = "ok";
            result.duplicate = true;
            result.settlement_key = "battle-result:" + signed_result.result.match_id;
            return result;
        }
        result.reason = "result_replay_mismatch";
        return result;
    }

    result_hash_by_match_[signed_result.result.match_id] = signed_result.result.result_hash;
    result.ok = true;
    result.reason = "ok";
    result.settlement_key = "battle-result:" + signed_result.result.match_id;
    return result;
}

std::uint64_t BattleServer::DeriveMatchSeed(const std::string& match_id) const {
    std::uint64_t seed = 1469598103934665603ull;
    for (const char ch : match_id) {
        seed ^= static_cast<unsigned char>(ch);
        seed *= 1099511628211ull;
    }
    for (const char ch : config_.server_id) {
        seed ^= static_cast<unsigned char>(ch);
        seed *= 1099511628211ull;
    }
    return seed;
}

std::int32_t BattleServer::InitialPlayerX(std::size_t player_index) const {
    if (player_index == 0) {
        return -20000;
    }
    if (player_index == 1) {
        return 20000;
    }
    return static_cast<std::int32_t>(player_index) * 10000 - 30000;
}

}  // namespace phk::battle
