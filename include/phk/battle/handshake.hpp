#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "phk/battle/ticket.hpp"

namespace phk::battle {

struct BattleHandshakeHello {
    VersionStamp version;
    SignedBattleTicket battle_ticket;
    std::array<std::uint8_t, 32> client_x25519_pub{};
    std::array<std::uint8_t, 32> client_random{};
    std::vector<std::string> supported_aead;
};

struct BattleHandshakeAccept {
    bool ok = false;
    std::string reason;
    VersionStamp version;
    std::string match_id;
    std::string player_id;
    std::array<std::uint8_t, 32> server_x25519_pub{};
    std::array<std::uint8_t, 32> server_random{};
    std::string selected_aead;
    std::uint32_t kcp_conv = 0;
    std::string key_id;
    std::string transcript_hash_hex;
    std::string client_to_server_key_ref;
    std::string server_to_client_key_ref;
    std::string server_signature_alg = "ED25519";
    std::string server_signature_hex;
    std::string dev_session_id;
};

class HandshakeManager {
public:
    [[nodiscard]] BattleHandshakeAccept Accept(
        const BattleHandshakeHello& hello,
        const BattleTicket& verified_ticket,
        std::string_view server_key_id
    ) const;
};

[[nodiscard]] std::uint32_t DeriveDevKcpConv(std::string_view match_id, std::string_view player_id);
[[nodiscard]] std::string DevTranscriptHash(
    const BattleHandshakeHello& hello,
    const BattleTicket& verified_ticket
);
[[nodiscard]] std::string DevHandshakeKeyRef(
    std::string_view transcript_hash_hex,
    std::string_view direction_label
);
[[nodiscard]] std::string DevHandshakeServerSignature(
    std::string_view transcript_hash_hex,
    std::string_view server_key_id
);

}  // namespace phk::battle
