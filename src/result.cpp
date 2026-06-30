#include "phk/battle/result.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
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

bool ContainsForbiddenModeResultMutation(std::string_view json) {
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
        "client_result_authoritative",
    }) {
        if (lowered.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::uint64_t HashAppend(std::uint64_t hash, std::string_view value) {
    for (const char ch : value) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t HashAppend(std::uint64_t hash, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        hash ^= static_cast<unsigned char>((value >> shift) & 0xffu);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string Hex64(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

std::string DevHexMaterial(std::string_view seed, std::size_t hex_chars) {
    std::string out;
    std::uint64_t counter = 0;
    while (out.size() < hex_chars) {
        std::uint64_t hash = 1469598103934665603ull;
        hash = HashAppend(hash, seed);
        hash = HashAppend(hash, counter);
        out += Hex64(hash);
        ++counter;
    }
    out.resize(hex_chars);
    return out;
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
    if ((options.required_event_cursor > 0 || options.require_replay_counter_fields) &&
        !ContainsJsonUintField(result.mode_result_json, "event_cursor", options.required_event_cursor)) {
        Fail(verification, "event_cursor_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonStringField(
            result.mode_result_json,
            "battle_result_owner",
            options.required_battle_result_owner
        )) {
        Fail(verification, "battle_result_owner_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonUintField(result.mode_result_json, "final_tick", options.required_final_tick)) {
        Fail(verification, "final_tick_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonUintField(result.mode_result_json, "tick_rate_hz", options.required_tick_rate_hz)) {
        Fail(verification, "tick_rate_hz_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonUintField(result.mode_result_json, "match_seed", options.required_match_seed)) {
        Fail(verification, "match_seed_mismatch");
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
        !ContainsJsonStringField(result.mode_result_json, "replay_summary_hash", options.required_replay_summary_hash)) {
        Fail(verification, "replay_summary_hash_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonStringField(result.mode_result_json, "replay_fixture_hash", options.required_replay_fixture_hash)) {
        Fail(verification, "replay_fixture_hash_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonUintField(result.mode_result_json, "final_snapshot_tick", options.required_final_snapshot_tick)) {
        Fail(verification, "final_snapshot_tick_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonStringField(
            result.mode_result_json,
            "final_snapshot_kind",
            options.required_final_snapshot_kind
        )) {
        Fail(verification, "final_snapshot_kind_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonStringField(
            result.mode_result_json,
            "final_snapshot_state_hash",
            options.required_final_snapshot_state_hash
        )) {
        Fail(verification, "final_snapshot_state_hash_mismatch");
        return verification;
    }
    if (options.require_replay_counter_fields &&
        !ContainsJsonUintField(
            result.mode_result_json,
            "final_snapshot_event_cursor",
            options.required_final_snapshot_event_cursor
        )) {
        Fail(verification, "final_snapshot_event_cursor_mismatch");
        return verification;
    }
    if (options.require_boss_result_fields &&
        !ContainsJsonStringField(result.mode_result_json, "boss_scope", options.required_boss_scope)) {
        Fail(verification, "boss_scope_mismatch");
        return verification;
    }
    if (options.require_boss_result_fields &&
        !ContainsJsonStringField(result.mode_result_json, "boss_completion_policy", options.required_boss_completion_policy)) {
        Fail(verification, "boss_completion_policy_mismatch");
        return verification;
    }
    if (options.require_boss_result_fields &&
        !ContainsJsonStringField(
            result.mode_result_json,
            "boss_friendly_fire_policy",
            options.required_boss_friendly_fire_policy
        )) {
        Fail(verification, "boss_friendly_fire_policy_mismatch");
        return verification;
    }
    if (options.require_boss_result_fields &&
        !ContainsJsonUintField(result.mode_result_json, "boss_min_players", options.required_boss_min_players)) {
        Fail(verification, "boss_min_players_mismatch");
        return verification;
    }
    if (options.require_boss_result_fields &&
        !ContainsJsonUintField(result.mode_result_json, "boss_max_players", options.required_boss_max_players)) {
        Fail(verification, "boss_max_players_mismatch");
        return verification;
    }
    if (options.require_boss_result_fields &&
        !ContainsJsonUintField(result.mode_result_json, "boss_start_ready", options.required_boss_start_ready)) {
        Fail(verification, "boss_start_ready_mismatch");
        return verification;
    }
    if (options.require_boss_result_fields &&
        !ContainsJsonUintField(
            result.mode_result_json,
            "boss_ready_player_count",
            options.required_boss_ready_player_count
        )) {
        Fail(verification, "boss_ready_player_count_mismatch");
        return verification;
    }
    if (options.require_boss_result_fields &&
        !ContainsJsonUintField(result.mode_result_json, "boss_ready_to_start", options.required_boss_ready_to_start)) {
        Fail(verification, "boss_ready_to_start_mismatch");
        return verification;
    }
    if (options.require_boss_result_fields &&
        !ContainsJsonUintField(
            result.mode_result_json,
            "connected_player_count",
            options.required_connected_player_count
        )) {
        Fail(verification, "connected_player_count_mismatch");
        return verification;
    }
    if (options.require_boss_result_fields &&
        !ContainsJsonUintField(
            result.mode_result_json,
            "disconnected_player_count",
            options.required_disconnected_player_count
        )) {
        Fail(verification, "disconnected_player_count_mismatch");
        return verification;
    }
    if (options.require_boss_result_fields &&
        !ContainsJsonUintField(result.mode_result_json, "boss_max_hp", options.required_boss_max_hp)) {
        Fail(verification, "boss_max_hp_mismatch");
        return verification;
    }
    if (options.require_boss_result_fields &&
        !ContainsJsonUintField(result.mode_result_json, "boss_current_hp", options.required_boss_current_hp)) {
        Fail(verification, "boss_current_hp_mismatch");
        return verification;
    }
    if (options.require_boss_result_fields &&
        !ContainsJsonUintField(result.mode_result_json, "boss_damage_total", options.required_boss_damage_total)) {
        Fail(verification, "boss_damage_total_mismatch");
        return verification;
    }
    if (options.require_boss_result_fields) {
        for (const auto& item : options.required_boss_damage_by_player) {
            if (!ContainsJsonUintField(result.mode_result_json, "boss_damage_" + item.first, item.second)) {
                Fail(verification, "boss_player_damage_mismatch");
                return verification;
            }
        }
    }
    if (options.require_boss_result_fields) {
        for (const auto& item : options.required_boss_spawn_slot_by_player) {
            if (!ContainsJsonStringField(
                result.mode_result_json,
                "boss_player_" + item.first + "_spawn_slot",
                item.second
            )) {
                Fail(verification, "boss_player_spawn_slot_mismatch");
                return verification;
            }
        }
        for (const auto& item : options.required_boss_fire_target_by_player) {
            if (!ContainsJsonStringField(
                result.mode_result_json,
                "boss_player_" + item.first + "_fire_target",
                item.second
            )) {
                Fail(verification, "boss_player_fire_target_mismatch");
                return verification;
            }
        }
    }
    if (options.require_boss_result_fields &&
        !ContainsJsonUintField(result.mode_result_json, "boss_defeated", options.required_boss_defeated)) {
        Fail(verification, "boss_defeated_mismatch");
        return verification;
    }
    if (options.require_boss_result_fields &&
        !ContainsJsonUintField(result.mode_result_json, "boss_defeated_tick", options.required_boss_defeated_tick)) {
        Fail(verification, "boss_defeated_tick_mismatch");
        return verification;
    }
    if (options.require_boss_result_fields &&
        !ContainsJsonStringField(result.mode_result_json, "boss_clear_status", options.required_boss_clear_status)) {
        Fail(verification, "boss_clear_status_mismatch");
        return verification;
    }
    if (options.require_boss_result_fields &&
        !ContainsJsonStringField(result.mode_result_json, "boss_result_disposition", options.required_boss_result_disposition)) {
        Fail(verification, "boss_result_disposition_mismatch");
        return verification;
    }
    if (options.require_transfer_result_fields &&
        !ContainsJsonUintField(result.mode_result_json, "transfer_card_count", options.required_transfer_card_count)) {
        Fail(verification, "transfer_card_count_mismatch");
        return verification;
    }
    if (options.require_transfer_result_fields &&
        !ContainsJsonStringField(
            result.mode_result_json,
            "last_transfer_card_instance_id",
            options.required_last_transfer_card_instance_id
        )) {
        Fail(verification, "transfer_card_instance_mismatch");
        return verification;
    }
    if (options.require_transfer_result_fields &&
        !ContainsJsonStringField(
            result.mode_result_json,
            "last_transfer_from_player_id",
            options.required_last_transfer_from_player_id
        )) {
        Fail(verification, "transfer_card_from_player_mismatch");
        return verification;
    }
    if (options.require_transfer_result_fields &&
        !ContainsJsonStringField(
            result.mode_result_json,
            "last_transfer_to_player_id",
            options.required_last_transfer_to_player_id
        )) {
        Fail(verification, "transfer_card_to_player_mismatch");
        return verification;
    }
    if (options.require_transfer_result_fields &&
        !ContainsJsonStringField(
            result.mode_result_json,
            "last_transfer_authority_owner_player_id",
            options.required_last_transfer_authority_owner_player_id
        )) {
        Fail(verification, "transfer_card_authority_owner_mismatch");
        return verification;
    }
    if (options.require_transfer_result_fields &&
        !ContainsJsonUintField(
            result.mode_result_json,
            "last_transfer_authority_mode_allowed",
            options.required_last_transfer_authority_mode_allowed
        )) {
        Fail(verification, "transfer_card_authority_mode_allowed_mismatch");
        return verification;
    }
    if (options.require_transfer_result_fields &&
        !ContainsJsonUintField(
            result.mode_result_json,
            "last_transfer_authority_cost_paid",
            options.required_last_transfer_authority_cost_paid
        )) {
        Fail(verification, "transfer_card_authority_cost_paid_mismatch");
        return verification;
    }
    if (options.require_transfer_result_fields &&
        !ContainsJsonUintField(
            result.mode_result_json,
            "last_transfer_authority_cooldown_ready",
            options.required_last_transfer_authority_cooldown_ready
        )) {
        Fail(verification, "transfer_card_authority_cooldown_mismatch");
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
    if (ContainsForbiddenModeResultMutation(result.mode_result_json)) {
        Fail(verification, "mode_result_mutation_forbidden");
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
    if (options.require_dev_signature_payload_binding &&
        signed_result.signature_hex != DevBattleResultSignatureHex(result, signed_result.key_id)) {
        Fail(verification, "dev_result_signature_mismatch");
        return verification;
    }
    if (!signed_result.server_authoritative) {
        Fail(verification, "result_not_server_authoritative");
        return verification;
    }

    verification.ok = true;
    verification.reason = "ok";
    if (options.allow_dev_signature_shape_only) {
        verification.warnings.push_back("dev_result_signature_payload_bound_not_real_ed25519");
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

std::string DevBattleResultSignatureHex(
    const BattleResult& result,
    std::string_view key_id
) {
    return DevHexMaterial(
        CanonicalBattleResultPayload(result) + ":" + std::string(key_id) + ":result-signature",
        128
    );
}

}  // namespace phk::battle
