#include "phk/battle/simulation.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace phk::battle {

namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::uint64_t HashAppend(std::uint64_t hash, std::string_view value) {
    for (const char ch : value) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= kFnvPrime;
    }
    return hash;
}

std::uint64_t HashAppend(std::uint64_t hash, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        hash ^= static_cast<unsigned char>((value >> shift) & 0xffu);
        hash *= kFnvPrime;
    }
    return hash;
}

std::uint64_t HashAppendSigned(std::uint64_t hash, std::int64_t value) {
    return HashAppend(hash, static_cast<std::uint64_t>(value));
}

std::string HexHash(std::uint64_t hash) {
    std::ostringstream out;
    out << "fnv64:" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

std::string BoolToken(bool value) {
    return value ? "1" : "0";
}

std::string JsonEscape(std::string_view value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (ch < 0x20) {
                    out << "\\u"
                        << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(ch)
                        << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    return out.str();
}

std::string JsonString(std::string_view value) {
    return "\"" + JsonEscape(value) + "\"";
}

std::int32_t ClampMilli(std::int32_t value, std::int32_t min_value, std::int32_t max_value) {
    return std::max(min_value, std::min(value, max_value));
}

std::int32_t DirectionAxis(bool negative, bool positive) {
    if (negative == positive) {
        return 0;
    }
    return positive ? 1 : -1;
}

bool IsAllowedModeActionType(std::string_view action_type) {
    return action_type == "cast_card" ||
        action_type == "select_round_card" ||
        action_type == "transfer_card" ||
        action_type == "ready" ||
        action_type == "reconnect";
}

bool IsReconnectModeAction(std::string_view action_type) {
    return action_type == "reconnect";
}

bool IsBossMode(std::string_view mode_id) {
    return mode_id == "world_boss" || mode_id == "instance_boss";
}

std::string DefaultBossInstanceId(std::string_view mode_id, std::string_view match_id) {
    const std::string prefix = mode_id == "world_boss" ? "world-boss:" : "instance-boss:";
    return prefix + std::string(match_id);
}

bool IsAllowedBossIdentityChar(char ch) {
    const auto byte = static_cast<unsigned char>(ch);
    return std::isalnum(byte) || ch == '_' || ch == '-' || ch == ':' || ch == '.';
}

bool IsAllowedAuditTokenChar(char ch) {
    const auto byte = static_cast<unsigned char>(ch);
    return std::isalnum(byte) || ch == '_' || ch == '-' || ch == '.';
}

bool IsValidBossIdentityField(std::string_view value) {
    return !value.empty() &&
        value.size() <= kDefaultMaxBossIdentityBytes &&
        std::all_of(value.begin(), value.end(), IsAllowedBossIdentityChar);
}

bool IsValidTransferCardInstanceId(std::string_view value) {
    return !value.empty() &&
        value.size() <= kDefaultMaxTransferCardInstanceIdBytes &&
        std::all_of(value.begin(), value.end(), IsAllowedAuditTokenChar);
}

bool IsValidAuditToken(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), IsAllowedAuditTokenChar);
}

bool IsValidOptionalAuditToken(std::string_view value) {
    return value.empty() || IsValidAuditToken(value);
}

std::string NormalizedBossIdentityField(std::string value, std::string fallback) {
    if (IsValidBossIdentityField(value)) {
        return value;
    }
    return fallback;
}

bool IsBattleRoyaleMode(std::string_view mode_id) {
    return mode_id == "battle_royale";
}

bool IsAllowedBossFriendlyFirePolicy(std::string_view policy) {
    return policy == "disabled" ||
        policy == "player_bullets_only" ||
        policy == "all_friendly_fire";
}

std::string LowerAscii(std::string_view value) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered;
}

