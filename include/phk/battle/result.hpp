#pragma once

#include <cstdint>
#include <string>
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
    std::uint64_t required_input_count = 0;
    std::uint64_t required_fallback_input_count = 0;
    std::uint64_t required_neutral_fallback_count = 0;
    std::uint64_t required_held_input_fallback_count = 0;
    std::uint64_t required_mode_action_count = 0;
    std::uint64_t required_input_trace_count = 0;
    std::uint64_t required_event_trace_count = 0;
    std::int64_t now_ms = 0;
    bool allow_dev_signature_shape_only = true;
    bool require_projection_only_reward = true;
    bool require_replay_counter_fields = false;
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

}  // namespace phk::battle
