#pragma once

#include <map>
#include <set>
#include <string>
#include <utility>

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

struct BossMatchConfig {
    std::string match_id;
    std::string mode_id;
    std::string boss_instance_id;
    std::string boss_season_id;
    std::string boss_phase_id;
    std::uint64_t boss_max_hp = 1000;
    std::string boss_friendly_fire_policy = "disabled";
};

struct ConfigureBossMatchResult {
    bool ok = false;
    std::string reason;
    std::size_t active_sessions_before = 0;
    std::size_t active_matches_before = 0;
    std::size_t pending_boss_configs_before = 0;
    std::size_t active_sessions_after = 0;
    std::size_t active_matches_after = 0;
    std::size_t pending_boss_configs_after = 0;
};

struct BattleSessionRecord {
    std::string session_id;
    std::string ticket_id;
    std::string match_id;
    std::string user_id;
    std::string player_id;
    std::string mode_id;
    std::string deck_snapshot_hash;
    std::string ruleset_version;
    std::uint32_t kcp_conv = 0;
    std::string key_id;
    std::string server_to_client_key_id;
    std::string handshake_transcript_hash;
    std::string selected_aead;
    bool handshake_accepted = false;
};

struct EncryptedSessionValidation {
	bool ok = false;
	std::string reason;
	const BattleSessionRecord* session = nullptr;
};

struct RegisterTicketResult {
	bool ok = false;
	std::string reason;
	BattleSessionRecord session;
	VerificationResult verification;
	std::size_t active_sessions_before = 0;
	std::size_t active_matches_before = 0;
	std::size_t pending_boss_configs_before = 0;
	std::size_t active_sessions_after = 0;
	std::size_t active_matches_after = 0;
	std::size_t pending_boss_configs_after = 0;
	std::size_t match_session_count_before = 0;
	std::size_t match_session_count_after = 0;
	bool created_match = false;
};

struct SubmitBattleResultResult {
	bool ok = false;
	std::string reason;
	BattleResultVerification verification;
	std::string settlement_key;
	bool duplicate = false;
};

struct RetireMatchResult {
	bool ok = false;
	std::string reason;
	std::string match_id;
	std::string result_hash;
	std::string input_stream_hash;
	std::string event_stream_hash;
	std::string final_state_hash;
	std::uint64_t final_tick = 0;
	std::uint64_t input_count = 0;
	std::uint64_t event_count = 0;
	std::size_t active_sessions_before = 0;
	std::size_t active_matches_before = 0;
	std::size_t pending_boss_configs_before = 0;
	std::size_t removed_sessions = 0;
	std::size_t active_sessions_after = 0;
	std::size_t active_matches_after = 0;
	std::size_t pending_boss_configs_after = 0;
	bool already_retired = false;
};

struct CancelMatchResult {
	bool ok = false;
	std::string reason;
	std::string match_id;
	std::size_t active_sessions_before = 0;
	std::size_t active_matches_before = 0;
	std::size_t pending_boss_configs_before = 0;
	std::size_t removed_sessions = 0;
	std::size_t active_sessions_after = 0;
	std::size_t active_matches_after = 0;
	std::size_t pending_boss_configs_after = 0;
	bool removed_match = false;
	bool removed_pending_boss_config = false;
	bool already_cancelled = false;
};

struct BuildSignedBattleResultResult {
	bool ok = false;
	std::string reason;
	SignedBattleResult signed_result;
	ReplaySummary replay_summary;
};

struct BuildReplayRecordResult {
	bool ok = false;
	std::string reason;
	ReplayRecordBridge replay_record;
	std::string replay_record_hash;
};

enum class DecodedBattlePayloadKind {
	None,
	Input,
	ModeAction,
};

struct DecodedBattlePacket {
	BattleEncryptedPacket encrypted_packet;
	DecodedBattlePayloadKind decoded_payload_kind = DecodedBattlePayloadKind::None;
	BattleInput decoded_input;
	BattleModeAction decoded_mode_action;
};

struct DecodedBattlePacketResult {
	bool ok = false;
	std::string reason;
	bool encrypted_dispatch_accepted = false;
	DispatchResult dispatch;
	InputValidationResult decoded;
};

class BattleServer {
public:
	explicit BattleServer(BattleServerConfig config);

    [[nodiscard]] const BattleServerConfig& Config() const;
	[[nodiscard]] std::size_t ActiveSessionCount() const;
    [[nodiscard]] std::size_t ActiveMatchCount() const;

    ConfigureBossMatchResult ConfigureBossMatch(BossMatchConfig boss_config);
	RegisterTicketResult RegisterTicket(const SignedBattleTicket& signed_ticket);
	BattleHandshakeAccept AcceptHandshake(const BattleHandshakeHello& hello);
	DispatchResult Dispatch(const BattlePacketHeader& header, const std::vector<std::uint8_t>& plaintext_payload);
	DispatchResult DispatchEncrypted(const BattleEncryptedPacket& packet);
	InputValidationResult AcceptDecodedInput(const BattlePacketHeader& header, const BattleInput& input);
	InputValidationResult AcceptDecodedModeAction(const BattlePacketHeader& header, const BattleModeAction& action);
	InputValidationResult AcceptDecodedReconnectModeAction(
		const BattlePacketHeader& header,
		const BattleModeAction& action
	);
	bool ConfigureTransferableCard(const std::string& match_id, TransferableCardState card);
	InputValidationResult AcceptInput(const BattleInput& input);
	InputValidationResult AcceptModeAction(const BattleModeAction& action);
	[[nodiscard]] bool IsPlayerConnected(const std::string& match_id, const std::string& player_id) const;
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
	BuildReplayRecordResult BuildReplayRecord(
		const std::string& match_id,
		std::string owner_user_id = "",
		std::string stage_id = ""
	) const;
	SubmitBattleResultResult SubmitBattleResult(const SignedBattleResult& signed_result);
	CancelMatchResult CancelMatch(const std::string& match_id);
	RetireMatchResult RetireMatch(const std::string& match_id);

private:
	[[nodiscard]] EncryptedSessionValidation ValidateEncryptedSession(
		const BattlePacketHeader& header
	) const;
	[[nodiscard]] InputValidationResult ValidateDecodedSessionBoundary(
		const BattlePacketHeader& header
	) const;
	[[nodiscard]] std::uint64_t DeriveMatchSeed(const std::string& match_id) const;
	[[nodiscard]] std::pair<std::int32_t, std::int32_t> InitialPlayerPosition(
		const std::string& mode_id,
		std::size_t player_index
	) const;
	void UpdateBossRosterLock(const std::string& match_id, const BattleSimulation& simulation);

	BattleServerConfig config_;
	TicketVerifier ticket_verifier_;
	BattleResultVerifier result_verifier_;
	HandshakeManager handshake_manager_;
	BattleDispatcher dispatcher_;
	std::map<std::string, BattleSessionRecord> sessions_by_ticket_;
	std::map<std::string, BattleSimulation> simulations_by_match_;
	std::map<std::string, BossMatchConfig> pending_boss_config_by_match_;
	std::map<std::string, std::string> result_hash_by_match_;
	std::set<std::string> cancelled_match_ids_;
	std::set<std::string> boss_roster_locked_match_ids_;
};

class DecodedBattlePacketAdapter final {
public:
	explicit DecodedBattlePacketAdapter(BattleServer& server);

	DecodedBattlePacketResult AcceptDecodedPacket(const DecodedBattlePacket& packet);

private:
	BattleServer& server_;
};

}  // namespace phk::battle