bool ContainsClientAuthoredAuthorityField(std::string_view payload_json) {
    const std::string lowered = LowerAscii(payload_json);
    for (const std::string_view needle : {
        "x_milli",
        "y_milli",
        "position",
        "damage",
        "hp_delta",
        "current_hp",
        "max_hp",
        "boss_hp",
        "boss_current_hp",
        "boss_max_hp",
        "boss_damage",
        "score",
        "rank",
        "reward",
        "inventory",
        "wallet",
        "currency",
        "grant",
        "item_id",
        "balance",
        "database",
        "steam_inventory",
        "result_hash",
        "battle_result",
        "settlement",
    }) {
        if (lowered.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string BossSpawnSlotName(std::int32_t x_milli, std::int32_t y_milli) {
    if (x_milli == 0 && y_milli < 0) {
        return "north";
    }
    if (x_milli > 0 && y_milli < 0) {
        return "northeast";
    }
    if (x_milli > 0 && y_milli == 0) {
        return "east";
    }
    if (x_milli > 0 && y_milli > 0) {
        return "southeast";
    }
    if (x_milli == 0 && y_milli > 0) {
        return "south";
    }
    if (x_milli < 0 && y_milli > 0) {
        return "southwest";
    }
    if (x_milli < 0 && y_milli == 0) {
        return "west";
    }
    if (x_milli < 0 && y_milli < 0) {
        return "northwest";
    }
    return "center";
}

std::string ExtractJsonStringField(std::string_view payload_json, std::string_view field_name) {
    const std::string prefix = "\"" + std::string(field_name) + "\":\"";
    const auto value_start = payload_json.find(prefix);
    if (value_start == std::string_view::npos) {
        return "";
    }
    const auto string_start = value_start + prefix.size();
    std::string decoded;
    for (std::size_t index = string_start; index < payload_json.size(); ++index) {
        const char ch = payload_json[index];
        if (ch == '"') {
            return decoded;
        }
        if (ch != '\\') {
            decoded.push_back(ch);
            continue;
        }
        ++index;
        if (index >= payload_json.size()) {
            return "";
        }
        const char escaped = payload_json[index];
        switch (escaped) {
            case '"':
            case '\\':
            case '/':
                decoded.push_back(escaped);
                break;
            case 'b':
                decoded.push_back('\b');
                break;
            case 'f':
                decoded.push_back('\f');
                break;
            case 'n':
                decoded.push_back('\n');
                break;
            case 'r':
                decoded.push_back('\r');
                break;
            case 't':
                decoded.push_back('\t');
                break;
            default:
                return "";
        }
    }
    return "";
}

std::optional<bool> ExtractJsonBoolField(std::string_view payload_json, std::string_view field_name) {
    const std::string prefix = "\"" + std::string(field_name) + "\":";
    const auto value_start = payload_json.find(prefix);
    if (value_start == std::string_view::npos) {
        return std::nullopt;
    }
    auto token_start = value_start + prefix.size();
    while (token_start < payload_json.size() &&
        std::isspace(static_cast<unsigned char>(payload_json[token_start]))) {
        ++token_start;
    }
    if (payload_json.substr(token_start, 4) == "true") {
        return true;
    }
    if (payload_json.substr(token_start, 5) == "false") {
        return false;
    }
    return std::nullopt;
}

std::optional<std::int64_t> ExtractJsonIntField(std::string_view payload_json, std::string_view field_name) {
    const std::string prefix = "\"" + std::string(field_name) + "\":";
    const auto value_start = payload_json.find(prefix);
    if (value_start == std::string_view::npos) {
        return std::nullopt;
    }
    auto token_start = value_start + prefix.size();
    while (token_start < payload_json.size() &&
        std::isspace(static_cast<unsigned char>(payload_json[token_start]))) {
        ++token_start;
    }
    bool negative = false;
    if (token_start < payload_json.size() && payload_json[token_start] == '-') {
        negative = true;
        ++token_start;
    }
    if (token_start >= payload_json.size() ||
        !std::isdigit(static_cast<unsigned char>(payload_json[token_start]))) {
        return std::nullopt;
    }

    std::int64_t value = 0;
    while (token_start < payload_json.size() &&
        std::isdigit(static_cast<unsigned char>(payload_json[token_start]))) {
        value = value * 10 + static_cast<std::int64_t>(payload_json[token_start] - '0');
        ++token_start;
    }
    while (token_start < payload_json.size() &&
        std::isspace(static_cast<unsigned char>(payload_json[token_start]))) {
        ++token_start;
    }
    if (token_start < payload_json.size() &&
        payload_json[token_start] != ',' &&
        payload_json[token_start] != '}') {
        return std::nullopt;
    }
    return negative ? -value : value;
}

std::string CanonicalSnapshotPayload(const BattleSnapshot& snapshot) {
    std::ostringstream out;
    out << snapshot.match_id << '|'
        << snapshot.snapshot_tick << '|'
        << snapshot.snapshot_kind << '|'
        << snapshot.state_hash << '|'
        << snapshot.event_cursor << '|';

    std::vector<BattlePlayerSnapshot> players = snapshot.players;
    std::sort(players.begin(), players.end(), [](const BattlePlayerSnapshot& left, const BattlePlayerSnapshot& right) {
        return left.player_id < right.player_id;
    });
    for (const auto& player : players) {
        out << "player="
            << player.player_id << ','
            << player.x_milli << ','
            << player.y_milli << ','
            << BoolToken(player.connected) << ','
            << player.hand_size << ';';
    }
    out << '|';

    std::vector<BattleBulletDelta> bullets = snapshot.bullets_delta;
    std::sort(bullets.begin(), bullets.end(), [](const BattleBulletDelta& left, const BattleBulletDelta& right) {
        return left.bullet_id < right.bullet_id;
    });
    for (const auto& bullet : bullets) {
        out << "bullet="
            << bullet.bullet_id << ','
            << bullet.op << ','
            << bullet.x_milli << ','
            << bullet.y_milli << ','
            << bullet.vx_milli << ','
            << bullet.vy_milli << ','
            << bullet.radius_milli << ','
            << bullet.pattern_id << ','
            << bullet.color << ';';
    }
    out << '|';

    for (const auto& item : snapshot.mode_state) {
        out << "mode=" << item.first << '=' << item.second << ';';
    }
    return out.str();
}

}  // namespace

BattleSimulation::BattleSimulation(SimulationConfig config)
    : config_(std::move(config)) {
    if (config_.ruleset_version.empty()) {
        config_.ruleset_version = std::string(kDefaultRulesetVersion);
    }
    if (config_.tick_rate_hz == 0) {
        config_.tick_rate_hz = kBattleTickRateHz;
    }
    if (config_.max_input_ahead_ticks == 0) {
        config_.max_input_ahead_ticks = 1;
    }
    if (config_.max_seq_ahead == 0) {
        config_.max_seq_ahead = 1;
    }
    if (config_.spawn_period_ticks == 0) {
        config_.spawn_period_ticks = 1;
    }
    if (config_.max_mode_action_id_bytes == 0) {
        config_.max_mode_action_id_bytes = 1;
    }
    if (config_.max_mode_action_type_bytes == 0) {
        config_.max_mode_action_type_bytes = 1;
    }
    if (config_.max_mode_action_payload_bytes == 0) {
        config_.max_mode_action_payload_bytes = 1;
    }
    if (IsBossMode(config_.mode_id)) {
        if (!IsAllowedBossFriendlyFirePolicy(config_.boss_friendly_fire_policy)) {
            config_.boss_friendly_fire_policy = "disabled";
        }
        config_.boss_instance_id = NormalizedBossIdentityField(
            std::move(config_.boss_instance_id),
            DefaultBossInstanceId(config_.mode_id, config_.match_id)
        );
        config_.boss_season_id = NormalizedBossIdentityField(
            std::move(config_.boss_season_id),
            "season-local-s0"
        );
        config_.boss_phase_id = NormalizedBossIdentityField(
            std::move(config_.boss_phase_id),
            "phase-1"
        );
        if (config_.boss_max_hp == 0) {
            config_.boss_max_hp = 1;
        }
        boss_max_hp_ = config_.boss_max_hp;
        boss_current_hp_ = config_.boss_max_hp;
    }
}

const SimulationConfig& BattleSimulation::Config() const {
    return config_;
}

std::uint64_t BattleSimulation::CurrentTick() const {
    return current_tick_;
}

std::size_t BattleSimulation::PlayerCount() const {
    return players_.size();
}

std::size_t BattleSimulation::BulletCount() const {
    return bullets_.size();
}

std::uint64_t BattleSimulation::AcceptedInputCount() const {
    return accepted_input_count_;
}

bool BattleSimulation::IsPlayerConnected(const std::string& player_id) const {
    const auto player_it = players_.find(player_id);
    return player_it != players_.end() && player_it->second.connected;
}

bool BattleSimulation::BossRosterReadyToLock() const {
    return IsBossMode(config_.mode_id) &&
        (boss_combat_started_ || BossReadyToStartForTick(current_tick_ + 1));
}

bool BattleSimulation::AddPlayer(const std::string& player_id, std::int32_t x_milli, std::int32_t y_milli) {
    if (!IsValidAuditToken(player_id) || players_.find(player_id) != players_.end()) {
        return false;
    }
    if (IsBossMode(config_.mode_id) && players_.size() >= kBossModeMaxPlayers) {
        return false;
    }

    if (IsBossMode(config_.mode_id)) {
        const auto spawn_point = BossSpawnPointForIndex(players_.size());
        x_milli = spawn_point.first;
        y_milli = spawn_point.second;
    }

    PlayerState player;
    player.player_id = player_id;
    player.x_milli = ClampMilli(x_milli, -kArenaHalfWidthMilli, kArenaHalfWidthMilli);
    player.y_milli = ClampMilli(y_milli, -kArenaHalfHeightMilli, kArenaHalfHeightMilli);
    player.last_input.match_id = config_.match_id;
    player.last_input.player_id = player_id;
    players_[player_id] = player;
    if (IsBossMode(config_.mode_id)) {
        boss_damage_by_player_[player_id] = 0;
    }
    return true;
}

bool BattleSimulation::ConfigureTransferableCard(TransferableCardState card) {
    if (!IsBossMode(config_.mode_id)) {
        return false;
    }
    if (!IsValidTransferCardInstanceId(card.card_instance_id) ||
        !IsValidAuditToken(card.owner_player_id)) {
        return false;
    }
    if (players_.find(card.owner_player_id) == players_.end()) {
        return false;
    }
    transferable_cards_[card.card_instance_id] = std::move(card);
    return true;
}

InputValidationResult BattleSimulation::SetPlayerConnected(const std::string& player_id, bool connected) {
    InputValidationResult result;
    const auto player_it = players_.find(player_id);
    if (player_it == players_.end()) {
        result.code = InputValidationCode::PlayerUnknown;
        result.reason = "player_unknown";
        return result;
    }

    PlayerState& player = player_it->second;
    if (player.connected != connected) {
        player.connected = connected;
        if (connected) {
            pending_reconnect_player_ids_.erase(player.player_id);
        }
        if (!connected) {
            ready_player_ids_.erase(player.player_id);
            pending_ready_player_ids_.erase(player.player_id);
        }
        AccumulateConnectionEvent(player);
    }

    result.ok = true;
    result.code = InputValidationCode::Ok;
    result.reason = "ok";
    return result;
}

InputValidationResult BattleSimulation::ValidateInput(const BattleInput& input) const {
    InputValidationResult result;
    if (!input.version.IsCompatible()) {
        result.code = InputValidationCode::VersionIncompatible;
        result.reason = "version_incompatible";
        return result;
    }
    if (input.match_id != config_.match_id) {
        result.code = InputValidationCode::MatchMismatch;
        result.reason = "match_mismatch";
        return result;
    }
    if (BossDefeated()) {
        result.code = InputValidationCode::MatchSettled;
        result.reason = "boss_defeated";
        return result;
    }
    const auto player_it = players_.find(input.player_id);
    if (player_it == players_.end()) {
        result.code = InputValidationCode::PlayerUnknown;
        result.reason = "player_unknown";
        return result;
    }
    if (IsBossMode(config_.mode_id) && players_.size() < kBossModeMinPlayers) {
        result.code = InputValidationCode::BossMinPlayersNotMet;
        result.reason = "boss_min_players_not_met";
        return result;
    }
    if (!player_it->second.connected) {
        result.code = InputValidationCode::PlayerDisconnected;
        result.reason = "player_disconnected";
        return result;
    }
    if (input.seq == 0) {
        result.code = InputValidationCode::SeqMissing;
        result.reason = "seq_missing";
        return result;
    }
    if (input.seq <= player_it->second.last_seq) {
        result.code = InputValidationCode::SeqReplay;
        result.reason = "seq_replay";
        return result;
    }
    if (input.seq > player_it->second.last_seq + config_.max_seq_ahead) {
        result.code = InputValidationCode::SeqTooFarAhead;
        result.reason = "seq_too_far_ahead";
        return result;
    }
    if (input.tick <= current_tick_) {
        result.code = InputValidationCode::TickTooOld;
        result.reason = "input_tick_too_old";
        return result;
    }
    if (input.tick > current_tick_ + config_.max_input_ahead_ticks) {
        result.code = InputValidationCode::TickTooFarAhead;
        result.reason = "input_tick_too_far_ahead";
        return result;
    }
    const auto pending_tick_it = pending_inputs_by_tick_.find(input.tick);
    if (pending_tick_it != pending_inputs_by_tick_.end() &&
        pending_tick_it->second.find(input.player_id) != pending_tick_it->second.end()) {
        result.code = InputValidationCode::DuplicateInputForTick;
        result.reason = "input_tick_duplicate";
        return result;
    }
    if ((input.direction_bits & ~0x0fu) != 0) {
        result.code = InputValidationCode::InvalidDirectionBits;
        result.reason = "invalid_direction_bits";
        return result;
    }
    if (input.card_slot < -1 || input.card_slot > 7) {
        result.code = InputValidationCode::InvalidCardSlot;
        result.reason = "invalid_card_slot";
        return result;
    }
    if (input.mode_action_id.size() > config_.max_mode_action_id_bytes) {
        result.code = InputValidationCode::InvalidModeAction;
        result.reason = "input_mode_action_id_too_large";
        return result;
    }
    if (!IsValidOptionalAuditToken(input.mode_action_id)) {
        result.code = InputValidationCode::InvalidModeAction;
        result.reason = "input_mode_action_id_invalid";
        return result;
    }

    result.ok = true;
    result.code = InputValidationCode::Ok;
    result.reason = "ok";
    return result;
}

InputValidationResult BattleSimulation::AcceptInput(const BattleInput& input) {
    auto result = ValidateInput(input);
    if (!result.ok) {
        return result;
    }

    pending_inputs_by_tick_[input.tick][input.player_id] = input;
    std::uint64_t pending_input_records = 0;
    for (const auto& item : pending_inputs_by_tick_) {
        pending_input_records += item.second.size();
    }
    input_buffer_peak_tick_count_ = std::max<std::uint64_t>(
        input_buffer_peak_tick_count_,
        pending_inputs_by_tick_.size()
    );
    input_buffer_peak_record_count_ = std::max(input_buffer_peak_record_count_, pending_input_records);
    players_[input.player_id].last_seq = input.seq;
    AccumulateAcceptedInput(input);
    return result;
}

InputValidationResult BattleSimulation::ValidateModeAction(const BattleModeAction& action) const {
    InputValidationResult result;
    if (!action.version.IsCompatible()) {
        result.code = InputValidationCode::VersionIncompatible;
        result.reason = "version_incompatible";
        return result;
    }
    if (action.match_id != config_.match_id) {
        result.code = InputValidationCode::MatchMismatch;
        result.reason = "match_mismatch";
        return result;
    }
    if (BossDefeated()) {
        result.code = InputValidationCode::MatchSettled;
        result.reason = "boss_defeated";
        return result;
    }
    const auto player_it = players_.find(action.player_id);
    if (player_it == players_.end()) {
        result.code = InputValidationCode::PlayerUnknown;
        result.reason = "player_unknown";
        return result;
    }
    if (IsReconnectModeAction(action.action_type) && player_it->second.connected) {
        result.code = InputValidationCode::InvalidModeAction;
        result.reason = "reconnect_player_connected";
        return result;
    }
    if (!player_it->second.connected && !IsReconnectModeAction(action.action_type)) {
        result.code = InputValidationCode::PlayerDisconnected;
        result.reason = "player_disconnected";
        return result;
    }
    if (action.seq == 0) {
        result.code = InputValidationCode::SeqMissing;
        result.reason = "seq_missing";
        return result;
    }
    if (action.seq <= player_it->second.last_seq) {
        result.code = InputValidationCode::SeqReplay;
        result.reason = "seq_replay";
        return result;
    }
    if (action.seq > player_it->second.last_seq + config_.max_seq_ahead) {
        result.code = InputValidationCode::SeqTooFarAhead;
        result.reason = "seq_too_far_ahead";
        return result;
    }
    if (action.tick <= current_tick_) {
        result.code = InputValidationCode::TickTooOld;
        result.reason = "mode_action_tick_too_old";
        return result;
    }
    if (action.tick > current_tick_ + config_.max_input_ahead_ticks) {
        result.code = InputValidationCode::TickTooFarAhead;
        result.reason = "mode_action_tick_too_far_ahead";
        return result;
    }
    if (action.action_id.empty() || action.action_type.empty() || action.payload_json.empty()) {
        result.code = InputValidationCode::InvalidModeAction;
        result.reason = "mode_action_missing_fields";
        return result;
    }
    if (action.action_id.size() > config_.max_mode_action_id_bytes) {
        result.code = InputValidationCode::InvalidModeAction;
        result.reason = "mode_action_id_too_large";
        return result;
    }
    if (!IsValidAuditToken(action.action_id)) {
        result.code = InputValidationCode::InvalidModeAction;
        result.reason = "mode_action_id_invalid";
        return result;
    }
    if (action.action_type.size() > config_.max_mode_action_type_bytes) {
        result.code = InputValidationCode::InvalidModeAction;
        result.reason = "mode_action_type_too_large";
        return result;
    }
    if (action.payload_json.size() > config_.max_mode_action_payload_bytes) {
        result.code = InputValidationCode::InvalidModeAction;
        result.reason = "mode_action_payload_too_large";
        return result;
    }
    if (!IsAllowedModeActionType(action.action_type)) {
        result.code = InputValidationCode::InvalidModeAction;
        result.reason = "mode_action_type_unsupported";
        return result;
    }
    if (action.client_result_authoritative) {
        result.code = InputValidationCode::InvalidModeAction;
        result.reason = "mode_action_client_result_forbidden";
        return result;
    }
    if (ContainsClientAuthoredAuthorityField(action.payload_json)) {
        result.code = InputValidationCode::InvalidModeAction;
        result.reason = "mode_action_authority_field_forbidden";
        return result;
    }
    if (IsReconnectModeAction(action.action_type) &&
        pending_reconnect_player_ids_.find(action.player_id) != pending_reconnect_player_ids_.end()) {
        result.code = InputValidationCode::InvalidModeAction;
        result.reason = "reconnect_already_pending";
        return result;
    }
    if (IsReconnectModeAction(action.action_type)) {
        const auto last_seen_event_cursor = ExtractJsonIntField(action.payload_json, "last_seen_event_cursor");
        if (!last_seen_event_cursor.has_value()) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "reconnect_cursor_missing";
            return result;
        }
        if (last_seen_event_cursor.value() < 0) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "reconnect_cursor_invalid";
            return result;
        }
        if (static_cast<std::uint64_t>(last_seen_event_cursor.value()) > event_count_) {
            result.code = InputValidationCode::EventCursorAhead;
            result.reason = "reconnect_cursor_ahead";
            return result;
        }
    }
    if (action.action_type == "cast_card") {
        const auto card_slot = ExtractJsonIntField(action.payload_json, "card_slot");
        if (!card_slot.has_value()) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "cast_card_slot_missing";
            return result;
        }
        if (card_slot.value() < 0 || card_slot.value() > 7) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "cast_card_slot_invalid";
            return result;
        }
    }
    if (action.action_type == "select_round_card") {
        if (!IsBattleRoyaleMode(config_.mode_id)) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "select_round_card_mode_unsupported";
            return result;
        }
        const auto candidate_index = ExtractJsonIntField(action.payload_json, "candidate_index");
        if (!candidate_index.has_value()) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "select_round_card_candidate_missing";
            return result;
        }
        if (candidate_index.value() < 0 || candidate_index.value() > 2) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "select_round_card_candidate_invalid";
            return result;
        }
    }
    if (action.action_type == "ready") {
        if (!IsBossMode(config_.mode_id)) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "ready_mode_unsupported";
            return result;
        }
        const auto ready = ExtractJsonBoolField(action.payload_json, "ready");
        if (!ready.has_value()) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "ready_payload_missing";
            return result;
        }
        if (!ready.value()) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "ready_payload_not_true";
            return result;
        }
        if (ready_player_ids_.find(action.player_id) != ready_player_ids_.end() ||
            pending_ready_player_ids_.find(action.player_id) != pending_ready_player_ids_.end()) {
            result.code = InputValidationCode::ReadyAlreadySet;
            result.reason = "ready_already_set";
            return result;
        }
    }
    if (action.action_type == "transfer_card") {
        if (!IsBossMode(config_.mode_id)) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "transfer_card_mode_unsupported";
            return result;
        }
        const std::string target_player_id = ExtractJsonStringField(action.payload_json, "target_player_id");
        const std::string card_instance_id = ExtractJsonStringField(action.payload_json, "card_instance_id");
        if (target_player_id.empty() || card_instance_id.empty()) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "transfer_card_payload_missing_fields";
            return result;
        }
        if (!IsValidTransferCardInstanceId(card_instance_id)) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "transfer_card_instance_id_invalid";
            return result;
        }
        if (!IsValidAuditToken(target_player_id)) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "transfer_card_target_invalid";
            return result;
        }
        if (target_player_id == action.player_id) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "transfer_card_self_forbidden";
            return result;
        }
        const auto target_it = players_.find(target_player_id);
        if (target_it == players_.end()) {
            result.code = InputValidationCode::PlayerUnknown;
            result.reason = "transfer_card_target_unknown";
            return result;
        }
        if (!target_it->second.connected) {
            result.code = InputValidationCode::PlayerDisconnected;
            result.reason = "transfer_card_target_disconnected";
            return result;
        }
        if (reserved_transfer_card_instance_ids_.find(card_instance_id) != reserved_transfer_card_instance_ids_.end()) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "transfer_card_duplicate";
            return result;
        }
        const auto card_it = transferable_cards_.find(card_instance_id);
        if (card_it == transferable_cards_.end()) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "transfer_card_not_authorized";
            return result;
        }
        if (card_it->second.owner_player_id != action.player_id) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "transfer_card_owner_mismatch";
            return result;
        }
        if (!card_it->second.mode_allowed) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "transfer_card_mode_forbidden";
            return result;
        }
        if (!card_it->second.cost_paid) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "transfer_card_cost_unpaid";
            return result;
        }
        if (!card_it->second.cooldown_ready) {
            result.code = InputValidationCode::InvalidModeAction;
            result.reason = "transfer_card_cooldown_blocked";
            return result;
        }
    }
    if (accepted_mode_action_ids_.find(action.action_id) != accepted_mode_action_ids_.end()) {
        result.code = InputValidationCode::DuplicateModeAction;
        result.reason = "mode_action_duplicate";
        return result;
    }

    result.ok = true;
    result.code = InputValidationCode::Ok;
    result.reason = "ok";
    return result;
}

