#include "phk/battle/ticket.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace phk::battle {

namespace {

bool IsEmpty(const std::string& value) {
    return value.empty();
}

bool IsAllowedAuditTokenChar(char ch) {
    const auto byte = static_cast<unsigned char>(ch);
    return std::isalnum(byte) || ch == '_' || ch == '-' || ch == '.';
}

bool IsValidAuditToken(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), IsAllowedAuditTokenChar);
}

void Fail(VerificationResult& result, std::string reason) {
    result.ok = false;
    result.reason = std::move(reason);
}

}  // namespace

VerificationResult TicketVerifier::Verify(
    const SignedBattleTicket& signed_ticket,
    const TicketVerificationOptions& options
) const {
    VerificationResult result;
    const BattleTicket& ticket = signed_ticket.ticket;

    if (!ticket.version.IsCompatible()) {
        Fail(result, "version_incompatible");
        return result;
    }
    if (IsEmpty(ticket.ticket_id) || IsEmpty(ticket.match_id) || IsEmpty(ticket.user_id) ||
        IsEmpty(ticket.player_id) || IsEmpty(ticket.mode_id)) {
        Fail(result, "ticket_identity_missing");
        return result;
    }
    if (!IsValidAuditToken(ticket.player_id)) {
        Fail(result, "player_id_invalid");
        return result;
    }
    if (ticket.battle_server_id.empty() || ticket.endpoint.empty()) {
        Fail(result, "battle_endpoint_missing");
        return result;
    }
    if (!options.required_battle_server_id.empty() &&
        ticket.battle_server_id != options.required_battle_server_id) {
        Fail(result, "battle_server_mismatch");
        return result;
    }
    if (!options.required_endpoint.empty() && ticket.endpoint != options.required_endpoint) {
        Fail(result, "endpoint_mismatch");
        return result;
    }
    if (!LooksLikeSha256Ref(ticket.deck_snapshot_hash)) {
        Fail(result, "deck_snapshot_hash_invalid");
        return result;
    }
    if (ticket.ruleset_version.empty()) {
        Fail(result, "ruleset_missing");
        return result;
    }
    if (!ticket.version.ruleset_version.empty() &&
        ticket.version.ruleset_version != ticket.ruleset_version) {
        Fail(result, "ruleset_version_mismatch");
        return result;
    }
    if (ticket.ticket_nonce_hex.size() < 24 || !IsHex(ticket.ticket_nonce_hex)) {
        Fail(result, "ticket_nonce_invalid");
        return result;
    }
    if (ticket.issued_at_ms <= 0 || ticket.expires_at_ms <= ticket.issued_at_ms) {
        Fail(result, "ticket_time_invalid");
        return result;
    }
    if (options.now_ms > 0 && ticket.expires_at_ms <= options.now_ms) {
        Fail(result, "ticket_expired");
        return result;
    }
    if (LooksLikeRawBearerSession(ticket.business_session_id)) {
        Fail(result, "raw_business_session_rejected");
        return result;
    }
    if (signed_ticket.signature_alg != "ED25519") {
        Fail(result, "signature_alg_unsupported");
        return result;
    }
    if (!options.required_key_id.empty() && signed_ticket.key_id != options.required_key_id) {
        Fail(result, "signature_key_mismatch");
        return result;
    }
    if (signed_ticket.key_id.empty() || signed_ticket.signature_hex.empty() ||
        signed_ticket.public_key_hex.empty()) {
        Fail(result, "signature_fields_missing");
        return result;
    }
    if (signed_ticket.public_key_hex.size() != 64 || !IsHex(signed_ticket.public_key_hex)) {
        Fail(result, "ed25519_public_key_shape_invalid");
        return result;
    }
    if (signed_ticket.signature_hex.size() != 128 || !IsHex(signed_ticket.signature_hex)) {
        Fail(result, "ed25519_signature_shape_invalid");
        return result;
    }
    if (!signed_ticket.server_authoritative) {
        Fail(result, "ticket_not_server_authoritative");
        return result;
    }

    result.ok = true;
    result.reason = "ok";
    if (options.allow_dev_signature_shape_only) {
        result.warnings.push_back("dev_signature_shape_only");
    }
    return result;
}

bool IsHex(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isxdigit(ch) != 0;
    });
}

bool LooksLikeSha256Ref(std::string_view value) {
    constexpr std::string_view prefix = "sha256:";
    if (value.size() < prefix.size() + 3) {
        return false;
    }
    return value.substr(0, prefix.size()) == prefix;
}

bool LooksLikeRawBearerSession(std::string_view value) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered.rfind("bearer ", 0) == 0 ||
        lowered.rfind("jwt:", 0) == 0 ||
        lowered.find(".eyj") != std::string::npos;
}

std::string CanonicalTicketPayload(const BattleTicket& ticket) {
    std::ostringstream out;
    out << ticket.version.protocol_version << '|'
        << ticket.version.business_api_version << '|'
        << ticket.version.battle_api_version << '|'
        << ticket.version.ruleset_version << '|'
        << ticket.ticket_id << '|'
        << ticket.match_id << '|'
        << ticket.user_id << '|'
        << ticket.player_id << '|'
        << ticket.mode_id << '|'
        << ticket.battle_server_id << '|'
        << ticket.endpoint << '|'
        << ticket.deck_snapshot_hash << '|'
        << ticket.ruleset_version << '|'
        << ticket.ticket_nonce_hex << '|'
        << ticket.issued_at_ms << '|'
        << ticket.expires_at_ms << '|'
        << ticket.business_session_id;
    return out.str();
}

}  // namespace phk::battle
