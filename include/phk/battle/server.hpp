#pragma once

#include <map>
#include <string>

#include "phk/battle/handshake.hpp"
#include "phk/battle/protocol.hpp"
#include "phk/battle/result.hpp"
#include "phk/battle/simulation.hpp"

namespace phk::battle {

struct BattleServerConfig {
    std::string server_id = "battle-local-1";
    std::string endpoint = "127.0.0.1:7901";
    std::string build_id = "dev-build";
    std::string signing_key_id = "dev-ed25519-local";
    std::int64_t now_ms = 0;
    std::uint32_t max_players = 8;
};

struct BattleSessionRecord {
    std::string session_id;
    std::string ticket_id;
    std::string match_id;
    std::string player_id;
    std::string mode_id;
    std::uint32_t kcp_conv = 0;
    std::string key_id;
    std::string server_to_client_key_id;
    std::string handshake_transcript_hash;
    std::string selected_aead;
    bool handshake_accepted = false;
};

struct RegisterTicketResult {
	bool ok = false;
	std::string reason;
	BattleSessionRecord session;
	VerificationResult verification;
};

struct SubmitBattleResultResult {
	bool ok = false;
	std::string reason;
	BattleResultVerification verification;
	std::string settlement_key;
	bool duplicate = false;
};

struct BuildSignedBattleResultResult {
	bool ok = false;
	std::string reason;
	SignedBattleResult signed_result;
	ReplaySummary replay_summary;
};

class BattleServer {
public:
	explicit BattleServer(BattleServerConfig config);

    [[nodiscard]] const BattleServerConfig& Config() const;
    [[nodiscard]] std::size_t ActiveSessionCount() const;

	RegisterTicketResult RegisterTicket(const SignedBattleTicket& signed_ticket);
	BattleHandshakeAccept AcceptHandshake(const BattleHandshakeHello& hello);
	DispatchResult Dispatch(const BattlePacketHeader& header, const std::vector<std::uint8_t>& plaintext_payload);
	DispatchResult DispatchEncrypted(const BattleEncryptedPacket& packet);
	InputValidationResult AcceptDecodedInput(const BattlePacketHeader& header, const BattleInput& input);
	InputValidationResult AcceptDecodedModeAction(const BattlePacketHeader& header, const BattleModeAction& action);
	InputValidationResult AcceptInput(const BattleInput& input);
	InputValidationResult AcceptModeAction(const BattleModeAction& action);
	InputValidationResult SetPlayerConnected(const std::string& match_id, const std::string& player_id, bool connected);
	BattleSnapshot TickMatch(const std::string& match_id);
	BattleSnapshot MatchSnapshot(const std::string& match_id) const;
	BattleSnapshot ReconnectSnapshot(
		const std::string& match_id,
		const std::string& player_id,
		std::uint64_t last_seen_event_cursor
	) const;
	ReplaySummary MatchReplaySummary(const std::string& match_id) const;
	BuildSignedBattleResultResult BuildSignedBattleResult(const std::string& match_id) const;
	SubmitBattleResultResult SubmitBattleResult(const SignedBattleResult& signed_result);

private:
	[[nodiscard]] std::uint64_t DeriveMatchSeed(const std::string& match_id) const;
	[[nodiscard]] std::int32_t InitialPlayerX(std::size_t player_index) const;

	BattleServerConfig config_;
	TicketVerifier ticket_verifier_;
	BattleResultVerifier result_verifier_;
	HandshakeManager handshake_manager_;
	BattleDispatcher dispatcher_;
	std::map<std::string, BattleSessionRecord> sessions_by_ticket_;
	std::map<std::string, BattleSimulation> simulations_by_match_;
	std::map<std::string, std::string> result_hash_by_match_;
};

}  // namespace phk::battle