InputValidationResult BattleSimulation::AcceptModeAction(const BattleModeAction& action) {
    auto result = ValidateModeAction(action);
    if (!result.ok) {
        return result;
    }

    players_[action.player_id].last_seq = action.seq;
    accepted_mode_action_ids_.insert(action.action_id);
    if (action.action_type == "ready") {
        pending_ready_player_ids_.insert(action.player_id);
    }
    if (IsReconnectModeAction(action.action_type)) {
        pending_reconnect_player_ids_.insert(action.player_id);
    }
    if (action.action_type == "transfer_card") {
        const std::string card_instance_id = ExtractJsonStringField(action.payload_json, "card_instance_id");
        reserved_transfer_card_instance_ids_.insert(card_instance_id);
        pending_transfer_card_authority_by_action_id_[action.action_id] = transferable_cards_.at(card_instance_id);
    }
    pending_mode_actions_by_tick_[action.tick].push_back(action);
    std::uint64_t pending_mode_action_records = 0;
    for (const auto& item : pending_mode_actions_by_tick_) {
        pending_mode_action_records += item.second.size();
    }
    mode_action_buffer_peak_tick_count_ = std::max<std::uint64_t>(
        mode_action_buffer_peak_tick_count_,
        pending_mode_actions_by_tick_.size()
    );
    mode_action_buffer_peak_record_count_ = std::max(
        mode_action_buffer_peak_record_count_,
        pending_mode_action_records
    );
    return result;
}

BattleSnapshot BattleSimulation::Tick() {
    if (BossDefeated()) {
        pending_inputs_by_tick_.clear();
        pending_mode_actions_by_tick_.clear();
        return Snapshot("full");
    }

    const std::uint64_t tick_to_apply = current_tick_ + 1;
    const auto pending_it = pending_inputs_by_tick_.find(tick_to_apply);
    const bool battle_input_enabled = BattleInputEnabledForTick(tick_to_apply);

    for (auto& item : players_) {
        PlayerState& player = item.second;
        if (!player.connected) {
            continue;
        }
        BattleInput input = InputForTick(player);
        if (pending_it != pending_inputs_by_tick_.end()) {
            const auto input_it = pending_it->second.find(player.player_id);
            if (input_it != pending_it->second.end()) {
                input = input_it->second;
                if (battle_input_enabled) {
                    player.last_input = input;
                }
            } else {
                if (battle_input_enabled) {
                    AccumulateFallbackInput(player, input);
                }
            }
        } else {
            if (battle_input_enabled) {
                AccumulateFallbackInput(player, input);
            }
        }
        if (!battle_input_enabled) {
            continue;
        }
        ApplyInput(player, input, tick_to_apply);
    }

    if (pending_it != pending_inputs_by_tick_.end()) {
        pending_inputs_by_tick_.erase(pending_it);
    }

    current_tick_ = tick_to_apply;
    ApplyModeActionsForTick(tick_to_apply);
    if (IsBossMode(config_.mode_id) && !boss_combat_started_) {
        boss_combat_started_ = BossReadyToStartForTick(tick_to_apply);
    }
    if (!BossDefeated()) {
        SpawnBulletsForTick();
        AdvanceBullets();
    }
    return Snapshot("full");
}

