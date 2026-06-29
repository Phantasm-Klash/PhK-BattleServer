#include "phk/battle/simulation.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
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

bool BattleSimulation::AddPlayer(const std::string& player_id, std::int32_t x_milli, std::int32_t y_milli) {
    if (player_id.empty() || players_.find(player_id) != players_.end()) {
        return false;
    }

    PlayerState player;
    player.player_id = player_id;
    player.x_milli = ClampMilli(x_milli, -kArenaHalfWidthMilli, kArenaHalfWidthMilli);
    player.y_milli = ClampMilli(y_milli, -kArenaHalfHeightMilli, kArenaHalfHeightMilli);
    player.last_input.match_id = config_.match_id;
    player.last_input.player_id = player_id;
    players_[player_id] = player;
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
    const auto player_it = players_.find(input.player_id);
    if (player_it == players_.end()) {
        result.code = InputValidationCode::PlayerUnknown;
        result.reason = "player_unknown";
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
    const auto player_it = players_.find(action.player_id);
    if (player_it == players_.end()) {
        result.code = InputValidationCode::PlayerUnknown;
        result.reason = "player_unknown";
        return result;
    }
    if (!player_it->second.connected) {
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
    pending_mode_actions_by_tick_[action.tick].push_back(action);
    return result;
}

BattleSnapshot BattleSimulation::Tick() {
    const std::uint64_t tick_to_apply = current_tick_ + 1;
    const auto pending_it = pending_inputs_by_tick_.find(tick_to_apply);

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
                player.last_input = input;
            } else {
                AccumulateFallbackInput(player, input);
            }
        } else {
            AccumulateFallbackInput(player, input);
        }
        ApplyInput(player, input);
    }

    if (pending_it != pending_inputs_by_tick_.end()) {
        pending_inputs_by_tick_.erase(pending_it);
    }

    current_tick_ = tick_to_apply;
    ApplyModeActionsForTick(tick_to_apply);
    SpawnBulletsForTick();
    AdvanceBullets();
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
    snapshot.mode_state["bullet_count"] = std::to_string(bullets_.size());
    snapshot.mode_state["accepted_input_count"] = std::to_string(accepted_input_count_);
    snapshot.mode_state["fallback_input_count"] = std::to_string(fallback_input_count_);
    snapshot.mode_state["neutral_fallback_count"] = std::to_string(neutral_fallback_count_);
    snapshot.mode_state["held_input_fallback_count"] = std::to_string(held_input_fallback_count_);
    snapshot.mode_state["mode_action_count"] = std::to_string(mode_action_count_);
    std::size_t connected_player_count = 0;
    for (const auto& item : players_) {
        if (item.second.connected) {
            ++connected_player_count;
        }
    }
    snapshot.mode_state["connected_player_count"] = std::to_string(connected_player_count);
    snapshot.mode_state["disconnected_player_count"] = std::to_string(players_.size() - connected_player_count);
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
    summary.final_tick = current_tick_;
    summary.input_count = accepted_input_count_;
    summary.fallback_input_count = fallback_input_count_;
    summary.neutral_fallback_count = neutral_fallback_count_;
    summary.held_input_fallback_count = held_input_fallback_count_;
    summary.mode_action_count = mode_action_count_;
    summary.event_count = event_count_;
    if (has_last_mode_action_) {
        summary.last_mode_action_id = last_mode_action_.action_id;
        summary.last_mode_action_type = last_mode_action_.action_type;
        summary.last_mode_action_player_id = last_mode_action_.player_id;
        summary.last_mode_action_tick = last_mode_action_.tick;
        summary.last_mode_action_seq = last_mode_action_.seq;
    }
    return summary;
}

std::uint64_t BattleSimulation::MixSeed(std::uint64_t value) const {
    std::uint64_t hash = kFnvOffset;
    hash = HashAppend(hash, config_.match_seed);
    hash = HashAppend(hash, value);
    return hash;
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

BattleInput BattleSimulation::InputForTick(const PlayerState& player) const {
    BattleInput input = player.last_input;
    input.tick = current_tick_ + 1;
    return input;
}

void BattleSimulation::ApplyInput(PlayerState& player, const BattleInput& input) {
    constexpr std::uint32_t kUp = 1u << 0;
    constexpr std::uint32_t kDown = 1u << 1;
    constexpr std::uint32_t kLeft = 1u << 2;
    constexpr std::uint32_t kRight = 1u << 3;
    const std::int32_t axis_x = DirectionAxis((input.direction_bits & kLeft) != 0, (input.direction_bits & kRight) != 0);
    const std::int32_t axis_y = DirectionAxis((input.direction_bits & kUp) != 0, (input.direction_bits & kDown) != 0);
    const std::int32_t speed = input.slow ? 2500 : 5000;

    player.x_milli = ClampMilli(player.x_milli + axis_x * speed, -kArenaHalfWidthMilli, kArenaHalfWidthMilli);
    player.y_milli = ClampMilli(player.y_milli + axis_y * speed, -kArenaHalfHeightMilli, kArenaHalfHeightMilli);
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
        AccumulateAcceptedModeAction(action);
    }
    pending_mode_actions_by_tick_.erase(actions_it);
}

void BattleSimulation::SpawnBulletsForTick() {
    if (current_tick_ == 0 || (current_tick_ % config_.spawn_period_ticks) != 0 || bullets_.size() >= config_.max_bullets) {
        return;
    }

    const std::uint64_t mixed = MixSeed(current_tick_);
    const std::int32_t drift = static_cast<std::int32_t>(mixed % 7000u) - 3500;
    const std::array<std::pair<std::int32_t, std::int32_t>, 4> velocities = {{
        {0, 3000},
        {3000, 0},
        {0, -3000},
        {-3000, 0},
    }};

    for (std::size_t i = 0; i < velocities.size() && bullets_.size() < config_.max_bullets; ++i) {
        BulletState bullet;
        bullet.bullet_id = "b" + std::to_string(next_bullet_id_++);
        bullet.x_milli = i % 2 == 0 ? drift : 0;
        bullet.y_milli = i % 2 == 0 ? 0 : drift;
        bullet.vx_milli = velocities[i].first;
        bullet.vy_milli = velocities[i].second;
        bullet.radius_milli = 4000;
        bullet.pattern_id = "basic_radial";
        bullet.color = (i % 2 == 0) ? "red" : "blue";
        bullets_.push_back(bullet);
    }

    event_stream_hash_ = HashAppend(event_stream_hash_, current_tick_);
    event_stream_hash_ = HashAppend(event_stream_hash_, next_bullet_id_);
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
    ++fallback_input_count_;
    if (input.seq == 0 && input.direction_bits == 0 && !input.slow && !input.shoot && !input.bomb &&
        input.card_slot == -1) {
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
    ++mode_action_count_;
    ++event_count_;
}

void BattleSimulation::AccumulateConnectionEvent(const PlayerState& player) {
    event_stream_hash_ = HashAppend(event_stream_hash_, config_.match_id);
    event_stream_hash_ = HashAppend(event_stream_hash_, player.player_id);
    event_stream_hash_ = HashAppend(event_stream_hash_, current_tick_);
    event_stream_hash_ = HashAppend(event_stream_hash_, player.connected ? "connected" : "disconnected");
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
    }
    return "unknown";
}

}  // namespace phk::battle
