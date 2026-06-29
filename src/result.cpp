#include "phk/battle/result.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "phk/battle/ticket.hpp"

namespace phk::battle {

namespace {

void Fail(BattleResultVerification& verification, std::string reason) {
    verification.ok = false;
    verification.reason = std::move(reason);
}

bool SameStringSet(std::vector<std::string> left, std::vector<std::string> right) {
    if (left.size() != right.size()) {
        return false;
    }
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    return left == right && std::none_of(left.begin(), left.end(), [](const std::string& value) {
        return value.empty();
    });
}

bool ContainsJsonUintField(const std::string& json, std::string_view field_name, std::uint64_t expected) {
    const std::string needle = "\"" + std::string(field_name) + "\":" + std::to_string(expected);
    const std::size_t offset = json.find(needle);
    if (offset == std::string::npos) {
        return false;
    }
    const std::size_t after_value = offset + needle.size();
    if (after_value == json.size()) {
        return true;
    }
    const char next = json[after_value];
    return next == ',' || next == '}';
}

bool ContainsJsonStringField(const std::string& json, std::string_view field_name, const std::string& expected) {
    if (expected.empty()) {
        return true;
    }
    const std::string needle = "\"" + std::string(field_name) + "\":\"" + expected + "\"";
    const std::size_t offset = json.find(needle);
    if (offset == std::string::npos) {
        return false;
    }
    const std::size_t after_value = offset + needle.size();
    if (after_value == json.size()) {
        return true;
    }
    const char next = json[after_value];
    return next == ',' || next == '}';
}

std::string LowerAscii(std::string_view value) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered;
}

bool ContainsForbiddenRewardMutation(std::string_view json) {
    const std::string lowered = LowerAscii(json);
    for (const std::string_view needle : {
        "inventory",
        "wallet",
        "currency",
        "grant",
        "reward_grant",
        "item_id",
        "balance",
        "database",
        "steam_inventory",
    }) {
        if (lowered.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

BattleResultVerification BattleResultVerifier::Verify(
    const SignedBattleResult& signed_result,
    const BattleResultVerificationOptions& options
) const {
    BattleResultVerification verification;
    const BattleResult& result = signed_result.result;

    if (!result.version.IsCompatible()) {
        Fail(verification, "version_incompatible");
        return verification;
    }
    if (result.match_id.empty() || result.mode_id.empty()) {
        Fail(verification, "result_identity_missing");
        return verification;
    }
    const std::string required_ruleset = options.required_ruleset_version.empty()
        ? std::string(kDefaultRulesetVersion)
        : options.required_ruleset_version;
    if (!result.version.ruleset_version.empty() &&
        result.version.ruleset_version != required_ruleset) {
        Fail(verification, "ruleset_version_mismatch");
        return verification;
    }
    if (!options.required_match_id.empty() && result.match_id != options.required_match_id) {
        Fail(verification, "match_mismatch");
        return verification;
    }
    if (!options.required_mode_id.empty() && result.mode_id != options.required_mode_id) {
        Fail(verification, "mode_mismatch");
        return verification;
    }
    if (!LooksLikeSha256Ref(result.result_hash)) {
        Fail(verification, "result_hash_invalid");
        return verification;
    }
    if (!options.required_result_hash.empty() && result.result_hash != options.required_result_hash) {
        Fail(verification, "result_hash_mismatch");
        return verification;
    }
    if (result.replay_id.empty()) {
        Fail(verification, "replay_id_missing");
        return verification;
    }
    if (!options.required_replay_id.empty() && result.replay_id != options.required_replay_id) {
        Fail(verification, "replay_id_mismatch");
        return verification;
    }
    if (result.player_ids.empty() || !SameStringSet(result.player_ids, options.required_player_ids)) {
        Fail(verification, "player_ids_mismatch");
        return verification;
    }
    if (options.required_event_cursor > 0 &&
        !ContainsJsonUintField(result.mode_result_json, "event_cursor", options.required_event_cursor)) {
        Fail(verification, "event_cursor_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonUintField(result.mode_result_json, "final_tick", options.required_final_tick)) {
        Fail(verification, "final_tick_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonUintField(result.mode_result_json, "input_count", options.required_input_count)) {
        Fail(verification, "input_count_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonUintField(result.mode_result_json, "fallback_input_count", options.required_fallback_input_count)) {
        Fail(verification, "fallback_input_count_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonUintField(
            result.mode_result_json,
            "neutral_fallback_count",
            options.required_neutral_fallback_count
        )) {
        Fail(verification, "neutral_fallback_count_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonUintField(
            result.mode_result_json,
            "held_input_fallback_count",
            options.required_held_input_fallback_count
        )) {
        Fail(verification, "held_input_fallback_count_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonUintField(result.mode_result_json, "mode_action_count", options.required_mode_action_count)) {
        Fail(verification, "mode_action_count_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonUintField(result.mode_result_json, "input_trace_count", options.required_input_trace_count)) {
        Fail(verification, "input_trace_count_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonUintField(result.mode_result_json, "event_trace_count", options.required_event_trace_count)) {
        Fail(verification, "event_trace_count_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonStringField(result.mode_result_json, "input_stream_hash", options.required_input_stream_hash)) {
        Fail(verification, "input_stream_hash_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonStringField(result.mode_result_json, "event_stream_hash", options.required_event_stream_hash)) {
        Fail(verification, "event_stream_hash_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonStringField(result.mode_result_json, "final_state_hash", options.required_final_state_hash)) {
        Fail(verification, "final_state_hash_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonStringField(result.mode_result_json, "replay_fixture_hash", options.required_replay_fixture_hash)) {
        Fail(verification, "replay_fixture_hash_mismatch");
        return verification;
    }
    if (result.reward_projection_json.empty()) {
        Fail(verification, "reward_projection_missing");
        return verification;
    }
    if (options.require_projection_only_reward &&
        ContainsForbiddenRewardMutation(result.reward_projection_json)) {
        Fail(verification, "reward_projection_mutation_forbidden");
        return verification;
    }
    if (result.settled_at_ms <= 0) {
        Fail(verification, "settled_at_missing");
        return verification;
    }
    if (options.now_ms > 0 && result.settled_at_ms > options.now_ms + 5 * 60 * 1000) {
        Fail(verification, "settled_at_future");
        return verification;
    }
    if (signed_result.signature_alg != "ED25519") {
        Fail(verification, "signature_alg_unsupported");
        return verification;
    }
    if (!options.required_key_id.empty() && signed_result.key_id != options.required_key_id) {
        Fail(verification, "signature_key_mismatch");
        return verification;
    }
    if (signed_result.key_id.empty() || signed_result.signature_hex.empty()) {
        Fail(verification, "signature_fields_missing");
        return verification;
    }
    if (signed_result.signature_hex.size() != 128 || !IsHex(signed_result.signature_hex)) {
        Fail(verification, "ed25519_signature_shape_invalid");
        return verification;
    }
    if (!signed_result.public_key_hex.empty() &&
        (signed_result.public_key_hex.size() != 64 || !IsHex(signed_result.public_key_hex))) {
        Fail(verification, "ed25519_public_key_shape_invalid");
        return verification;
    }
    if (!signed_result.server_authoritative) {
        Fail(verification, "result_not_server_authoritative");
        return verification;
    }

    verification.ok = true;
    verification.reason = "ok";
    if (options.allow_dev_signature_shape_only) {
        verification.warnings.push_back("dev_result_signature_shape_only");
    }
    return verification;
}

std::string CanonicalBattleResultPayload(const BattleResult& result) {
    std::ostringstream out;
    out << result.version.protocol_version << '|'
        << result.version.business_api_version << '|'
        << result.version.battle_api_version << '|'
        << result.version.ruleset_version << '|'
        << result.match_id << '|'
        << result.mode_id << '|'
        << result.result_hash << '|'
        << result.replay_id << '|';
    for (const auto& player_id : result.player_ids) {
        out << player_id << ',';
    }
    out << '|' << result.reward_projection_json << '|'
        << result.mode_result_json << '|'
        << result.settled_at_ms;
    return out.str();
}

}  // namespace phk::battle