BattleSnapshot BattleSimulation::Snapshot(std::string snapshot_kind) const {
    BattleSnapshot snapshot;
    snapshot.match_id = config_.match_id;
    snapshot.snapshot_tick = current_tick_;
    snapshot.snapshot_kind = std::move(snapshot_kind);
    snapshot.state_hash = CanonicalStateHash();
    snapshot.event_cursor = event_count_;
    snapshot.mode_state["mode_id"] = config_.mode_id;
    snapshot.mode_state["ruleset_version"] = config_.ruleset_version;
    snapshot.mode_state["tick_rate_hz"] = std::to_string(config_.tick_rate_hz);
    snapshot.mode_state["match_seed"] = std::to_string(config_.match_seed);
    snapshot.mode_state["bullet_count"] = std::to_string(bullets_.size());
    snapshot.mode_state["accepted_input_count"] = std::to_string(accepted_input_count_);
    snapshot.mode_state["fallback_input_count"] = std::to_string(fallback_input_count_);
    snapshot.mode_state["neutral_fallback_count"] = std::to_string(neutral_fallback_count_);
    snapshot.mode_state["held_input_fallback_count"] = std::to_string(held_input_fallback_count_);
    snapshot.mode_state["mode_action_count"] = std::to_string(mode_action_count_);
    std::uint64_t pending_input_records = 0;
    for (const auto& item : pending_inputs_by_tick_) {
        pending_input_records += item.second.size();
    }
    std::uint64_t pending_mode_action_records = 0;
    for (const auto& item : pending_mode_actions_by_tick_) {
        pending_mode_action_records += item.second.size();
    }
    snapshot.mode_state["pending_input_tick_count"] = std::to_string(pending_inputs_by_tick_.size());
    snapshot.mode_state["pending_input_record_count"] = std::to_string(pending_input_records);
    snapshot.mode_state["input_buffer_peak_tick_count"] = std::to_string(input_buffer_peak_tick_count_);
    snapshot.mode_state["input_buffer_peak_record_count"] = std::to_string(input_buffer_peak_record_count_);
    snapshot.mode_state["pending_mode_action_tick_count"] = std::to_string(pending_mode_actions_by_tick_.size());
    snapshot.mode_state["pending_mode_action_record_count"] = std::to_string(pending_mode_action_records);
    snapshot.mode_state["mode_action_buffer_peak_tick_count"] =
        std::to_string(mode_action_buffer_peak_tick_count_);
    snapshot.mode_state["mode_action_buffer_peak_record_count"] =
        std::to_string(mode_action_buffer_peak_record_count_);
    std::size_t connected_player_count = 0;
    for (const auto& item : players_) {
        if (item.second.connected) {
            ++connected_player_count;
        }
    }
    snapshot.mode_state["connected_player_count"] = std::to_string(connected_player_count);
    snapshot.mode_state["disconnected_player_count"] = std::to_string(players_.size() - connected_player_count);
    std::size_t ready_connected_player_count = 0;
    for (const auto& player_id : ready_player_ids_) {
        const auto player_it = players_.find(player_id);
        if (player_it != players_.end() && player_it->second.connected) {
            ++ready_connected_player_count;
        }
    }
    snapshot.mode_state["ready_player_count"] = std::to_string(ready_connected_player_count);
    snapshot.mode_state["all_players_ready"] =
        !players_.empty() &&
            connected_player_count == players_.size() &&
            ready_connected_player_count == players_.size() ? "1" : "0";
    if (IsBossMode(config_.mode_id)) {
        const bool instance_boss = config_.mode_id == "instance_boss";
        const bool replay_final_snapshot = snapshot.snapshot_kind == "replay_final";
        const bool boss_defeated = boss_current_hp_ == 0;
        const bool instance_clear_credit = instance_boss && boss_defeated && connected_player_count > 0;
        const bool boss_roster_locked = boss_combat_started_;
        const bool boss_start_ready =
            boss_roster_locked ||
            (connected_player_count >= kBossModeMinPlayers &&
                connected_player_count <= kBossModeMaxPlayers);
        const bool boss_all_registered_connected =
            !players_.empty() && connected_player_count == players_.size();
        const bool boss_all_registered_ready =
            boss_all_registered_connected && ready_connected_player_count == players_.size();
        const bool boss_ready_to_start =
            boss_roster_locked ||
            (boss_start_ready && boss_all_registered_ready);
        const std::string boss_lifecycle_state = boss_combat_started_ ? "combat_started" :
            (!boss_start_ready ? "waiting_for_players" :
                (!boss_all_registered_ready ? "waiting_for_ready" : "start_ready"));
        snapshot.mode_state["battle_layout"] = "boss_center_ring";
        snapshot.mode_state["boss_center_x_milli"] = "0";
        snapshot.mode_state["boss_center_y_milli"] = "0";
        snapshot.mode_state["player_fire_target"] = "boss_center";
        snapshot.mode_state["boss_min_players"] = std::to_string(kBossModeMinPlayers);
        snapshot.mode_state["boss_max_players"] = std::to_string(kBossModeMaxPlayers);
        snapshot.mode_state["boss_registered_player_count"] = std::to_string(players_.size());
        snapshot.mode_state["boss_layout_player_count"] = std::to_string(players_.size());
        snapshot.mode_state["boss_ready_player_count"] = std::to_string(ready_connected_player_count);
        snapshot.mode_state["boss_all_registered_connected"] = BoolToken(boss_all_registered_connected);
        snapshot.mode_state["boss_all_registered_ready"] = BoolToken(boss_all_registered_ready);
        snapshot.mode_state["boss_start_ready"] = BoolToken(boss_start_ready);
        snapshot.mode_state["boss_ready_to_start"] = BoolToken(boss_ready_to_start);
        snapshot.mode_state["boss_lifecycle_state"] = boss_lifecycle_state;
        snapshot.mode_state["boss_roster_locked"] = BoolToken(boss_roster_locked);
        snapshot.mode_state["boss_scope"] = config_.mode_id == "world_boss" ? "world_persistent" : "instance_match";
        snapshot.mode_state["boss_completion_policy"] = config_.mode_id == "world_boss" ?
            "damage_report_to_business" :
            "defeat_required";
        snapshot.mode_state["boss_instance_id"] = config_.boss_instance_id;
        snapshot.mode_state["boss_season_id"] = config_.boss_season_id;
        snapshot.mode_state["boss_phase_id"] = config_.boss_phase_id;
        snapshot.mode_state["boss_friendly_fire_policy"] = config_.boss_friendly_fire_policy;
        snapshot.mode_state["boss_max_hp"] = std::to_string(boss_max_hp_);
        snapshot.mode_state["boss_current_hp"] = std::to_string(boss_current_hp_);
        snapshot.mode_state["boss_damage_total"] = std::to_string(boss_damage_total_);
        snapshot.mode_state["boss_defeated"] = BoolToken(boss_defeated);
        snapshot.mode_state["boss_defeated_tick"] = std::to_string(boss_defeated_tick_);
        snapshot.mode_state["boss_combat_started"] = BoolToken(boss_combat_started_);
        snapshot.mode_state["boss_clear_status"] =
            boss_defeated && (!instance_boss || instance_clear_credit) ? "cleared" :
            (instance_boss && replay_final_snapshot ? "failed" : "running");
        if (config_.mode_id == "world_boss") {
            snapshot.mode_state["boss_result_disposition"] = "world_damage_report";
            snapshot.mode_state["boss_world_persistent_damage_delta"] = std::to_string(boss_damage_total_);
            snapshot.mode_state["boss_world_persistent_hp_after_delta"] = std::to_string(boss_current_hp_);
            snapshot.mode_state["boss_world_defeat_announcement_required"] = BoolToken(boss_defeated);
            snapshot.mode_state["boss_world_defeat_announcement_key"] = boss_defeated ?
                (config_.boss_instance_id + ":" + config_.boss_season_id + ":" +
                    config_.boss_phase_id + ":" + std::to_string(boss_defeated_tick_)) :
                "";
        } else {
            snapshot.mode_state["boss_result_disposition"] = instance_clear_credit ?
                "instance_cleared" :
                (replay_final_snapshot ? "instance_failed" : "instance_incomplete");
            snapshot.mode_state["boss_instance_surviving_player_count"] =
                std::to_string(connected_player_count);
            snapshot.mode_state["boss_instance_clear_credit"] = BoolToken(instance_clear_credit);
            snapshot.mode_state["boss_instance_result_state"] = instance_clear_credit ?
                "cleared" :
                (replay_final_snapshot ? "failed" : "running");
        }
        for (const auto& item : boss_damage_by_player_) {
            snapshot.mode_state["boss_damage_" + item.first] = std::to_string(item.second);
        }
        for (const auto& item : players_) {
            const PlayerState& player = item.second;
            snapshot.mode_state["boss_player_" + player.player_id + "_spawn_slot"] =
                BossSpawnSlotName(player.x_milli, player.y_milli);
            snapshot.mode_state["boss_player_" + player.player_id + "_fire_target"] = "boss_center";
        }
    }
    if (transfer_card_count_ > 0) {
        snapshot.mode_state["transfer_card_count"] = std::to_string(transfer_card_count_);
        snapshot.mode_state["transfer_card_edges_material"] = TransferCardAuditMaterial();
        snapshot.mode_state["last_transfer_card_instance_id"] = last_transfer_card_instance_id_;
        snapshot.mode_state["last_transfer_from_player_id"] = last_transfer_from_player_id_;
        snapshot.mode_state["last_transfer_to_player_id"] = last_transfer_to_player_id_;
        snapshot.mode_state["last_transfer_authority_owner_player_id"] = last_transfer_card_authority_.owner_player_id;
        snapshot.mode_state["last_transfer_authority_mode_allowed"] = BoolToken(last_transfer_card_authority_.mode_allowed);
        snapshot.mode_state["last_transfer_authority_cost_paid"] = BoolToken(last_transfer_card_authority_.cost_paid);
        snapshot.mode_state["last_transfer_authority_cooldown_ready"] = BoolToken(last_transfer_card_authority_.cooldown_ready);
    }
    if (has_last_mode_action_) {
        snapshot.mode_state["last_mode_action_id"] = last_mode_action_.action_id;
        snapshot.mode_state["last_mode_action_type"] = last_mode_action_.action_type;
        snapshot.mode_state["last_mode_action_player_id"] = last_mode_action_.player_id;
        snapshot.mode_state["last_mode_action_tick"] = std::to_string(last_mode_action_.tick);
        snapshot.mode_state["last_mode_action_seq"] = std::to_string(last_mode_action_.seq);
    }

    for (const auto& item : players_) {
        BattlePlayerSnapshot player;
        player.player_id = item.second.player_id;
        player.x_milli = item.second.x_milli;
        player.y_milli = item.second.y_milli;
        player.connected = item.second.connected;
        player.hand_size = 0;
        snapshot.players.push_back(player);
    }

    for (const auto& item : bullets_) {
        BattleBulletDelta bullet;
        bullet.bullet_id = item.bullet_id;
        bullet.op = "upsert";
        bullet.x_milli = item.x_milli;
        bullet.y_milli = item.y_milli;
        bullet.vx_milli = item.vx_milli;
        bullet.vy_milli = item.vy_milli;
        bullet.radius_milli = item.radius_milli;
        bullet.pattern_id = item.pattern_id;
        bullet.color = item.color;
        snapshot.bullets_delta.push_back(bullet);
    }

    return snapshot;
}

BattleSnapshot BattleSimulation::ReconnectSnapshot(
    const std::string& player_id,
    std::uint64_t last_seen_event_cursor
) const {
    const auto player_it = players_.find(player_id);
    if (player_it == players_.end()) {
        BattleSnapshot snapshot;
        snapshot.match_id = config_.match_id;
        snapshot.snapshot_tick = current_tick_;
        snapshot.snapshot_kind = "player_unknown";
        snapshot.event_cursor = event_count_;
        return snapshot;
    }
    if (last_seen_event_cursor > event_count_) {
        BattleSnapshot snapshot;
        snapshot.match_id = config_.match_id;
        snapshot.snapshot_tick = current_tick_;
        snapshot.snapshot_kind = "event_cursor_ahead";
        snapshot.event_cursor = event_count_;
        snapshot.mode_state["requested_event_cursor"] = std::to_string(last_seen_event_cursor);
        return snapshot;
    }

    BattleSnapshot snapshot = Snapshot("reconnect");
    snapshot.mode_state["reconnect_player_id"] = player_id;
    snapshot.mode_state["requested_event_cursor"] = std::to_string(last_seen_event_cursor);
    snapshot.mode_state["missed_event_count"] = std::to_string(event_count_ - last_seen_event_cursor);
    return snapshot;
}

ReplaySummary BattleSimulation::Summary() const {
    ReplaySummary summary;
    summary.match_id = config_.match_id;
    summary.mode_id = config_.mode_id;
    summary.ruleset_version = config_.ruleset_version;
    summary.input_stream_hash = HexHash(input_stream_hash_);
    summary.event_stream_hash = HexHash(event_stream_hash_);
    summary.final_state_hash = CanonicalStateHash();
    summary.match_seed = config_.match_seed;
    summary.final_tick = current_tick_;
    summary.input_count = accepted_input_count_;
    summary.fallback_input_count = fallback_input_count_;
    summary.neutral_fallback_count = neutral_fallback_count_;
    summary.held_input_fallback_count = held_input_fallback_count_;
    summary.mode_action_count = mode_action_count_;
    summary.event_count = event_count_;
    for (const auto& item : players_) {
        summary.player_ids.push_back(item.first);
        if (IsBossMode(config_.mode_id)) {
            const PlayerState& player = item.second;
            summary.boss_spawn_slot_by_player[item.first] =
                BossSpawnSlotName(player.x_milli, player.y_milli);
            summary.boss_fire_target_by_player[item.first] = "boss_center";
        }
    }
    if (IsBossMode(config_.mode_id)) {
        summary.boss_max_hp = boss_max_hp_;
        summary.boss_current_hp = boss_current_hp_;
        summary.boss_damage_total = boss_damage_total_;
        summary.boss_defeated_tick = boss_defeated_tick_;
        summary.boss_damage_by_player = boss_damage_by_player_;
    }
    summary.input_trace = input_trace_;
    summary.event_trace = event_trace_;
    if (has_last_mode_action_) {
        summary.last_mode_action_id = last_mode_action_.action_id;
        summary.last_mode_action_type = last_mode_action_.action_type;
        summary.last_mode_action_player_id = last_mode_action_.player_id;
        summary.last_mode_action_tick = last_mode_action_.tick;
        summary.last_mode_action_seq = last_mode_action_.seq;
    }
    return summary;
}

ReplayInputStreamSummaryRecord BattleSimulation::BuildReplayInputStreamSummary(
    std::string owner_user_id
) const {
    const ReplaySummary summary = Summary();
    ReplayInputStreamSummaryRecord record;
    record.replay_id = DevReplayIdFromReplaySummary(summary);
    record.owner_user_id = std::move(owner_user_id);
    record.match_id = summary.match_id;
    record.input_count = summary.input_count;
    record.fallback_input_count = summary.fallback_input_count;
    record.neutral_fallback_count = summary.neutral_fallback_count;
    record.held_input_fallback_count = summary.held_input_fallback_count;
    record.event_count = summary.event_count;
    record.input_stream_hash = summary.input_stream_hash;
    record.event_stream_hash = summary.event_stream_hash;
    record.final_state_hash = summary.final_state_hash;
    record.match_seed = summary.match_seed;
    record.final_tick = summary.final_tick;
    record.boss_max_hp = summary.boss_max_hp;
    record.boss_current_hp = summary.boss_current_hp;
    record.boss_damage_total = summary.boss_damage_total;
    record.boss_defeated_tick = summary.boss_defeated_tick;
    return record;
}

ReplayFixture BattleSimulation::BuildReplayFixture(std::string owner_user_id) const {
    ReplayFixture fixture;
    fixture.summary = Summary();
    fixture.replay_summary_record = BuildReplayInputStreamSummary(owner_user_id);
    fixture.replay_id = fixture.replay_summary_record.replay_id;
    fixture.owner_user_id = fixture.replay_summary_record.owner_user_id;
    fixture.match_id = fixture.summary.match_id;
    fixture.mode_id = fixture.summary.mode_id;
    fixture.ruleset_version = fixture.summary.ruleset_version;
    fixture.result_hash = DevResultHashFromReplaySummary(fixture.summary);
    fixture.final_snapshot = Snapshot("replay_final");
    fixture.tick_rate_hz = config_.tick_rate_hz;
    fixture.match_seed = config_.match_seed;
    fixture.event_cursor = fixture.summary.event_count;
    fixture.server_authoritative = true;
    fixture.input_trace = fixture.summary.input_trace;
    fixture.event_trace = fixture.summary.event_trace;
    for (const auto& item : players_) {
        fixture.player_ids.push_back(item.first);
    }
    return fixture;
}

std::pair<std::int32_t, std::int32_t> BattleSimulation::BossSpawnPointForIndex(
    std::size_t player_index
) const {
    constexpr std::int32_t kBossSpawnRadiusMilli = 60000;
    constexpr std::array<std::pair<std::int32_t, std::int32_t>, 8> kBossSpawnPoints = {{
        {0, -kBossSpawnRadiusMilli},
        {kBossSpawnRadiusMilli, 0},
        {0, kBossSpawnRadiusMilli},
        {-kBossSpawnRadiusMilli, 0},
        {42426, -42426},
        {42426, 42426},
        {-42426, 42426},
        {-42426, -42426},
    }};
    return kBossSpawnPoints[player_index % kBossSpawnPoints.size()];
}

