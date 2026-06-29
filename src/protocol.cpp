#include "phk/battle/protocol.hpp"

#include "phk/battle/ticket.hpp"

namespace phk::battle {

namespace {

std::string PlayerSeqKey(const BattlePacketHeader& header) {
    return header.match_id + ":" + header.player_id;
}

bool RequiresEncryptedPacketShape(BattlePayloadType payload_type) {
    return payload_type == BattlePayloadType::Input ||
        payload_type == BattlePayloadType::ModeAction ||
        payload_type == BattlePayloadType::Ping ||
        payload_type == BattlePayloadType::Reconnect;
}

bool HasExpectedAeadTagShape(const std::vector<std::uint8_t>& auth_tag) {
    return auth_tag.size() == 16;
}

std::string NonceReplayKey(const BattlePacketHeader& header) {
    return header.match_id + ":" + header.player_id + ":" + header.key_id + ":" + header.nonce_hex;
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
    if (header.payload_type == BattlePayloadType::Unspecified) {
        result.reason = "payload_type_missing";
        return result;
    }
    if (RequiresEncryptedPacketShape(header.payload_type)) {
        if (header.key_id.empty()) {
            result.reason = "key_id_missing";
            return result;
        }
        if (header.nonce_hex.size() < 24 || !IsHex(header.nonce_hex)) {
            result.reason = "nonce_invalid";
            return result;
        }
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

DispatchResult BattleDispatcher::DispatchEncrypted(const BattleEncryptedPacket& packet) {
    DispatchResult result;
    result.payload_type = packet.header.payload_type;

    if (packet.ciphertext.empty()) {
        result.reason = "ciphertext_missing";
        return result;
    }
    if (!HasExpectedAeadTagShape(packet.auth_tag)) {
        result.reason = "auth_tag_invalid";
        return result;
    }
    if (!RequiresEncryptedPacketShape(packet.header.payload_type)) {
        result.reason = packet.header.payload_type == BattlePayloadType::Result
            ? "client_result_forbidden"
            : "encrypted_payload_type_invalid";
        return result;
    }

    if (!packet.header.match_id.empty() &&
        !packet.header.player_id.empty() &&
        !packet.header.key_id.empty() &&
        packet.header.nonce_hex.size() >= 24 &&
        IsHex(packet.header.nonce_hex)) {
        const std::string nonce_key = NonceReplayKey(packet.header);
        if (seen_encrypted_nonces_.find(nonce_key) != seen_encrypted_nonces_.end()) {
            result.reason = "nonce_replay";
            return result;
        }

        DispatchResult dispatched = Dispatch(packet.header, packet.ciphertext);
        if (!dispatched.ok) {
            return dispatched;
        }
        seen_encrypted_nonces_.insert(nonce_key);
        if (dispatched.response_kind == "input_empty_payload") {
            dispatched.response_kind = "input_encrypted";
        } else if (dispatched.response_kind == "mode_action_empty_payload") {
            dispatched.response_kind = "mode_action_encrypted";
        }
        return dispatched;
    }

    DispatchResult dispatched = Dispatch(packet.header, packet.ciphertext);
    if (dispatched.ok && dispatched.response_kind == "input_empty_payload") {
        dispatched.response_kind = "input_encrypted";
    } else if (dispatched.ok && dispatched.response_kind == "mode_action_empty_payload") {
        dispatched.response_kind = "mode_action_encrypted";
    }
    return dispatched;
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
