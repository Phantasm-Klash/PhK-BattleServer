#include "phk/battle/simulation.hpp"

#include "phk/battle/ticket.hpp"  // ParseDecimalInt64Text（票据 server_seed 文本解析）

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
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

std::size_t JsonFieldValueStart(std::string_view payload_json, std::string_view field_name);

std::string ExtractJsonStringField(std::string_view payload_json, std::string_view field_name) {
    const auto value_start = JsonFieldValueStart(payload_json, field_name);
    if (value_start == std::string_view::npos ||
        value_start >= payload_json.size() ||
        payload_json[value_start] != '"') {
        return "";
    }
    const auto string_start = value_start + 1;
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

bool IsTopLevelJsonObjectKey(std::string_view payload_json, std::size_t key_start) {
    int object_depth = 0;
    int array_depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t index = 0; index < key_start; ++index) {
        const char ch = payload_json[index];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '{') {
            ++object_depth;
        } else if (ch == '}') {
            --object_depth;
        } else if (ch == '[') {
            ++array_depth;
        } else if (ch == ']') {
            --array_depth;
        }
    }
    return !in_string && object_depth == 1 && array_depth == 0;
}

std::size_t JsonFieldValueStart(std::string_view payload_json, std::string_view field_name) {
    const std::string key = "\"" + std::string(field_name) + "\"";
    std::size_t search_start = 0;
    while (search_start < payload_json.size()) {
        const auto key_start = payload_json.find(key, search_start);
        if (key_start == std::string_view::npos) {
            return std::string_view::npos;
        }
        if (!IsTopLevelJsonObjectKey(payload_json, key_start)) {
            search_start = key_start + 1;
            continue;
        }
        auto value_start = key_start + key.size();
        while (value_start < payload_json.size() &&
            std::isspace(static_cast<unsigned char>(payload_json[value_start]))) {
            ++value_start;
        }
        if (value_start < payload_json.size() && payload_json[value_start] == ':') {
            ++value_start;
            while (value_start < payload_json.size() &&
                std::isspace(static_cast<unsigned char>(payload_json[value_start]))) {
                ++value_start;
            }
            return value_start;
        }
        search_start = key_start + 1;
    }
    return std::string_view::npos;
}