std::uint64_t BattleSimulation::MixSeed(std::uint64_t value) const {
    std::uint64_t hash = kFnvOffset;
    hash = HashAppend(hash, config_.match_seed);
    hash = HashAppend(hash, value);
    return hash;
}

bool BattleSimulation::BossReadyToStartForTick(std::uint64_t tick) const {
    if (!IsBossMode(config_.mode_id) ||
        players_.size() < kBossModeMinPlayers ||
        players_.size() > kBossModeMaxPlayers) {
        return false;
    }

    std::set<std::string> ready_player_ids = ready_player_ids_;
    const auto actions_it = pending_mode_actions_by_tick_.find(tick);
    if (actions_it != pending_mode_actions_by_tick_.end()) {
        for (const auto& action : actions_it->second) {
            if (action.action_type == "ready") {
                ready_player_ids.insert(action.player_id);
            }
        }
    }

    for (const auto& item : players_) {
        const PlayerState& player = item.second;
        if (!player.connected || ready_player_ids.find(player.player_id) == ready_player_ids.end()) {
            return false;
        }
    }
    return true;
}

bool BattleSimulation::BossDefeated() const {
    return IsBossMode(config_.mode_id) && boss_current_hp_ == 0;
}

bool BattleSimulation::BattleInputEnabledForTick(std::uint64_t tick) const {
    return !BossDefeated() &&
        (!IsBossMode(config_.mode_id) || boss_combat_started_ || BossReadyToStartForTick(tick));
}

std::string BattleSimulation::CanonicalStateHash() const {
    std::uint64_t hash = kFnvOffset;
    hash = HashAppend(hash, config_.match_id);
    hash = HashAppend(hash, config_.mode_id);
    hash = HashAppend(hash, config_.ruleset_version);
    hash = HashAppend(hash, current_tick_);
    hash = HashAppend(hash, config_.match_seed);
    hash = HashAppend(hash, config_.tick_rate_hz);
    hash = HashAppend(hash, accepted_input_count_);
    hash = HashAppend(hash, fallback_input_count_);
    hash = HashAppend(hash, neutral_fallback_count_);
    hash = HashAppend(hash, held_input_fallback_count_);
    hash = HashAppend(hash, mode_action_count_);
    for (const auto& player_id : ready_player_ids_) {
        hash = HashAppend(hash, player_id);
    }
    if (transfer_card_count_ > 0) {
        hash = HashAppend(hash, transfer_card_count_);
        hash = HashAppend(hash, last_transfer_card_instance_id_);
        hash = HashAppend(hash, last_transfer_from_player_id_);
        hash = HashAppend(hash, last_transfer_to_player_id_);
        hash = HashAppend(hash, last_transfer_card_authority_.owner_player_id);
        hash = HashAppend(hash, last_transfer_card_authority_.mode_allowed ? 1u : 0u);
        hash = HashAppend(hash, last_transfer_card_authority_.cost_paid ? 1u : 0u);
        hash = HashAppend(hash, last_transfer_card_authority_.cooldown_ready ? 1u : 0u);
        for (const auto& item : transferred_card_edges_) {
            hash = HashAppend(hash, item.first);
            hash = HashAppend(hash, item.second.first);
            hash = HashAppend(hash, item.second.second);
            const auto authority_it = transferred_card_authority_by_card_instance_id_.find(item.first);
            if (authority_it != transferred_card_authority_by_card_instance_id_.end()) {
                hash = HashAppend(hash, authority_it->second.owner_player_id);
                hash = HashAppend(hash, authority_it->second.mode_allowed ? 1u : 0u);
                hash = HashAppend(hash, authority_it->second.cost_paid ? 1u : 0u);
                hash = HashAppend(hash, authority_it->second.cooldown_ready ? 1u : 0u);
            }
        }
    }
    if (IsBossMode(config_.mode_id)) {
        hash = HashAppend(hash, config_.boss_instance_id);
        hash = HashAppend(hash, config_.boss_season_id);
        hash = HashAppend(hash, config_.boss_phase_id);
        hash = HashAppend(hash, boss_max_hp_);
        hash = HashAppend(hash, boss_current_hp_);
        hash = HashAppend(hash, boss_damage_total_);
        hash = HashAppend(hash, boss_defeated_tick_);
        hash = HashAppend(hash, boss_combat_started_ ? 1u : 0u);
        hash = HashAppend(hash, config_.boss_friendly_fire_policy);
        for (const auto& item : boss_damage_by_player_) {
            hash = HashAppend(hash, item.first);
            hash = HashAppend(hash, item.second);
        }
    }

    for (const auto& item : players_) {
        const PlayerState& player = item.second;
        hash = HashAppend(hash, player.player_id);
        hash = HashAppendSigned(hash, player.x_milli);
        hash = HashAppendSigned(hash, player.y_milli);
        hash = HashAppend(hash, player.last_seq);
        hash = HashAppend(hash, player.connected ? 1u : 0u);
        hash = HashAppend(hash, player.last_input.direction_bits);
        hash = HashAppend(hash, player.last_input.slow ? 1u : 0u);
        hash = HashAppend(hash, player.last_input.shoot ? 1u : 0u);
        hash = HashAppend(hash, player.last_input.bomb ? 1u : 0u);
        hash = HashAppendSigned(hash, player.last_input.card_slot);
    }

    std::vector<BulletState> bullets = bullets_;
    std::sort(bullets.begin(), bullets.end(), [](const BulletState& left, const BulletState& right) {
        return left.bullet_id < right.bullet_id;
    });
    for (const auto& bullet : bullets) {
        hash = HashAppend(hash, bullet.bullet_id);
        hash = HashAppendSigned(hash, bullet.x_milli);
        hash = HashAppendSigned(hash, bullet.y_milli);
        hash = HashAppendSigned(hash, bullet.vx_milli);
        hash = HashAppendSigned(hash, bullet.vy_milli);
        hash = HashAppend(hash, bullet.radius_milli);
        hash = HashAppend(hash, bullet.pattern_id);
        hash = HashAppend(hash, bullet.color);
    }
    return HexHash(hash);
}

std::string CanonicalReplayInputStreamSummaryRecord(
    const ReplayInputStreamSummaryRecord& record
) {
    std::ostringstream out;
    out << record.version.protocol_version << '|'
        << record.version.business_api_version << '|'
        << record.version.battle_api_version << '|'
        << record.version.ruleset_version << '|'
        << record.replay_id << '|'
        << record.owner_user_id << '|'
        << record.match_id << '|'
        << record.input_count << '|'
        << record.fallback_input_count << '|'
        << record.neutral_fallback_count << '|'
        << record.held_input_fallback_count << '|'
        << record.event_count << '|'
        << record.input_stream_hash << '|'
        << record.event_stream_hash << '|'
        << record.final_state_hash << '|'
        << record.match_seed << '|'
        << record.final_tick << '|'
        << record.boss_max_hp << '|'
        << record.boss_current_hp << '|'
        << record.boss_damage_total << '|'
        << record.boss_defeated_tick;
    return out.str();
}

std::string CanonicalReplaySummaryPayload(const ReplaySummary& summary) {
    std::ostringstream out;
    out << summary.match_id << '|'
        << summary.mode_id << '|'
        << summary.ruleset_version << '|'
        << summary.input_stream_hash << '|'
        << summary.event_stream_hash << '|'
        << summary.final_state_hash << '|'
        << summary.match_seed << '|'
        << summary.final_tick << '|'
        << summary.input_count << '|'
        << summary.fallback_input_count << '|'
        << summary.neutral_fallback_count << '|'
        << summary.held_input_fallback_count << '|'
        << summary.mode_action_count << '|'
        << summary.event_count << '|'
        << summary.last_mode_action_id << '|'
        << summary.last_mode_action_type << '|'
        << summary.last_mode_action_player_id << '|'
        << summary.last_mode_action_tick << '|'
        << summary.last_mode_action_seq << '|';
    for (const auto& player_id : summary.player_ids) {
        out << player_id << ',';
    }
    out << '|';
    if (!summary.boss_spawn_slot_by_player.empty() ||
        !summary.boss_damage_by_player.empty() ||
        summary.boss_max_hp != 0 ||
        summary.boss_damage_total != 0) {
        out << summary.boss_max_hp << '|'
            << summary.boss_current_hp << '|'
            << summary.boss_damage_total << '|'
            << summary.boss_defeated_tick << '|';
        for (const auto& item : summary.boss_damage_by_player) {
            out << "boss_damage=" << item.first << ':' << item.second << ';';
        }
        out << '|';
    }
    for (const auto& item : summary.boss_spawn_slot_by_player) {
        out << "boss_spawn=" << item.first << ':' << item.second << ';';
    }
    out << '|';
    for (const auto& item : summary.boss_fire_target_by_player) {
        out << "boss_fire=" << item.first << ':' << item.second << ';';
    }
    out << '|';
    for (const auto& trace : summary.input_trace) {
        out << trace << '\n';
    }
    out << '|';
    for (const auto& trace : summary.event_trace) {
        out << trace << '\n';
    }
    return out.str();
}

