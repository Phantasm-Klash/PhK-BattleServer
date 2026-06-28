#include "phk/battle/server.hpp"

#include <utility>

namespace phk::battle {

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
        simulation_config.match_seed = DeriveMatchSeed(session.match_id);
        simulation_config.tick_rate_hz = kBattleTickRateHz;
        simulation_it = simulations_by_match_.emplace(session.match_id, BattleSimulation(simulation_config)).first;
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
    return handshake_manager_.Accept(hello, hello.battle_ticket.ticket, config_.signing_key_id);
}

DispatchResult BattleServer::Dispatch(
    const BattlePacketHeader& header,
    const std::vector<std::uint8_t>& plaintext_payload
) {
    return dispatcher_.Dispatch(header, plaintext_payload);
}

InputValidationResult BattleServer::AcceptInput(const BattleInput& input) {
    const auto simulation_it = simulations_by_match_.find(input.match_id);
    if (simulation_it == simulations_by_match_.end()) {
        InputValidationResult result;
        result.code = InputValidationCode::MatchUnknown;
        result.reason = "match_unknown";
        return result;
    }
    return simulation_it->second.AcceptInput(input);
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

    BattleResultVerificationOptions options;
    options.required_match_id = signed_result.result.match_id;
    options.required_mode_id = signed_result.result.mode_id;
    options.required_key_id = config_.server_id;
    options.now_ms = config_.now_ms;
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
