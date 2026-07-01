#include "phk/battle/protocol.hpp"

#include "phk/battle/ticket.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

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

bool HasExpectedAeadNonceShape(const std::string& nonce_hex) {
    return nonce_hex.size() == 24 && IsHex(nonce_hex);
}

std::string LowerAscii(std::string_view value) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered;
}

bool LooksLikeJsonObject(std::string_view value) {
    auto first = value.begin();
    while (first != value.end() && std::isspace(static_cast<unsigned char>(*first))) {
        ++first;
    }
    auto last = value.end();
    while (last != first && std::isspace(static_cast<unsigned char>(*(last - 1)))) {
        --last;
    }
    return first != last && *first == '{' && *(last - 1) == '}';
}

bool ContainsClientAuthoredAuthorityField(std::string_view payload_json) {
    const std::string lowered = LowerAscii(payload_json);
    for (const std::string_view needle : {
        "x_milli",
        "y_milli",
        "position",
        "damage",
        "hp_delta",
        "current_hp",
        "max_hp",
        "boss_hp",
        "boss_current_hp",
        "boss_max_hp",
        "boss_damage",
        "score",
        "rank",
        "reward",
        "inventory",
        "wallet",
        "currency",
        "grant",
        "item_id",
        "balance",
        "database",
        "steam_inventory",
        "result_hash",
        "battle_result",
        "settlement",
    }) {
        if (lowered.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool IsAllowedModeActionType(std::string_view action_type) {
    return action_type == "cast_card" ||
        action_type == "select_round_card" ||
        action_type == "transfer_card" ||
        action_type == "ready" ||
        action_type == "reconnect";
}

std::string ExtractJsonStringField(std::string_view payload_json, std::string_view field_name) {
    const std::string prefix = "\"" + std::string(field_name) + "\":\"";
    const auto value_start = payload_json.find(prefix);
    if (value_start == std::string_view::npos) {
        return "";
    }
    const auto string_start = value_start + prefix.size();
    std::string decoded;
    for (std::size_t index = string_start; index < payload_json.size(); ++index) {
        const char ch = payload_json[index];
        if (ch == '"') {
            return decoded;
        }
        if (ch != '\\') {
            decoded.push_back(ch);
            continue;
        }
        ++index;
        if (index >= payload_json.size()) {
            return "";
        }
        const char escaped = payload_json[index];
        switch (escaped) {
            case '"':
            case '\\':
            case '/':
                decoded.push_back(escaped);
                break;
            case 'b':
                decoded.push_back('\b');
                break;
            case 'f':
                decoded.push_back('\f');
                break;
            case 'n':
                decoded.push_back('\n');
                break;
            case 'r':
                decoded.push_back('\r');
                break;
            case 't':
                decoded.push_back('\t');
                break;
            default:
                return "";
        }
    }
    return "";
}

bool JsonBoolFieldIsTrue(std::string_view payload_json, std::string_view field_name) {
    const std::string prefix = "\"" + std::string(field_name) + "\":";
    const auto value_start = payload_json.find(prefix);
    if (value_start == std::string_view::npos) {
        return false;
    }
    auto token_start = value_start + prefix.size();
    while (token_start < payload_json.size() &&
        std::isspace(static_cast<unsigned char>(payload_json[token_start]))) {
        ++token_start;
    }
    return payload_json.substr(token_start, 4) == "true";
}

bool ValidatePlaintextModeActionPayload(std::string_view payload_json, std::string& reason) {
    if (payload_json.empty() || !LooksLikeJsonObject(payload_json)) {
        return true;
    }
    const std::string action_type = ExtractJsonStringField(payload_json, "action_type");
    if (!action_type.empty() && !IsAllowedModeActionType(action_type)) {
        reason = "mode_action_type_unsupported";
        return false;
    }
    if (JsonBoolFieldIsTrue(payload_json, "client_result_authoritative")) {
        reason = "mode_action_client_result_forbidden";
        return false;
    }
    if (ContainsClientAuthoredAuthorityField(payload_json)) {
        reason = "mode_action_authority_field_forbidden";
        return false;
    }
    return true;
}

std::uint64_t HashAppend(std::uint64_t hash, std::string_view value) {
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

std::string Hex64(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

std::string NonceReplayKey(const BattlePacketHeader& header) {
    return header.match_id + ":" + header.player_id + ":" + header.key_id + ":" + header.nonce_hex;
}

}  // namespace

DispatchResult BattleDispatcher::Dispatch(
    const BattlePacketHeader& header,
    const std::vector<std::uint8_t>& plaintext_payload
) {
    return DispatchWithPayloadValidation(header, plaintext_payload, true);
}

DispatchResult BattleDispatcher::DispatchWithPayloadValidation(
    const BattlePacketHeader& header,
    const std::vector<std::uint8_t>& plaintext_payload,
    bool validate_plaintext_mode_action
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
    if (validate_plaintext_mode_action && header.payload_type == BattlePayloadType::ModeAction) {
        const std::string payload_json(plaintext_payload.begin(), plaintext_payload.end());
        if (!ValidatePlaintextModeActionPayload(payload_json, result.reason)) {
            return result;
        }
    }
    if (RequiresEncryptedPacketShape(header.payload_type)) {
        if (header.key_id.empty()) {
            result.reason = "key_id_missing";
            return result;
        }
        if (!HasExpectedAeadNonceShape(header.nonce_hex)) {
            result.reason = "nonce_invalid";
            return result;
        }
        if (header.nonce_hex != DevAeadNonceHex(header)) {
            result.reason = "nonce_mismatch";
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
        HasExpectedAeadNonceShape(packet.header.nonce_hex)) {
        const std::string nonce_key = NonceReplayKey(packet.header);
        if (seen_encrypted_nonces_.find(nonce_key) != seen_encrypted_nonces_.end()) {
            result.reason = "nonce_replay";
            return result;
        }

        DispatchResult dispatched = DispatchWithPayloadValidation(packet.header, packet.ciphertext, false);
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

    DispatchResult dispatched = DispatchWithPayloadValidation(packet.header, packet.ciphertext, false);
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

std::string DevAeadNonceHex(const BattlePacketHeader& header, std::string_view direction_label) {
    std::uint64_t first = 1469598103934665603ull;
    first = HashAppend(first, direction_label);
    first = HashAppend(first, header.key_id);
    first = HashAppend(first, header.match_id);
    first = HashAppend(first, header.player_id);
    first = HashAppend(first, static_cast<std::uint64_t>(header.payload_type));
    first = HashAppend(first, header.tick);
    first = HashAppend(first, header.seq);
    first = HashAppend(first, header.ack);

    std::uint64_t second = HashAppend(first, "nonce-tail");
    return (Hex64(first) + Hex64(second)).substr(0, 24);
}

}  // namespace phk::battle
