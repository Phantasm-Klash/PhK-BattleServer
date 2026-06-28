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
