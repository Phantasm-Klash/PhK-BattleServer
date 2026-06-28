#include "phk/battle/handshake.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace phk::battle {

namespace {

std::uint64_t Fnv1a(std::string_view value) {
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::array<std::uint8_t, 32> FillDevBytes(std::string_view seed) {
    std::array<std::uint8_t, 32> bytes{};
    std::uint64_t state = Fnv1a(seed);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        bytes[i] = static_cast<std::uint8_t>((state >> ((i % 8) * 8)) & 0xffu);
    }
    return bytes;
}

std::string Hex64(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

bool SupportsAead(const std::vector<std::string>& values, std::string_view name) {
    for (const auto& value : values) {
        if (value == name) {
            return true;
        }
    }
    return false;
}

bool HasAnyNonZero(const std::array<std::uint8_t, 32>& values) {
    return std::any_of(values.begin(), values.end(), [](std::uint8_t value) {
        return value != 0;
    });
}

}  // namespace

BattleHandshakeAccept HandshakeManager::Accept(
    const BattleHandshakeHello& hello,
    const BattleTicket& verified_ticket,
    std::string_view server_key_id
) const {
    BattleHandshakeAccept accept;
    if (!HasAnyNonZero(hello.client_x25519_pub)) {
        accept.reason = "client_key_missing";
        return accept;
    }
    if (!HasAnyNonZero(hello.client_random)) {
        accept.reason = "client_random_missing";
        return accept;
    }
    if (!SupportsAead(hello.supported_aead, "CHACHA20_POLY1305") &&
        !SupportsAead(hello.supported_aead, "XCHACHA20_POLY1305")) {
        accept.reason = "aead_unsupported";
        return accept;
    }
    accept.ok = true;
    accept.reason = "ok";
    accept.version = verified_ticket.version;
    accept.match_id = verified_ticket.match_id;
    accept.player_id = verified_ticket.player_id;
    accept.server_x25519_pub = FillDevBytes(verified_ticket.match_id + ":server-pub");
    accept.server_random = FillDevBytes(verified_ticket.ticket_nonce_hex + ":server-random");
    accept.selected_aead = SupportsAead(hello.supported_aead, "CHACHA20_POLY1305")
        ? "CHACHA20_POLY1305"
        : "XCHACHA20_POLY1305";
    accept.kcp_conv = DeriveDevKcpConv(verified_ticket.match_id, verified_ticket.player_id);
    accept.key_id = std::string(server_key_id);
    accept.transcript_hash_hex = DevTranscriptHash(hello, verified_ticket);
    accept.dev_session_id = verified_ticket.match_id + ":" + verified_ticket.player_id + ":" +
        Hex64(Fnv1a(accept.transcript_hash_hex));
    return accept;
}

std::uint32_t DeriveDevKcpConv(std::string_view match_id, std::string_view player_id) {
    const std::uint64_t hash = Fnv1a(std::string(match_id) + ":" + std::string(player_id));
    return static_cast<std::uint32_t>(hash & 0xffffffffu);
}

std::string DevTranscriptHash(
    const BattleHandshakeHello& hello,
    const BattleTicket& verified_ticket
) {
    std::ostringstream input;
    input << CanonicalTicketPayload(verified_ticket) << '|'
          << hello.supported_aead.size() << '|'
          << static_cast<int>(hello.client_x25519_pub[0]) << '|'
          << static_cast<int>(hello.client_random[0]);
    const std::uint64_t first = Fnv1a(input.str());
    const std::uint64_t second = Fnv1a(input.str() + ":2");
    return Hex64(first) + Hex64(second);
}

}  // namespace phk::battle