std::optional<bool> ExtractJsonBoolField(std::string_view payload_json, std::string_view field_name) {
    const auto token_start = JsonFieldValueStart(payload_json, field_name);
    if (token_start == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t token_end = token_start;
    if (payload_json.substr(token_start, 4) == "true") {
        token_end += 4;
    } else if (payload_json.substr(token_start, 5) == "false") {
        token_end += 5;
    } else {
        return std::nullopt;
    }
    while (token_end < payload_json.size() &&
        std::isspace(static_cast<unsigned char>(payload_json[token_end]))) {
        ++token_end;
    }
    if (token_end < payload_json.size() &&
        payload_json[token_end] != ',' &&
        payload_json[token_end] != '}') {
        return std::nullopt;
    }
    return payload_json.substr(token_start, 4) == "true";
}

std::optional<std::int64_t> ExtractJsonIntField(std::string_view payload_json, std::string_view field_name) {
    const auto value_start = JsonFieldValueStart(payload_json, field_name);
    if (value_start == std::string_view::npos) {
        return std::nullopt;
    }
    auto token_start = value_start;
    while (token_start < payload_json.size() &&
        std::isspace(static_cast<unsigned char>(payload_json[token_start]))) {
        ++token_start;
    }
    const auto number_start = token_start;
    if (token_start < payload_json.size() && payload_json[token_start] == '-') {
        ++token_start;
    }
    if (token_start >= payload_json.size() ||
        !std::isdigit(static_cast<unsigned char>(payload_json[token_start]))) {
        return std::nullopt;
    }

    while (token_start < payload_json.size() &&
        std::isdigit(static_cast<unsigned char>(payload_json[token_start]))) {
        ++token_start;
    }
    const auto number_end = token_start;
    while (token_start < payload_json.size() &&
        std::isspace(static_cast<unsigned char>(payload_json[token_start]))) {
        ++token_start;
    }
    if (token_start < payload_json.size() &&
        payload_json[token_start] != ',' &&
        payload_json[token_start] != '}') {
        return std::nullopt;
    }

    std::int64_t value = 0;
    const auto parsed = std::from_chars(
        payload_json.data() + number_start,
        payload_json.data() + number_end,
        value
    );
    if (parsed.ec != std::errc{} || parsed.ptr != payload_json.data() + number_end) {
        return std::nullopt;
    }
    return value;
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
        // T10: input_timeout_ticks is a pure server-side tuning knob. It stays in
        // the live snapshot for observability, but must not enter the canonical
        // (hashed) view: a tuning knob that changes the replay fixture hash would
        // invalidate result verification.
        if (item.first == "input_timeout_ticks") {
            continue;
        }
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

    // T5 链式操作日志：genesis 条目 = 本局种子（append-only 审计链）。
    // 今日值为 DeriveMatchSeed（FNV-1a-64，server.cpp:1585，经
    // server.cpp:599 写入 config_.match_seed），seed_kind=battle_derived。
    // 大厅权威种子（seedFrom，SHA-256，service.go:6376）须经 loadout 的
    // server_seed 下发（T7 H-2/W-1）：落地后本行改传
    // lobby_authoritative，链结构与调用点不变。
    operation_log_.AppendMatchSeed(0, kChainSeedKindBattleDerived, config_.match_seed);
}

const SimulationConfig& BattleSimulation::Config() const {
    return config_;
}

const OperationChainLog& BattleSimulation::OperationLog() const {
    return operation_log_;
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
        player.ticks_without_input = 0;
        if (connected) {
            // Rejoin from neutral input; never resume a stale held direction/card.
            player.last_input = BattleInput{};
            player.last_input.match_id = config_.match_id;
            player.last_input.player_id = player.player_id;
            player.last_input.card_slot = -1;
        }
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
    if (input.seq > player_it->second.last_seq &&
        input.seq - player_it->second.last_seq > config_.max_seq_ahead) {
        result.code = InputValidationCode::SeqTooFarAhead;
        result.reason = "seq_too_far_ahead";
        return result;
    }
    if (input.tick <= current_tick_) {
        result.code = InputValidationCode::TickTooOld;
        result.reason = "input_tick_too_old";
        return result;
    }
    if (input.tick > current_tick_ &&
        input.tick - current_tick_ > config_.max_input_ahead_ticks) {
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
    // 传输层 sanity check（M-1，T7 §3）：-1 = "本 tick 不出牌"哨兵
    // （BattleInput.card_slot 的结构默认值，与 lobby validateInputPacket 的
    // card_slot -1..3 检查同级），0..kCardHandLimit 为 wire 粗筛（大厅权威
    // 校验：-1 或 slot < hand.size() <= kCardHandLimit=4）。**权威上界是
    // 手牌长度 hand.size()-1（恒 <= kCardHandLimit-1）**：由 card_hand.hpp
    // 的 ValidateCast 执行，slot >= hand.size() 一律走 card_rejected 拒绝
    // 路径（cast_card_slot_empty / cast_card_slot_out_of_range）。本层不
    // 判定手牌，保持粗筛定位。
    if (input.card_slot < -1 || input.card_slot > kCardHandLimit) {
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
    // T5 链式操作日志：按接受序入链（被拒输入在 ValidateInput 早退，不进链）。
    operation_log_.AppendInput(
        static_cast<std::int64_t>(input.tick),
        input.player_id,
        input.seq,
        input.direction_bits,
        input.slow,
        input.shoot,
        input.bomb,
        input.card_slot,
        input.mode_action_id
    );
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
    if (action.seq > player_it->second.last_seq &&
        action.seq - player_it->second.last_seq > config_.max_seq_ahead) {
        result.code = InputValidationCode::SeqTooFarAhead;
        result.reason = "seq_too_far_ahead";
        return result;
    }
    if (action.tick <= current_tick_) {
        result.code = InputValidationCode::TickTooOld;
        result.reason = "mode_action_tick_too_old";
        return result;
    }
    if (action.tick > current_tick_ &&
        action.tick - current_tick_ > config_.max_input_ahead_ticks) {
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
        // 传输层 sanity check（M-1，T7 §3）：显式 cast_card 动作要求槽位
        // >= 0 且 <= kCardHandLimit（大厅权威：slot < hand.size() <= 4）。
        // 权威上界同为 hand.size()-1，由 card_hand.hpp 的 ValidateCast
        // 执行并走 card_rejected 拒绝路径（cast_card_slot_empty /
        // cast_card_slot_out_of_range）；本动作将在 tick 应用处经
        // ValidateCast 复核。
        if (card_slot.value() < 0 || card_slot.value() > kCardHandLimit) {
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

    // T5 链式操作日志：mode action 按接受序入链（被拒动作不进链；
    // action_type 用真实类型，payload_json 原始字节入 digest）。
    operation_log_.AppendModeAction(
        static_cast<std::int64_t>(action.tick),
        action.player_id,
        action.action_id,
        action.seq,
        action.action_type,
        action.payload_json
    );

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
        bool real_input = false;
        if (pending_it != pending_inputs_by_tick_.end()) {
            const auto input_it = pending_it->second.find(player.player_id);
            if (input_it != pending_it->second.end()) {
                input = input_it->second;
                real_input = true;
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
        if (real_input) {
            player.ticks_without_input = 0;
        } else if (config_.input_timeout_ticks > 0) {
            if (player.ticks_without_input < config_.input_timeout_ticks) {
                ++player.ticks_without_input;
            }
            if (player.ticks_without_input >= config_.input_timeout_ticks) {
                // Silence is a passive drop: deactivate the player at the
                // timeout boundary. Tick already skips disconnected players,
                // so held fallback cannot move/shoot/cast after this point.
                const auto disconnected = SetPlayerConnected(player.player_id, false);
                (void)disconnected;
                continue;
            }
        }
        // 手牌能量逐 tick 回复（大厅权威语义，service.go:4816-4821）：
        // 仅存活（connected）玩家、每 tick 一次
        // min(kCardMaxEnergy, energy + kCardEnergyPerTick)，逐 tick 累加
        // （不按 elapsed 批量），与大厅哈希一致；timeout 断开当 tick 起冻结。
        // 回复先于本 tick 出牌扣费（同 Go applyInputLocked 先 accrue 再
        // 处理输入的顺序）。
        player.hand.energy =
            std::min(kCardMaxEnergy, player.hand.energy + kCardEnergyPerTick);
        ApplyInput(player, input, tick_to_apply);
        // T3 step4：出牌意图（Go 同构通道，service.go:3219 packet.CardSlot>=0）。
        // 只消费**真实**客户端输入——held fallback 不重放 card_slot，避免陈旧
        // 意图逐 tick 重放烧能量；-1 为不出牌哨兵（struct default），直接跳过
        // （无事件行、无状态变更）。
        if (real_input && input.card_slot >= 0) {
            ExecuteCastCard(player.player_id, input.card_slot, static_cast<std::int64_t>(tick_to_apply));
        }
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
        // B3：先生成、再移动、**再判定接触**（与 Go simulation.go:37-42 同序）。
        ResolveBulletContacts();
    }
    // T10 rule 2：本 tick 可能刚把最后一名玩家判为被动掉线；若整队已掉线，
    // 在对局级事件行里发一次 all_players_disconnected（去重，只发一次）。
    MaybeEmitAllPlayersDisconnected();
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
    snapshot.mode_state["input_timeout_ticks"] = std::to_string(config_.input_timeout_ticks);
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
        summary.boss_defeated = BossDefeated() ? 1u : 0u;
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
    record.boss_defeated = summary.boss_defeated;
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
        // T10: ticks_without_input is bookkeeping for the input_timeout_ticks
        // tuning knob, not authoritative gameplay state. Its observable effect
        // (the connected flag hashed just above) is already covered, so the raw
        // counter is deliberately kept out of the canonical state: a tuning/test
        // knob must not perturb the signed result hash.
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
        << record.boss_defeated << '|'
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
            << summary.boss_defeated << '|'
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
        hash = HashAppend(hash, summary.boss_defeated);
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
    RegisterBossDamage(player.player_id, applied_tick, damage);
}

bool BattleSimulation::RegisterBossDamage(
    const std::string& player_id,
    std::uint64_t tick,
    std::uint64_t damage
) {
    if (boss_current_hp_ == 0) {
        return false;
    }
    const std::uint64_t applied = std::min(damage, boss_current_hp_);
    boss_current_hp_ -= applied;
    boss_damage_total_ += applied;
    boss_damage_by_player_[player_id] += applied;

    if (boss_current_hp_ == 0 && boss_defeated_tick_ == 0) {
        boss_defeated_tick_ = tick;
        event_stream_hash_ = HashAppend(event_stream_hash_, config_.match_id);
        event_stream_hash_ = HashAppend(event_stream_hash_, config_.mode_id);
        event_stream_hash_ = HashAppend(event_stream_hash_, tick);
        event_stream_hash_ = HashAppend(event_stream_hash_, player_id);
        event_stream_hash_ = HashAppend(event_stream_hash_, boss_damage_total_);
        event_trace_.push_back(
            "boss_defeated|tick=" + std::to_string(tick) +
            "|player=" + player_id +
            "|total_damage=" + std::to_string(boss_damage_total_)
        );
        ++event_count_;
        return true;
    }
    return false;
}

// ---- T3 step4：出牌执行器 ----

std::int64_t BattleSimulation::LobbySeed() const {
    return card_purge_seed_;
}

bool BattleSimulation::SeedPlayerHand(
    const std::string& player_id,
    const std::vector<std::string>& hand_card_ids,
    double initial_energy,
    const std::string& server_seed_decimal,
    const std::vector<std::string>& deck_card_ids
) {
    auto player_it = players_.find(player_id);
    if (player_it == players_.end()) {
        return false;
    }
    // 对局级大厅种子（T7 H-2）：首张含种子的票据设定；同场第二张票据携带
    // 不同种子 = 大厅不变量违例，保持首个值并告知调用方（RegisterTicket 记录）。
    if (!server_seed_decimal.empty()) {
        std::int64_t seed = 0;
        if (!ParseDecimalInt64Text(server_seed_decimal, seed)) {
            return false;
        }
        if (card_purge_seed_set_ && seed != card_purge_seed_) {
            return false;
        }
        card_purge_seed_ = seed;
        card_purge_seed_set_ = true;
    }
    PlayerState& player = player_it->second;
    player.hand.card_ids = hand_card_ids;
    player.hand.energy = initial_energy;
    // W-1 phase 2（票 field 18 deck_card_ids）：牌堆随票据下发，出牌后可补抽。
    // draw_cursor 从大厅发牌游标续跑：hand = deck prefix（TicketVerifier 的
    // deck_hand_not_prefix 已保不变量），故开局消耗恰为 hand_card_ids.size() 张。
    // 未下发牌堆（空）时保持旧行为：牌堆空、游标 0，DrawServerCard 为 no-op。
    player.hand.deck = deck_card_ids;
    player.hand.draw_cursor = deck_card_ids.empty() ? 0 : hand_card_ids.size();
    player.invulnerable_until = 0;
    return true;
}

std::size_t BattleSimulation::HandSize(const std::string& player_id) const {
    const auto player_it = players_.find(player_id);
    if (player_it == players_.end()) {
        return 0;
    }
    return player_it->second.hand.card_ids.size();
}

double BattleSimulation::PlayerEnergy(const std::string& player_id) const {
    const auto player_it = players_.find(player_id);
    if (player_it == players_.end()) {
        return 0.0;
    }
    return player_it->second.hand.energy;
}

std::int64_t BattleSimulation::PlayerInvulnerableUntil(const std::string& player_id) const {
    const auto player_it = players_.find(player_id);
    if (player_it == players_.end()) {
        return 0;
    }
    return player_it->second.invulnerable_until;
}

std::uint64_t BattleSimulation::PlayerHitCount(const std::string& player_id) const {
    const auto player_it = players_.find(player_id);
    if (player_it == players_.end()) {
        return 0;
    }
    return player_it->second.hit_count;
}

std::uint64_t BattleSimulation::PlayerGrazeCount(const std::string& player_id) const {
    const auto player_it = players_.find(player_id);
    if (player_it == players_.end()) {
        return 0;
    }
    return player_it->second.graze_count;
}

const std::vector<CardCastEvent>& BattleSimulation::CardCastEvents() const {
    return card_cast_events_;
}

void BattleSimulation::ClearCardCastEvents() {
    card_cast_events_.clear();
}

void BattleSimulation::PushCardCastEvent(CardCastEvent row) {
    if (card_cast_events_.size() >= kCardCastEventRowLimit) {
        // 有界：超限丢最旧一行（宿主长期不上报的防御，正常排空不会触发）。
        card_cast_events_.erase(card_cast_events_.begin());
    }
    card_cast_events_.push_back(std::move(row));
}

void BattleSimulation::MaybeEmitAllPlayersDisconnected() {
    if (all_players_disconnected_emitted_ || players_.empty()) {
        return;
    }
    for (const auto& item : players_) {
        if (item.second.connected) {
            // 仍有玩家在局内：终局条件未达成。
            return;
        }
    }
    all_players_disconnected_emitted_ = true;
    CardCastEvent row;
    row.event_type = kAllPlayersDisconnectedEventType;
    // 终局 tick = 最后一名玩家被判掉线的那一 tick（本函数只在 Tick() 尾部调用，
    // current_tick_ 已推进）。对局级行不带 user_id / card_id（大厅据此拒绝）。
    row.tick = static_cast<std::int64_t>(current_tick_);
    PushCardCastEvent(std::move(row));
}

std::vector<CardCastEvent> BattleSimulation::TakeCardCastEvents(std::size_t limit) {
    const std::size_t count = limit == 0 || limit >= card_cast_events_.size()
        ? card_cast_events_.size()
        : limit;
    std::vector<CardCastEvent> taken(
        card_cast_events_.begin(),
        card_cast_events_.begin() + static_cast<std::ptrdiff_t>(count)
    );
    card_cast_events_.erase(
        card_cast_events_.begin(),
        card_cast_events_.begin() + static_cast<std::ptrdiff_t>(count)
    );
    return taken;
}

void BattleSimulation::RestoreCardCastEvents(const std::vector<CardCastEvent>& rows) {
    if (rows.empty()) {
        return;
    }
    card_cast_events_.insert(card_cast_events_.begin(), rows.begin(), rows.end());
    // 回传失败的行放回队首；容量上限内保留最新，超限丢最旧（有界防 OOM）。
    if (card_cast_events_.size() > kCardCastEventRowLimit) {
        card_cast_events_.erase(
            card_cast_events_.begin(),
            card_cast_events_.begin() + static_cast<std::ptrdiff_t>(
                card_cast_events_.size() - kCardCastEventRowLimit)
        );
    }
}

void BattleSimulation::ApplyCastCardModeAction(const BattleModeAction& action, std::int64_t tick) {
    // 传输层已校验 cast_card 必带 card_slot 且 ∈ 0..7（simulation.cpp:899-915）；
    // 缺失时按哨兵 -1（不出牌、无副作用）防御处理。
    const auto card_slot = ExtractJsonIntField(action.payload_json, "card_slot");
    const int slot = card_slot.has_value() ? static_cast<int>(card_slot.value()) : -1;
    ExecuteCastCard(action.player_id, slot, tick);
}

void BattleSimulation::ExecuteCastCard(const std::string& player_id, int slot, std::int64_t tick) {
    auto player_it = players_.find(player_id);
    if (player_it == players_.end()) {
        return;
    }
    PlayerState& player = player_it->second;

    // 校验 + 账本（扣能量/写冷却/移除出牌槽/补抽；拒绝与哨兵不改状态）。
    const CastValidationResult cast = ApplyCast(player.hand, slot, tick);
    if (cast.plays_no_card) {
        // -1 哨兵（BattleInput.card_slot struct default）：不出牌、无事件、
        // 无状态变更（M-1；card_hand.hpp 头注释）。
        return;
    }
    if (!cast.accepted) {
        CardCastEvent row;
        row.event_type = "card_rejected";
        row.tick = tick;
        row.player_id = player_id;
        row.slot = slot;
        row.reason = cast.reason;
        PushCardCastEvent(std::move(row));
        return;
    }
    const CardDefinition& definition = *cast.definition;

    // boss 伤害（通用激活路径语义，Go applyCardRequestLocked：activation.Damage
    // 施加到 BossHP，且不重复于效果层）。
    const std::int64_t damage = definition.damage > 0 ? definition.damage : 0;
    if (IsBossMode(config_.mode_id) && damage > 0 && boss_current_hp_ > 0) {
        if (RegisterBossDamage(player_id, static_cast<std::uint64_t>(tick), static_cast<std::uint64_t>(damage))) {
            // lobby 已识别 boss_defeated 行并路由 mixed-pool draft（T9）；
            // reason 携带 boss id（validateBattleEventRowLocked 约定）。
            CardCastEvent row;
            row.event_type = "boss_defeated";
            row.tick = tick;
            row.player_id = player_id;
            row.reason = config_.boss_instance_id;
            PushCardCastEvent(std::move(row));
        }
    }

    CardCastEvent accepted;
    accepted.event_type = "card_accepted";
    accepted.tick = tick;
    accepted.player_id = player_id;
    accepted.card_id = definition.card_id;
    accepted.event_value = definition.damage;
    accepted.slot = slot;
    PushCardCastEvent(std::move(accepted));

    RunCardEffect(player, player_id, BuildCardActivation(definition, tick), tick);
}

std::vector<CardBulletSnapshot> BattleSimulation::BuildBulletSnapshots() const {
    std::vector<CardBulletSnapshot> snapshots;
    snapshots.reserve(bullets_.size());
    for (const BulletState& bullet : bullets_) {
        CardBulletSnapshot snapshot;
        snapshot.bullet_id = bullet.bullet_id;
        snapshot.pattern_id = bullet.pattern_id;
        snapshot.x = static_cast<double>(bullet.x_milli) / 1000.0;
        snapshot.y = static_cast<double>(bullet.y_milli) / 1000.0;
        snapshot.vx = static_cast<double>(bullet.vx_milli) / 1000.0;
        snapshot.vy = static_cast<double>(bullet.vy_milli) / 1000.0;
        snapshot.radius = static_cast<double>(bullet.radius_milli) / 1000.0;
        snapshot.color = bullet.color;
        snapshots.push_back(std::move(snapshot));
    }
    return snapshots;
}

void BattleSimulation::RunCardEffect(
    const PlayerState& caster,
    const std::string& caster_id,
    const CardActivation& activation,
    std::int64_t tick
) {
    CardEffectResult effect;
    if (activation.effect_kind == "opponent_pattern") {
        std::vector<CardPlayerSnapshot> players;
        players.reserve(players_.size());
        for (const auto& item : players_) {
            CardPlayerSnapshot snapshot;
            snapshot.player_id = item.second.player_id;
            snapshot.x = static_cast<double>(item.second.x_milli) / 1000.0;
            snapshot.y = static_cast<double>(item.second.y_milli) / 1000.0;
            players.push_back(std::move(snapshot));
        }
        effect = ApplyOpponentPattern(
            players,
            caster_id,
            activation,
            tick,
            config_.match_id,
            next_bullet_id_,
            bullets_.size()
        );
    } else if (activation.effect_kind == "bullet_purge") {
        // T7 H-2：选取种子 = 票据 server_seed（card_purge_seed_），
        // 绝不用 DeriveMatchSeed（config_.match_seed）。
        CardPlayerSnapshot caster_snapshot;
        caster_snapshot.player_id = caster_id;
        caster_snapshot.x = static_cast<double>(caster.x_milli) / 1000.0;
        caster_snapshot.y = static_cast<double>(caster.y_milli) / 1000.0;
        effect = ApplyBulletPurge(
            BuildBulletSnapshots(),
            caster_snapshot,
            activation,
            card_purge_seed_,
            tick
        );
    } else if (activation.effect_kind == "invulnerable") {
        effect = ApplyInvulnerable(activation, tick);
    } else if (activation.effect_kind == "finisher") {
        effect = ApplyFinisher(BuildBulletSnapshots(), tick);
    } else {
        // self/pattern/economy（其余 12 张）：账本已变更但无子弹效果
        // （Go applyCardEffectLocked 的 switch 同样不处理，不发
        // card_effect_applied）。
        return;
    }

    ApplyBulletDeltas(effect.bullet_deltas);
    if (activation.effect_kind == "invulnerable") {
        auto target_it = players_.find(caster_id);
        if (target_it != players_.end()) {
            target_it->second.invulnerable_until = effect.invulnerable_until;
        }
    }

    CardCastEvent row;
    row.event_type = "card_effect_applied";
    row.tick = tick;
    row.player_id = caster_id;
    row.card_id = activation.card_id;
    row.event_value = effect.event_value;
    row.target_user_id = effect.target_user_id;
    PushCardCastEvent(std::move(row));
}

void BattleSimulation::ApplyBulletDeltas(const std::vector<CardBulletDelta>& deltas) {
    for (const CardBulletDelta& delta : deltas) {
        if (delta.op == "spawn") {
            if (bullets_.size() >= config_.max_bullets) {
                continue;  // 与 addBulletLocked 封顶一致（效果层已按场数预判）
            }
            BulletState bullet;
            // 统一 id 方案：丢弃效果层的 Go 风格 id，用模拟既有 "b<N>" 编号
            // （效果层收到的 next_bullet_seq = next_bullet_id_，同值同序）。
            bullet.bullet_id = "b" + std::to_string(next_bullet_id_++);
            bullet.x_milli = static_cast<std::int32_t>(std::llround(delta.x * 1000.0));
            bullet.y_milli = static_cast<std::int32_t>(std::llround(delta.y * 1000.0));
            bullet.vx_milli = static_cast<std::int32_t>(std::llround(delta.vx * 1000.0));
            bullet.vy_milli = static_cast<std::int32_t>(std::llround(delta.vy * 1000.0));
            const double radius_milli = std::llround(delta.radius * 1000.0);
            bullet.radius_milli = radius_milli > 0.0
                ? static_cast<std::uint32_t>(radius_milli)
                : 0u;
            bullet.pattern_id = delta.pattern_id;
            bullet.color = delta.color;
            bullets_.push_back(std::move(bullet));
        } else if (delta.op == "despawn") {
            const auto it = std::find_if(bullets_.begin(), bullets_.end(), [&](const BulletState& bullet) {
                return bullet.bullet_id == delta.bullet_id;
            });
            if (it != bullets_.end()) {
                bullets_.erase(it);
            }
        }
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
        if (action.action_type == "cast_card") {
            ApplyCastCardModeAction(action, static_cast<std::int64_t>(tick));
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

// B3：子弹↔玩家接触判定 + 无敌窗口消费。镜像 Go resolveBulletContactsLocked
// （Gensoulkyo/runtime/core/simulation.go:191-229），在 AdvanceBullets() 之后调用。
// 语义：欧氏距离 <= 玩家半径+子弹半径 ⇒ 命中（无敌时才掉血并刷新无敌帧，
// 无论是否无敌**子弹一律移除**；同 tick 一颗子弹只结算一个玩家，命中即 break）；
// 否则 <= 擦弹半径+子弹半径 ⇒ 擦弹（按 grazed_bullet_ids 去重）。
// 用整数平方距离（d2 <= R*R）等价于 Go 的 Hypot<=R，且无浮点不确定性。
// B3 step2：命中/擦弹同时向 card_cast_events_ 发一行（hit / graze，无 card_id），
// 复用 ≤64 行/批 → BuildBattleEventsJson → HMAC 上报路径；只写收集器，
// **不进任何 canonical 哈希**（见 simulation.hpp:201-202）。
void BattleSimulation::ResolveBulletContacts() {
    const std::int64_t tick = static_cast<std::int64_t>(current_tick_);
    for (auto bullet_it = bullets_.begin(); bullet_it != bullets_.end();) {
        const BulletState& bullet = *bullet_it;
        const std::int64_t radius = static_cast<std::int64_t>(bullet.radius_milli);
        const std::int64_t hit_radius = static_cast<std::int64_t>(kPlayerHitRadiusMilli) + radius;
        const std::int64_t graze_radius = static_cast<std::int64_t>(kPlayerGrazeRadiusMilli) + radius;
        bool removed = false;
        for (auto& player_item : players_) {
            PlayerState& player = player_item.second;
            const std::int64_t dx = static_cast<std::int64_t>(player.x_milli) - bullet.x_milli;
            const std::int64_t dy = static_cast<std::int64_t>(player.y_milli) - bullet.y_milli;
            const std::int64_t distance_sq = dx * dx + dy * dy;
            if (distance_sq <= hit_radius * hit_radius) {
                if (tick > player.invulnerable_until) {
                    ++player.hit_count;
                    player.invulnerable_until = tick + kPlayerHitInvulnerableTicks;
                    // B3 step2：命中行（Go simulation.go:210，内层分支内 ⇒ 被
                    // 无敌窗口吸收的命中不发）。Value = Go bullet.Damage；Go 所有
                    // 子弹 damage 恒为 1（addBulletLocked 的 4 个调用点均传 1），
                    // C++ BulletState 无 damage 字段，故直接镜像有效值 1。
                    CardCastEvent hit_row;
                    hit_row.event_type = "hit";
                    hit_row.tick = tick;
                    hit_row.player_id = player.player_id;
                    hit_row.event_value = 1;
                    PushCardCastEvent(std::move(hit_row));
                }
                removed = true;
                break;
            }
            if (distance_sq <= graze_radius * graze_radius) {
                if (player.grazed_bullet_ids.insert(bullet.bullet_id).second) {
                    ++player.graze_count;
                    // B3 step2：擦弹行（Go simulation.go:222，Value 为字面 1）。
                    // 只在去重集首次插入成功时发射，镜像 Go GrazedBullets 去重。
                    CardCastEvent graze_row;
                    graze_row.event_type = "graze";
                    graze_row.tick = tick;
                    graze_row.player_id = player.player_id;
                    graze_row.event_value = 1;
                    PushCardCastEvent(std::move(graze_row));
                }
            }
        }
        if (removed) {
            bullet_it = bullets_.erase(bullet_it);
        } else {
            ++bullet_it;
        }
    }
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
