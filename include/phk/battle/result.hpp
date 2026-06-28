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
    std::string required_key_id;
    std::vector<std::string> required_player_ids;
    std::int64_t now_ms = 0;
    bool allow_dev_signature_shape_only = true;
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
