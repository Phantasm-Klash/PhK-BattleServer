#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "phk/battle/version.hpp"

namespace phk::battle {

struct BattleTicket {
    VersionStamp version;
    std::string ticket_id;
    std::string match_id;
    std::string user_id;
    std::string player_id;
    std::string mode_id;
    std::string battle_server_id;
    std::string endpoint;
    std::string deck_snapshot_hash;
    std::string ruleset_version;
    std::string ticket_nonce_hex;
    std::int64_t issued_at_ms = 0;
    std::int64_t expires_at_ms = 0;
    std::string business_session_id;
    // W-1 opening loadout (proto BattleTicket fields 15/16/17). hand_card_ids
    // carries 0..kCardHandLimit catalog card ids; initial_energy is the opening
    // energy; server_seed_decimal is the authoritative lobby seed as DECIMAL
    // TEXT (a JS number would narrow seeds above 2^53). The lobby ED25519
    // signature covers these fields on the wire.
    std::vector<std::string> hand_card_ids;
    double initial_energy = 0.0;
    std::string server_seed_decimal;
};

struct SignedBattleTicket {
    BattleTicket ticket;
    std::string signature_alg = "ED25519";
    std::string key_id;
    std::string signature_hex;
    std::string public_key_hex;
    bool server_authoritative = false;
};

struct TicketVerificationOptions {
    std::int64_t now_ms = 0;
    std::string required_battle_server_id;
    std::string required_endpoint;
    std::string required_key_id;
    bool allow_dev_signature_shape_only = true;
};

struct VerificationResult {
    bool ok = false;
    std::string reason;
    std::vector<std::string> warnings;
};

class TicketVerifier {
public:
    VerificationResult Verify(
        const SignedBattleTicket& signed_ticket,
        const TicketVerificationOptions& options
    ) const;
};

[[nodiscard]] bool IsHex(std::string_view value);
[[nodiscard]] bool LooksLikeSha256Ref(std::string_view value);
[[nodiscard]] bool LooksLikeRawBearerSession(std::string_view value);
[[nodiscard]] std::string CanonicalTicketPayload(const BattleTicket& ticket);

}  // namespace phk::battle
