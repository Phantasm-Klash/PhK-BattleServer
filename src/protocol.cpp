#include "phk/battle/protocol.hpp"

namespace phk::battle {

namespace {

std::string PlayerSeqKey(const BattlePacketHeader& header) {
    return header.match_id + ":" + header.player_id;
}

}  // namespace

DispatchResult BattleDispatcher::Dispatch(
    const BattlePacketHeader& header,
    const std::vector<std::uint8_t>& plaintext_payload
) {
    DispatchResult result;
    result.payload_type = header.payload_type;

    if (!header.version.IsCompatible()) {
        result.reason = "version_incompatible";
        return result;
    }
    if (header.match_id.empty() || header.player_id.empty()) {
        result.reason = "identity_missing";
        return result;
    }
    if (header.seq == 0) {
        result.reason = "seq_missing";
        return result;
    }
    if (header.payload_type == BattlePayloadType::Result) {
        result.reason = "client_result_forbidden";
        return result;
    }

    const std::string seq_key = PlayerSeqKey(header);
    const auto seq_it = last_seq_by_player_.find(seq_key);
    if (seq_it != last_seq_by_player_.end() && header.seq <= seq_it->second) {
        result.reason = "seq_replay";
        return result;
    }

    const auto tick_it = last_tick_by_match_.find(header.match_id);
    if (tick_it != last_tick_by_match_.end() && header.tick > tick_it->second + 600) {
        result.reason = "tick_jump";
        return result;
    }

    last_seq_by_player_[seq_key] = header.seq;
    auto& match_tick = last_tick_by_match_[header.match_id];
    if (header.tick > match_tick) {
        match_tick = header.tick;
    }

    result.ok = true;
    result.reason = "ok";
    result.response_kind = header.payload_type == BattlePayloadType::Ping
        ? "pong"
        : PayloadTypeName(header.payload_type);
    if (plaintext_payload.empty() && header.payload_type == BattlePayloadType::Input) {
        result.response_kind = "input_empty_payload";
    } else if (plaintext_payload.empty() && header.payload_type == BattlePayloadType::ModeAction) {
        result.response_kind = "mode_action_empty_payload";
    }
    return result;
}

std::string PayloadTypeName(BattlePayloadType type) {
    switch (type) {
        case BattlePayloadType::HandshakeHello:
            return "handshake_hello";
        case BattlePayloadType::HandshakeAccept:
            return "handshake_accept";
        case BattlePayloadType::Input:
            return "input";
        case BattlePayloadType::Snapshot:
            return "snapshot";
        case BattlePayloadType::Event:
            return "event";
        case BattlePayloadType::Ping:
            return "ping";
        case BattlePayloadType::Reconnect:
            return "reconnect";
        case BattlePayloadType::Result:
            return "result";
        case BattlePayloadType::ModeAction:
            return "mode_action";
        default:
            return "unspecified";
    }
}

}  // namespace phk::battle
