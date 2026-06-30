#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "phk/battle/version.hpp"

namespace phk::battle {

struct BattleResult {
    VersionStamp version;
    std::string match_id;
    std::string mode_id;
    std::string result_hash;
    std::string replay_id;
    std::vector<std::string> player_ids;
    std::string reward_projection_json;
    std::string mode_result_json;
    std::int64_t settled_at_ms = 0;
};

struct SignedBattleResult {
    BattleResult result;
    std::string signature_alg = "ED25519";
    std::string key_id;
    std::string signature_hex;
    std::string public_key_hex;
    bool server_authoritative = false;
};

struct BattleResultVerificationOptions {
    std::string required_match_id;
    std::string required_mode_id;
    std::string required_ruleset_version;
    std::string required_key_id;
    std::string required_result_hash;
    std::string required_replay_id;
    std::vector<std::string> required_player_ids;
    std::uint64_t required_event_cursor = 0;
    std::uint64_t required_final_tick = 0;
    std::uint64_t required_tick_rate_hz = 0;
    std::uint64_t required_input_count = 0;
    std::uint64_t required_fallback_input_count = 0;
    std::uint64_t required_neutral_fallback_count = 0;
    std::uint64_t required_held_input_fallback_count = 0;
    std::uint64_t required_mode_action_count = 0;
    std::uint64_t required_input_trace_count = 0;
    std::uint64_t required_event_trace_count = 0;
    std::string required_input_stream_hash;
    std::string required_event_stream_hash;
    std::string required_final_state_hash;
    std::string required_replay_summary_hash;
    std::string required_replay_fixture_hash;
    std::string required_boss_scope;
    std::string required_boss_completion_policy;
    std::string required_boss_friendly_fire_policy;
    std::string required_boss_clear_status;
    std::string required_boss_result_disposition;
    std::string required_last_transfer_card_instance_id;
    std::string required_last_transfer_from_player_id;
    std::string required_last_transfer_to_player_id;
    std::string required_last_transfer_authority_owner_player_id;
    std::uint64_t required_boss_max_hp = 0;
    std::uint64_t required_boss_current_hp = 0;
    std::uint64_t required_boss_damage_total = 0;
    std::uint64_t required_boss_defeated = 0;
    std::uint64_t required_boss_defeated_tick = 0;
    std::uint64_t required_boss_min_players = 0;
    std::uint64_t required_boss_max_players = 0;
    std::uint64_t required_boss_start_ready = 0;
    std::uint64_t required_boss_ready_player_count = 0;
    std::uint64_t required_boss_ready_to_start = 0;
    std::uint64_t required_connected_player_count = 0;
    std::uint64_t required_disconnected_player_count = 0;
    std::uint64_t required_transfer_card_count = 0;
    std::uint64_t required_last_transfer_authority_mode_allowed = 0;
    std::uint64_t required_last_transfer_authority_cost_paid = 0;
    std::uint64_t required_last_transfer_authority_cooldown_ready = 0;
    std::map<std::string, std::uint64_t> required_boss_damage_by_player;
    std::int64_t now_ms = 0;
    bool allow_dev_signature_shape_only = true;
    bool require_dev_signature_payload_binding = true;
    bool require_projection_only_reward = true;
    bool require_replay_counter_fields = false;
    bool require_boss_result_fields = false;
    bool require_transfer_result_fields = false;
};

struct BattleResultVerification {
    bool ok = false;
    std::string reason;
    std::vector<std::string> warnings;
};

class BattleResultVerifier {
public:
    BattleResultVerification Verify(
        const SignedBattleResult& signed_result,
        const BattleResultVerificationOptions& options
    ) const;
};

[[nodiscard]] std::string CanonicalBattleResultPayload(const BattleResult& result);
[[nodiscard]] std::string DevBattleResultSignatureHex(
    const BattleResult& result,
    std::string_view key_id
);

}  // namespace phk::battle