std::string DevReplayInputStreamSummaryHash(
    const ReplayInputStreamSummaryRecord& record
) {
    std::uint64_t hash = kFnvOffset;
    hash = HashAppend(hash, CanonicalReplayInputStreamSummaryRecord(record));

    std::ostringstream out;
    out << "sha256:dev-fnv64-" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

std::string CanonicalReplayFixturePayload(const ReplayFixture& fixture) {
    std::ostringstream out;
    out << fixture.replay_id << '|'
        << fixture.owner_user_id << '|'
        << fixture.match_id << '|'
        << fixture.mode_id << '|'
        << fixture.ruleset_version << '|'
        << fixture.result_hash << '|'
        << fixture.tick_rate_hz << '|'
        << fixture.match_seed << '|'
        << fixture.event_cursor << '|'
        << (fixture.server_authoritative ? "1" : "0") << '|'
        << CanonicalReplaySummaryPayload(fixture.summary) << '|'
        << CanonicalReplayInputStreamSummaryRecord(fixture.replay_summary_record) << '|'
        << CanonicalSnapshotPayload(fixture.final_snapshot) << '|';
    for (const auto& player_id : fixture.player_ids) {
        out << player_id << ',';
    }
    out << '|';
    for (const auto& trace : fixture.input_trace) {
        out << trace << '\n';
    }
    out << '|';
    for (const auto& trace : fixture.event_trace) {
        out << trace << '\n';
    }
    return out.str();
}

std::string DevReplayFixtureHash(const ReplayFixture& fixture) {
    std::uint64_t hash = kFnvOffset;
    hash = HashAppend(hash, CanonicalReplayFixturePayload(fixture));

    std::ostringstream out;
    out << "sha256:dev-fnv64-" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

std::string CanonicalReplayLoadoutBridgePayload(
    const std::vector<ReplayLoadoutBridge>& loadout
) {
    std::ostringstream out;
    std::vector<ReplayLoadoutBridge> sorted = loadout;
    std::sort(sorted.begin(), sorted.end(), [](const ReplayLoadoutBridge& left, const ReplayLoadoutBridge& right) {
        if (left.player_id != right.player_id) {
            return left.player_id < right.player_id;
        }
        return left.user_id < right.user_id;
    });

    for (const auto& item : sorted) {
        out << "loadout="
            << item.user_id << ','
            << item.player_id << ','
            << item.character_id << ','
            << item.stage_id << ','
            << item.rating_code << ','
            << item.deck_snapshot_hash << ','
            << item.deck_ruleset_version << ',';
        for (const auto& card_id : item.deck_card_ids) {
            out << card_id << '/';
        }
        out << ';';
    }
    return out.str();
}

std::string CanonicalReplayRecordBridgePayload(const ReplayRecordBridge& record) {
    std::ostringstream out;
    out << record.version.protocol_version << '|'
        << record.version.business_api_version << '|'
        << record.version.battle_api_version << '|'
        << record.version.ruleset_version << '|'
        << record.replay_id << '|'
        << record.match_id << '|'
        << record.owner_user_id << '|'
        << record.mode_id << '|'
        << record.stage_id << '|'
        << CanonicalReplayLoadoutBridgePayload(record.loadout) << '|'
        << CanonicalReplayInputStreamSummaryRecord(record.stream) << '|'
        << CanonicalBattleResultPayload(record.settlement.result) << '|'
        << record.settlement.signature_alg << '|'
        << record.settlement.key_id << '|'
        << record.settlement.public_key_hex << '|'
        << record.settlement.signature_hex << '|'
        << (record.settlement.server_authoritative ? "1" : "0") << '|'
        << (record.server_authoritative ? "1" : "0") << '|'
        << record.created_at_ms;
    return out.str();
}

std::string DevReplayRecordBridgeHash(const ReplayRecordBridge& record) {
    std::uint64_t hash = kFnvOffset;
    hash = HashAppend(hash, CanonicalReplayRecordBridgePayload(record));

    std::ostringstream out;
    out << "sha256:dev-fnv64-" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

std::string DevModeResultJsonFromReplayFixture(const ReplayFixture& fixture) {
    const ReplaySummary& summary = fixture.summary;
    std::string json = "{\"battle_result_owner\":\"cpp\",\"event_cursor\":" +
        std::to_string(summary.event_count) +
        ",\"final_tick\":" +
        std::to_string(summary.final_tick) +
        ",\"tick_rate_hz\":" +
        std::to_string(fixture.tick_rate_hz) +
        ",\"match_seed\":" +
        std::to_string(fixture.match_seed) +
        ",\"input_count\":" +
        std::to_string(summary.input_count) +
        ",\"fallback_input_count\":" +
        std::to_string(summary.fallback_input_count) +
        ",\"neutral_fallback_count\":" +
        std::to_string(summary.neutral_fallback_count) +
        ",\"held_input_fallback_count\":" +
        std::to_string(summary.held_input_fallback_count) +
        ",\"mode_action_count\":" +
        std::to_string(summary.mode_action_count) +
        ",\"pending_input_tick_count\":" +
        fixture.final_snapshot.mode_state.at("pending_input_tick_count") +
        ",\"pending_input_record_count\":" +
        fixture.final_snapshot.mode_state.at("pending_input_record_count") +
        ",\"input_buffer_peak_tick_count\":" +
        fixture.final_snapshot.mode_state.at("input_buffer_peak_tick_count") +
        ",\"input_buffer_peak_record_count\":" +
        fixture.final_snapshot.mode_state.at("input_buffer_peak_record_count") +
        ",\"pending_mode_action_tick_count\":" +
        fixture.final_snapshot.mode_state.at("pending_mode_action_tick_count") +
        ",\"pending_mode_action_record_count\":" +
        fixture.final_snapshot.mode_state.at("pending_mode_action_record_count") +
        ",\"mode_action_buffer_peak_tick_count\":" +
        fixture.final_snapshot.mode_state.at("mode_action_buffer_peak_tick_count") +
        ",\"mode_action_buffer_peak_record_count\":" +
        fixture.final_snapshot.mode_state.at("mode_action_buffer_peak_record_count") +
        ",\"input_trace_count\":" +
        std::to_string(summary.input_trace.size()) +
        ",\"event_trace_count\":" +
        std::to_string(summary.event_trace.size()) +
        ",\"input_stream_hash\":" +
        JsonString(summary.input_stream_hash) +
        ",\"event_stream_hash\":" +
        JsonString(summary.event_stream_hash) +
        ",\"final_state_hash\":" +
        JsonString(summary.final_state_hash) +
        ",\"replay_summary_hash\":" +
        JsonString(DevReplayInputStreamSummaryHash(fixture.replay_summary_record)) +
        ",\"replay_fixture_hash\":" +
        JsonString(DevReplayFixtureHash(fixture)) +
        ",\"final_snapshot_tick\":" +
        std::to_string(fixture.final_snapshot.snapshot_tick) +
        ",\"final_snapshot_kind\":" +
        JsonString(fixture.final_snapshot.snapshot_kind) +
        ",\"final_snapshot_state_hash\":" +
        JsonString(fixture.final_snapshot.state_hash) +
        ",\"final_snapshot_event_cursor\":" +
        std::to_string(fixture.final_snapshot.event_cursor);
    const auto boss_scope = fixture.final_snapshot.mode_state.find("boss_scope");
    if (boss_scope != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_scope\":" + JsonString(boss_scope->second);
    }
    const auto boss_completion_policy = fixture.final_snapshot.mode_state.find("boss_completion_policy");
    if (boss_completion_policy != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_completion_policy\":" + JsonString(boss_completion_policy->second);
    }
    const auto boss_instance_id = fixture.final_snapshot.mode_state.find("boss_instance_id");
    if (boss_instance_id != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_instance_id\":" + JsonString(boss_instance_id->second);
    }
    const auto boss_season_id = fixture.final_snapshot.mode_state.find("boss_season_id");
    if (boss_season_id != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_season_id\":" + JsonString(boss_season_id->second);
    }
    const auto boss_phase_id = fixture.final_snapshot.mode_state.find("boss_phase_id");
    if (boss_phase_id != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_phase_id\":" + JsonString(boss_phase_id->second);
    }
    const auto boss_friendly_fire_policy = fixture.final_snapshot.mode_state.find("boss_friendly_fire_policy");
    if (boss_friendly_fire_policy != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_friendly_fire_policy\":" + JsonString(boss_friendly_fire_policy->second);
    }
    const auto boss_min_players = fixture.final_snapshot.mode_state.find("boss_min_players");
    if (boss_min_players != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_min_players\":" + boss_min_players->second;
    }
    const auto boss_max_players = fixture.final_snapshot.mode_state.find("boss_max_players");
    if (boss_max_players != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_max_players\":" + boss_max_players->second;
    }
    const auto boss_registered_player_count = fixture.final_snapshot.mode_state.find("boss_registered_player_count");
    if (boss_registered_player_count != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_registered_player_count\":" + boss_registered_player_count->second;
    }
    const auto boss_layout_player_count = fixture.final_snapshot.mode_state.find("boss_layout_player_count");
    if (boss_layout_player_count != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_layout_player_count\":" + boss_layout_player_count->second;
    }
    const auto boss_start_ready = fixture.final_snapshot.mode_state.find("boss_start_ready");
    if (boss_start_ready != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_start_ready\":" + boss_start_ready->second;
    }
    const auto boss_ready_player_count = fixture.final_snapshot.mode_state.find("boss_ready_player_count");
    if (boss_ready_player_count != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_ready_player_count\":" + boss_ready_player_count->second;
    }
    const auto boss_all_registered_connected = fixture.final_snapshot.mode_state.find("boss_all_registered_connected");
    if (boss_all_registered_connected != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_all_registered_connected\":" + boss_all_registered_connected->second;
    }
    const auto boss_all_registered_ready = fixture.final_snapshot.mode_state.find("boss_all_registered_ready");
    if (boss_all_registered_ready != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_all_registered_ready\":" + boss_all_registered_ready->second;
    }
    const auto boss_ready_to_start = fixture.final_snapshot.mode_state.find("boss_ready_to_start");
    if (boss_ready_to_start != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_ready_to_start\":" + boss_ready_to_start->second;
    }
    const auto boss_lifecycle_state = fixture.final_snapshot.mode_state.find("boss_lifecycle_state");
    if (boss_lifecycle_state != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_lifecycle_state\":" + JsonString(boss_lifecycle_state->second);
    }
    const auto boss_roster_locked = fixture.final_snapshot.mode_state.find("boss_roster_locked");
    if (boss_roster_locked != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_roster_locked\":" + boss_roster_locked->second;
    }
    const auto connected_player_count = fixture.final_snapshot.mode_state.find("connected_player_count");
    if (boss_scope != fixture.final_snapshot.mode_state.end() &&
        connected_player_count != fixture.final_snapshot.mode_state.end()) {
        json += ",\"connected_player_count\":" + connected_player_count->second;
    }
    const auto disconnected_player_count = fixture.final_snapshot.mode_state.find("disconnected_player_count");
    if (boss_scope != fixture.final_snapshot.mode_state.end() &&
        disconnected_player_count != fixture.final_snapshot.mode_state.end()) {
        json += ",\"disconnected_player_count\":" + disconnected_player_count->second;
    }
    const auto boss_max_hp = fixture.final_snapshot.mode_state.find("boss_max_hp");
    if (boss_max_hp != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_max_hp\":" + boss_max_hp->second;
    }
    const auto boss_current_hp = fixture.final_snapshot.mode_state.find("boss_current_hp");
    if (boss_current_hp != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_current_hp\":" + boss_current_hp->second;
    }
    const auto boss_damage_total = fixture.final_snapshot.mode_state.find("boss_damage_total");
    if (boss_damage_total != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_damage_total\":" + boss_damage_total->second;
    }
    for (const auto& player_id : fixture.player_ids) {
        const auto player_damage = fixture.final_snapshot.mode_state.find("boss_damage_" + player_id);
        if (player_damage != fixture.final_snapshot.mode_state.end()) {
            json += ",\"boss_damage_" + player_id + "\":" + player_damage->second;
        }
        const auto player_spawn_slot = fixture.final_snapshot.mode_state.find(
            "boss_player_" + player_id + "_spawn_slot"
        );
        if (player_spawn_slot != fixture.final_snapshot.mode_state.end()) {
            json += ",\"boss_player_" + player_id + "_spawn_slot\":" + JsonString(player_spawn_slot->second);
        }
        const auto player_fire_target = fixture.final_snapshot.mode_state.find(
            "boss_player_" + player_id + "_fire_target"
        );
        if (player_fire_target != fixture.final_snapshot.mode_state.end()) {
            json += ",\"boss_player_" + player_id + "_fire_target\":" + JsonString(player_fire_target->second);
        }
    }
    const auto boss_defeated = fixture.final_snapshot.mode_state.find("boss_defeated");
    if (boss_defeated != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_defeated\":" + boss_defeated->second;
    }
    const auto boss_defeated_tick = fixture.final_snapshot.mode_state.find("boss_defeated_tick");
    if (boss_defeated_tick != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_defeated_tick\":" + boss_defeated_tick->second;
    }
    const auto boss_clear_status = fixture.final_snapshot.mode_state.find("boss_clear_status");
    if (boss_clear_status != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_clear_status\":" + JsonString(boss_clear_status->second);
    }
    const auto boss_result_disposition = fixture.final_snapshot.mode_state.find("boss_result_disposition");
    if (boss_result_disposition != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_result_disposition\":" + JsonString(boss_result_disposition->second);
    }
    const auto boss_world_persistent_damage_delta =
        fixture.final_snapshot.mode_state.find("boss_world_persistent_damage_delta");
    if (boss_world_persistent_damage_delta != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_world_persistent_damage_delta\":" + boss_world_persistent_damage_delta->second;
    }
    const auto boss_world_persistent_hp_after_delta =
        fixture.final_snapshot.mode_state.find("boss_world_persistent_hp_after_delta");
    if (boss_world_persistent_hp_after_delta != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_world_persistent_hp_after_delta\":" + boss_world_persistent_hp_after_delta->second;
    }
    const auto boss_world_defeat_announcement_required =
        fixture.final_snapshot.mode_state.find("boss_world_defeat_announcement_required");
    if (boss_world_defeat_announcement_required != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_world_defeat_announcement_required\":" + boss_world_defeat_announcement_required->second;
    }
    const auto boss_world_defeat_announcement_key =
        fixture.final_snapshot.mode_state.find("boss_world_defeat_announcement_key");
    if (boss_world_defeat_announcement_key != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_world_defeat_announcement_key\":" +
            JsonString(boss_world_defeat_announcement_key->second);
    }
    const auto boss_instance_surviving_player_count =
        fixture.final_snapshot.mode_state.find("boss_instance_surviving_player_count");
    if (boss_instance_surviving_player_count != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_instance_surviving_player_count\":" + boss_instance_surviving_player_count->second;
    }
    const auto boss_instance_clear_credit = fixture.final_snapshot.mode_state.find("boss_instance_clear_credit");
    if (boss_instance_clear_credit != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_instance_clear_credit\":" + boss_instance_clear_credit->second;
    }
    const auto boss_instance_result_state = fixture.final_snapshot.mode_state.find("boss_instance_result_state");
    if (boss_instance_result_state != fixture.final_snapshot.mode_state.end()) {
        json += ",\"boss_instance_result_state\":" + JsonString(boss_instance_result_state->second);
    }
    const auto transfer_card_count = fixture.final_snapshot.mode_state.find("transfer_card_count");
    if (transfer_card_count != fixture.final_snapshot.mode_state.end()) {
        json += ",\"transfer_card_count\":" + transfer_card_count->second;
    }
    const auto transfer_card_edges_material = fixture.final_snapshot.mode_state.find("transfer_card_edges_material");
    if (transfer_card_edges_material != fixture.final_snapshot.mode_state.end()) {
        json += ",\"transfer_card_edges_material\":" + JsonString(transfer_card_edges_material->second);
    }
    const auto last_transfer_card_instance_id = fixture.final_snapshot.mode_state.find("last_transfer_card_instance_id");
    if (last_transfer_card_instance_id != fixture.final_snapshot.mode_state.end()) {
        json += ",\"last_transfer_card_instance_id\":" + JsonString(last_transfer_card_instance_id->second);
    }
    const auto last_transfer_authority_owner = fixture.final_snapshot.mode_state.find("last_transfer_authority_owner_player_id");
    if (last_transfer_authority_owner != fixture.final_snapshot.mode_state.end()) {
        json += ",\"last_transfer_authority_owner_player_id\":" + JsonString(last_transfer_authority_owner->second);
    }
    const auto last_transfer_authority_mode_allowed = fixture.final_snapshot.mode_state.find("last_transfer_authority_mode_allowed");
    if (last_transfer_authority_mode_allowed != fixture.final_snapshot.mode_state.end()) {
        json += ",\"last_transfer_authority_mode_allowed\":" + last_transfer_authority_mode_allowed->second;
    }
    const auto last_transfer_authority_cost_paid = fixture.final_snapshot.mode_state.find("last_transfer_authority_cost_paid");
    if (last_transfer_authority_cost_paid != fixture.final_snapshot.mode_state.end()) {
        json += ",\"last_transfer_authority_cost_paid\":" + last_transfer_authority_cost_paid->second;
    }
    const auto last_transfer_authority_cooldown_ready = fixture.final_snapshot.mode_state.find("last_transfer_authority_cooldown_ready");
    if (last_transfer_authority_cooldown_ready != fixture.final_snapshot.mode_state.end()) {
        json += ",\"last_transfer_authority_cooldown_ready\":" + last_transfer_authority_cooldown_ready->second;
    }
    const auto last_transfer_from_player_id = fixture.final_snapshot.mode_state.find("last_transfer_from_player_id");
    if (last_transfer_from_player_id != fixture.final_snapshot.mode_state.end()) {
        json += ",\"last_transfer_from_player_id\":" + JsonString(last_transfer_from_player_id->second);
    }
    const auto last_transfer_to_player_id = fixture.final_snapshot.mode_state.find("last_transfer_to_player_id");
    if (last_transfer_to_player_id != fixture.final_snapshot.mode_state.end()) {
        json += ",\"last_transfer_to_player_id\":" + JsonString(last_transfer_to_player_id->second);
    }
    json += "}";
    return json;
}

std::string DevResultHashFromReplaySummary(const ReplaySummary& summary) {
    std::uint64_t hash = kFnvOffset;
    hash = HashAppend(hash, summary.match_id);
    hash = HashAppend(hash, summary.mode_id);
    hash = HashAppend(hash, summary.ruleset_version);
    hash = HashAppend(hash, summary.input_stream_hash);
    hash = HashAppend(hash, summary.event_stream_hash);
    hash = HashAppend(hash, summary.final_state_hash);
    hash = HashAppend(hash, summary.match_seed);
    hash = HashAppend(hash, summary.final_tick);
    hash = HashAppend(hash, summary.input_count);
    hash = HashAppend(hash, summary.fallback_input_count);
    hash = HashAppend(hash, summary.neutral_fallback_count);
    hash = HashAppend(hash, summary.held_input_fallback_count);
    hash = HashAppend(hash, summary.mode_action_count);
    hash = HashAppend(hash, summary.event_count);
    for (const auto& player_id : summary.player_ids) {
        hash = HashAppend(hash, player_id);
    }
    if (!summary.boss_spawn_slot_by_player.empty() ||
        !summary.boss_damage_by_player.empty() ||
        summary.boss_max_hp != 0 ||
        summary.boss_damage_total != 0) {
        hash = HashAppend(hash, summary.boss_max_hp);
        hash = HashAppend(hash, summary.boss_current_hp);
        hash = HashAppend(hash, summary.boss_damage_total);
        hash = HashAppend(hash, summary.boss_defeated_tick);
        for (const auto& item : summary.boss_damage_by_player) {
            hash = HashAppend(hash, item.first);
            hash = HashAppend(hash, item.second);
        }
    }
    for (const auto& item : summary.boss_spawn_slot_by_player) {
        hash = HashAppend(hash, item.first);
        hash = HashAppend(hash, item.second);
    }
    for (const auto& item : summary.boss_fire_target_by_player) {
        hash = HashAppend(hash, item.first);
        hash = HashAppend(hash, item.second);
    }
    for (const auto& item : summary.input_trace) {
        hash = HashAppend(hash, item);
    }
    for (const auto& item : summary.event_trace) {
        hash = HashAppend(hash, item);
    }

    std::ostringstream out;
    out << "sha256:dev-fnv64-" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

std::string DevReplayIdFromReplaySummary(const ReplaySummary& summary) {
    return "battle-replay:" + summary.match_id + ":" + std::to_string(summary.final_tick);
}

BattleInput BattleSimulation::InputForTick(const PlayerState& player) const {
    BattleInput input = player.last_input;
    input.tick = current_tick_ + 1;
    return input;
}

void BattleSimulation::ApplyInput(PlayerState& player, const BattleInput& input, std::uint64_t applied_tick) {
    constexpr std::uint32_t kUp = 1u << 0;
    constexpr std::uint32_t kDown = 1u << 1;
    constexpr std::uint32_t kLeft = 1u << 2;
    constexpr std::uint32_t kRight = 1u << 3;
    const std::int32_t axis_x = DirectionAxis((input.direction_bits & kLeft) != 0, (input.direction_bits & kRight) != 0);
    const std::int32_t axis_y = DirectionAxis((input.direction_bits & kUp) != 0, (input.direction_bits & kDown) != 0);
    const std::int32_t speed = input.slow ? 2500 : 5000;

    player.x_milli = ClampMilli(player.x_milli + axis_x * speed, -kArenaHalfWidthMilli, kArenaHalfWidthMilli);
    player.y_milli = ClampMilli(player.y_milli + axis_y * speed, -kArenaHalfHeightMilli, kArenaHalfHeightMilli);
    ApplyBossDamageForInput(player, input, applied_tick);
}

void BattleSimulation::ApplyBossDamageForInput(
    const PlayerState& player,
    const BattleInput& input,
    std::uint64_t applied_tick
) {
    if (!IsBossMode(config_.mode_id) || !input.shoot || boss_current_hp_ == 0) {
        return;
    }
    if (!boss_combat_started_) {
        boss_combat_started_ = BossReadyToStartForTick(applied_tick);
    }
    if (!boss_combat_started_) {
        return;
    }

    constexpr std::uint64_t kPlayerShotDamagePerTick = 10;
    const std::uint64_t damage = std::min(kPlayerShotDamagePerTick, boss_current_hp_);
    boss_current_hp_ -= damage;
    boss_damage_total_ += damage;
    boss_damage_by_player_[player.player_id] += damage;

    if (boss_current_hp_ == 0 && boss_defeated_tick_ == 0) {
        boss_defeated_tick_ = applied_tick;
        event_stream_hash_ = HashAppend(event_stream_hash_, config_.match_id);
        event_stream_hash_ = HashAppend(event_stream_hash_, config_.mode_id);
        event_stream_hash_ = HashAppend(event_stream_hash_, applied_tick);
        event_stream_hash_ = HashAppend(event_stream_hash_, player.player_id);
        event_stream_hash_ = HashAppend(event_stream_hash_, boss_damage_total_);
        event_trace_.push_back(
            "boss_defeated|tick=" + std::to_string(applied_tick) +
            "|player=" + player.player_id +
            "|total_damage=" + std::to_string(boss_damage_total_)
        );
        ++event_count_;
    }
}

void BattleSimulation::ApplyModeActionsForTick(std::uint64_t tick) {
    const auto actions_it = pending_mode_actions_by_tick_.find(tick);
    if (actions_it == pending_mode_actions_by_tick_.end()) {
        return;
    }

    auto actions = actions_it->second;
    std::sort(actions.begin(), actions.end(), [](const BattleModeAction& left, const BattleModeAction& right) {
        if (left.seq != right.seq) {
            return left.seq < right.seq;
        }
        if (left.player_id != right.player_id) {
            return left.player_id < right.player_id;
        }
        return left.action_id < right.action_id;
    });
    for (const auto& action : actions) {
        if (IsReconnectModeAction(action.action_type)) {
            pending_reconnect_player_ids_.erase(action.player_id);
            auto player_it = players_.find(action.player_id);
            if (player_it != players_.end() && !player_it->second.connected) {
                player_it->second.connected = true;
                AccumulateConnectionEvent(player_it->second);
            }
        }
        if (action.action_type == "ready") {
            ApplyReadyModeAction(action);
        }
        if (action.action_type == "transfer_card") {
            if (!ApplyTransferCardModeAction(action)) {
                continue;
            }
        }
        AccumulateAcceptedModeAction(action);
    }
    pending_mode_actions_by_tick_.erase(actions_it);
}

void BattleSimulation::ApplyReadyModeAction(const BattleModeAction& action) {
    ready_player_ids_.insert(action.player_id);
    pending_ready_player_ids_.erase(action.player_id);
}

bool BattleSimulation::ApplyTransferCardModeAction(const BattleModeAction& action) {
    const std::string target_player_id = ExtractJsonStringField(action.payload_json, "target_player_id");
    const std::string card_instance_id = ExtractJsonStringField(action.payload_json, "card_instance_id");
    if (target_player_id.empty() || card_instance_id.empty()) {
        return false;
    }

    auto reject_queued_transfer = [&]() {
        reserved_transfer_card_instance_ids_.erase(card_instance_id);
        pending_transfer_card_authority_by_action_id_.erase(action.action_id);
    };

    const auto source_it = players_.find(action.player_id);
    const auto target_it = players_.find(target_player_id);
    if (source_it == players_.end() || target_it == players_.end() ||
        !source_it->second.connected || !target_it->second.connected) {
        reject_queued_transfer();
        return false;
    }
    const auto authority_it = pending_transfer_card_authority_by_action_id_.find(action.action_id);
    if (authority_it == pending_transfer_card_authority_by_action_id_.end()) {
        reject_queued_transfer();
        return false;
    }
    const auto latest_card_it = transferable_cards_.find(card_instance_id);
    if (latest_card_it == transferable_cards_.end() ||
        latest_card_it->second.owner_player_id != action.player_id ||
        !latest_card_it->second.mode_allowed ||
        !latest_card_it->second.cost_paid ||
        !latest_card_it->second.cooldown_ready) {
        reject_queued_transfer();
        return false;
    }

    transferred_card_edges_[card_instance_id] = {action.player_id, target_player_id};
    last_transfer_card_instance_id_ = card_instance_id;
    last_transfer_from_player_id_ = action.player_id;
    last_transfer_to_player_id_ = target_player_id;
    last_transfer_card_authority_ = latest_card_it->second;
    transferred_card_authority_by_card_instance_id_[card_instance_id] = latest_card_it->second;
    pending_transfer_card_authority_by_action_id_.erase(authority_it);
    ++transfer_card_count_;
    return true;
}

std::string BattleSimulation::TransferCardAuditMaterial() const {
    std::ostringstream out;
    for (const auto& item : transferred_card_edges_) {
        const auto authority_it = transferred_card_authority_by_card_instance_id_.find(item.first);
        out << item.first << ':'
            << item.second.first << '>'
            << item.second.second << ':';
        if (authority_it != transferred_card_authority_by_card_instance_id_.end()) {
            out << authority_it->second.owner_player_id << ':'
                << BoolToken(authority_it->second.mode_allowed) << ':'
                << BoolToken(authority_it->second.cost_paid) << ':'
                << BoolToken(authority_it->second.cooldown_ready);
        } else {
            out << "missing:0:0:0";
        }
        out << ';';
    }
    return out.str();
}

void BattleSimulation::SpawnBulletsForTick() {
    if (current_tick_ == 0 || (current_tick_ % config_.spawn_period_ticks) != 0 || bullets_.size() >= config_.max_bullets) {
        return;
    }
    if (IsBossMode(config_.mode_id) && !boss_combat_started_) {
        return;
    }

    const std::uint64_t mixed = MixSeed(current_tick_);
    const std::int32_t drift = static_cast<std::int32_t>(mixed % 7000u) - 3500;
    const bool boss_mode = IsBossMode(config_.mode_id);
    const std::array<std::pair<std::int32_t, std::int32_t>, 8> boss_velocities = {{
        {0, 2600},
        {1840, 1840},
        {2600, 0},
        {1840, -1840},
        {0, -2600},
        {-1840, -1840},
        {-2600, 0},
        {-1840, 1840},
    }};
    const std::array<std::pair<std::int32_t, std::int32_t>, 4> duel_velocities = {{
        {0, 3000},
        {3000, 0},
        {0, -3000},
        {-3000, 0},
    }};
    const std::size_t velocity_count = boss_mode ? boss_velocities.size() : duel_velocities.size();

    for (std::size_t i = 0; i < velocity_count && bullets_.size() < config_.max_bullets; ++i) {
        const auto velocity = boss_mode ? boss_velocities[i] : duel_velocities[i];
        BulletState bullet;
        bullet.bullet_id = "b" + std::to_string(next_bullet_id_++);
        bullet.x_milli = boss_mode ? 0 : (i % 2 == 0 ? drift : 0);
        bullet.y_milli = boss_mode ? 0 : (i % 2 == 0 ? 0 : drift);
        bullet.vx_milli = velocity.first;
        bullet.vy_milli = velocity.second;
        bullet.radius_milli = boss_mode ? 5000 : 4000;
        bullet.pattern_id = boss_mode ? "boss_center_radial" : "basic_radial";
        bullet.color = boss_mode ? (config_.mode_id == "world_boss" ? "ruby" : "violet") : ((i % 2 == 0) ? "red" : "blue");
        bullets_.push_back(bullet);
    }

    event_stream_hash_ = HashAppend(event_stream_hash_, current_tick_);
    event_stream_hash_ = HashAppend(event_stream_hash_, next_bullet_id_);
    std::string event_trace =
        "bullet_spawn|tick=" + std::to_string(current_tick_) +
        "|next_id=" + std::to_string(next_bullet_id_) +
        "|count=" + std::to_string(bullets_.size());
    if (boss_mode) {
        event_trace += "|pattern=boss_center_radial";
    }
    event_trace_.push_back(std::move(event_trace));
    ++event_count_;
}

void BattleSimulation::AdvanceBullets() {
    for (auto& bullet : bullets_) {
        bullet.x_milli += bullet.vx_milli;
        bullet.y_milli += bullet.vy_milli;
    }

    bullets_.erase(
        std::remove_if(bullets_.begin(), bullets_.end(), [](const BulletState& bullet) {
            return bullet.x_milli < -kArenaHalfWidthMilli || bullet.x_milli > kArenaHalfWidthMilli ||
                bullet.y_milli < -kArenaHalfHeightMilli || bullet.y_milli > kArenaHalfHeightMilli;
        }),
        bullets_.end()
    );
}

void BattleSimulation::AccumulateAcceptedInput(const BattleInput& input) {
    input_stream_hash_ = HashAppend(input_stream_hash_, input.match_id);
    input_stream_hash_ = HashAppend(input_stream_hash_, input.player_id);
    input_stream_hash_ = HashAppend(input_stream_hash_, input.tick);
    input_stream_hash_ = HashAppend(input_stream_hash_, input.seq);
    input_stream_hash_ = HashAppend(input_stream_hash_, input.direction_bits);
    input_stream_hash_ = HashAppend(input_stream_hash_, input.slow ? 1u : 0u);
    input_stream_hash_ = HashAppend(input_stream_hash_, input.shoot ? 1u : 0u);
    input_stream_hash_ = HashAppend(input_stream_hash_, input.bomb ? 1u : 0u);
    input_stream_hash_ = HashAppendSigned(input_stream_hash_, input.card_slot);
    input_stream_hash_ = HashAppend(input_stream_hash_, input.mode_action_id);
    input_trace_.push_back(
        "input|" + input.player_id +
        "|tick=" + std::to_string(input.tick) +
        "|seq=" + std::to_string(input.seq) +
        "|dir=" + std::to_string(input.direction_bits) +
        "|slow=" + BoolToken(input.slow) +
        "|shoot=" + BoolToken(input.shoot) +
        "|bomb=" + BoolToken(input.bomb) +
        "|card=" + std::to_string(input.card_slot)
    );
    ++accepted_input_count_;
}

void BattleSimulation::AccumulateFallbackInput(const PlayerState& player, const BattleInput& input) {
    input_stream_hash_ = HashAppend(input_stream_hash_, config_.match_id);
    input_stream_hash_ = HashAppend(input_stream_hash_, player.player_id);
    input_stream_hash_ = HashAppend(input_stream_hash_, input.tick);
    input_stream_hash_ = HashAppend(input_stream_hash_, input.seq);
    input_stream_hash_ = HashAppend(input_stream_hash_, input.direction_bits);
    input_stream_hash_ = HashAppend(input_stream_hash_, input.slow ? 1u : 0u);
    input_stream_hash_ = HashAppend(input_stream_hash_, input.shoot ? 1u : 0u);
    input_stream_hash_ = HashAppend(input_stream_hash_, input.bomb ? 1u : 0u);
    input_stream_hash_ = HashAppendSigned(input_stream_hash_, input.card_slot);
    input_stream_hash_ = HashAppend(input_stream_hash_, "fallback");
    const bool neutral = input.seq == 0 && input.direction_bits == 0 && !input.slow && !input.shoot && !input.bomb &&
        input.card_slot == -1;
    input_trace_.push_back(
        std::string("fallback|") + (neutral ? "neutral" : "held") +
        "|" + player.player_id +
        "|tick=" + std::to_string(input.tick) +
        "|seq=" + std::to_string(input.seq) +
        "|dir=" + std::to_string(input.direction_bits) +
        "|slow=" + BoolToken(input.slow) +
        "|shoot=" + BoolToken(input.shoot) +
        "|bomb=" + BoolToken(input.bomb) +
        "|card=" + std::to_string(input.card_slot)
    );
    ++fallback_input_count_;
    if (neutral) {
        ++neutral_fallback_count_;
    } else {
        ++held_input_fallback_count_;
    }
}

void BattleSimulation::AccumulateAcceptedModeAction(const BattleModeAction& action) {
    has_last_mode_action_ = true;
    last_mode_action_ = action;
    event_stream_hash_ = HashAppend(event_stream_hash_, action.match_id);
    event_stream_hash_ = HashAppend(event_stream_hash_, action.player_id);
    event_stream_hash_ = HashAppend(event_stream_hash_, action.tick);
    event_stream_hash_ = HashAppend(event_stream_hash_, action.seq);
    event_stream_hash_ = HashAppend(event_stream_hash_, action.action_id);
    event_stream_hash_ = HashAppend(event_stream_hash_, action.action_type);
    event_stream_hash_ = HashAppend(event_stream_hash_, action.payload_json);
    if (action.action_type == "transfer_card") {
        event_stream_hash_ = HashAppend(event_stream_hash_, last_transfer_card_instance_id_);
        event_stream_hash_ = HashAppend(event_stream_hash_, last_transfer_from_player_id_);
        event_stream_hash_ = HashAppend(event_stream_hash_, last_transfer_to_player_id_);
        event_stream_hash_ = HashAppend(event_stream_hash_, last_transfer_card_authority_.owner_player_id);
        event_stream_hash_ = HashAppend(event_stream_hash_, last_transfer_card_authority_.mode_allowed ? 1u : 0u);
        event_stream_hash_ = HashAppend(event_stream_hash_, last_transfer_card_authority_.cost_paid ? 1u : 0u);
        event_stream_hash_ = HashAppend(event_stream_hash_, last_transfer_card_authority_.cooldown_ready ? 1u : 0u);
        event_stream_hash_ = HashAppend(event_stream_hash_, transfer_card_count_);
    }
    std::string trace =
        "mode_action|" + action.player_id +
        "|tick=" + std::to_string(action.tick) +
        "|seq=" + std::to_string(action.seq) +
        "|id=" + action.action_id +
        "|type=" + action.action_type;
    if (action.action_type == "transfer_card") {
        trace += "|card=" + last_transfer_card_instance_id_ +
            "|from=" + last_transfer_from_player_id_ +
            "|to=" + last_transfer_to_player_id_ +
            "|authority_owner=" + last_transfer_card_authority_.owner_player_id +
            "|mode_allowed=" + BoolToken(last_transfer_card_authority_.mode_allowed) +
            "|cost_paid=" + BoolToken(last_transfer_card_authority_.cost_paid) +
            "|cooldown_ready=" + BoolToken(last_transfer_card_authority_.cooldown_ready);
    }
    event_trace_.push_back(std::move(trace));
    ++mode_action_count_;
    ++event_count_;
}

void BattleSimulation::AccumulateConnectionEvent(const PlayerState& player) {
    event_stream_hash_ = HashAppend(event_stream_hash_, config_.match_id);
    event_stream_hash_ = HashAppend(event_stream_hash_, player.player_id);
    event_stream_hash_ = HashAppend(event_stream_hash_, current_tick_);
    event_stream_hash_ = HashAppend(event_stream_hash_, player.connected ? "connected" : "disconnected");
    event_trace_.push_back(
        std::string("connection|") + (player.connected ? "connected" : "disconnected") +
        "|" + player.player_id +
        "|tick=" + std::to_string(current_tick_)
    );
    ++event_count_;
}

std::string InputValidationCodeName(InputValidationCode code) {
    switch (code) {
        case InputValidationCode::Ok:
            return "ok";
        case InputValidationCode::VersionIncompatible:
            return "version_incompatible";
        case InputValidationCode::MatchUnknown:
            return "match_unknown";
        case InputValidationCode::MatchSettled:
            return "match_settled";
        case InputValidationCode::MatchMismatch:
            return "match_mismatch";
        case InputValidationCode::PlayerUnknown:
            return "player_unknown";
        case InputValidationCode::SeqMissing:
            return "seq_missing";
        case InputValidationCode::SeqReplay:
            return "seq_replay";
        case InputValidationCode::DuplicateInputForTick:
            return "input_tick_duplicate";
        case InputValidationCode::TickTooOld:
            return "input_tick_too_old";
        case InputValidationCode::TickTooFarAhead:
            return "input_tick_too_far_ahead";
        case InputValidationCode::InvalidDirectionBits:
            return "invalid_direction_bits";
        case InputValidationCode::InvalidCardSlot:
            return "invalid_card_slot";
        case InputValidationCode::InvalidModeAction:
            return "invalid_mode_action";
        case InputValidationCode::PlayerDisconnected:
            return "player_disconnected";
        case InputValidationCode::SeqTooFarAhead:
            return "seq_too_far_ahead";
        case InputValidationCode::EventCursorAhead:
            return "event_cursor_ahead";
        case InputValidationCode::DuplicateModeAction:
            return "mode_action_duplicate";
        case InputValidationCode::ReadyAlreadySet:
            return "ready_already_set";
        case InputValidationCode::BossMinPlayersNotMet:
            return "boss_min_players_not_met";
    }
    return "unknown";
}

}  // namespace phk::battle
