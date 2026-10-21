#include "phk/battle/card_events_report.hpp"  // BattleCallbackHeaders (T6 签名)
#include "phk/battle/match_lifecycle.hpp"

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

namespace phk::battle {
namespace {

// ---- little-endian codec -------------------------------------------------

void PushU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void PushU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
    }
}

void PushU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
    }
}

void PushString(std::vector<std::uint8_t>& out, const std::string& value) {
    const std::uint16_t len = static_cast<std::uint16_t>(std::min<std::size_t>(value.size(), 0xffff));
    PushU16(out, len);
    out.insert(out.end(), value.begin(), value.begin() + len);
}

bool ReadU16(const std::vector<std::uint8_t>& data, std::size_t* cursor, std::uint16_t* out) {
    if (*cursor + 2 > data.size()) return false;
    *out = static_cast<std::uint16_t>(data[*cursor] | (data[*cursor + 1] << 8));
    *cursor += 2;
    return true;
}

bool ReadU32(const std::vector<std::uint8_t>& data, std::size_t* cursor, std::uint32_t* out) {
    if (*cursor + 4 > data.size()) return false;
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(data[*cursor + i]) << (8 * i);
    }
    *cursor += 4;
    *out = value;
    return true;
}

bool ReadU64(const std::vector<std::uint8_t>& data, std::size_t* cursor, std::uint64_t* out) {
    if (*cursor + 8 > data.size()) return false;
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data[*cursor + i]) << (8 * i);
    }
    *cursor += 8;
    *out = value;
    return true;
}

bool ReadString(const std::vector<std::uint8_t>& data, std::size_t* cursor, std::string* out) {
    std::uint16_t len = 0;
    if (!ReadU16(data, cursor, &len)) return false;
    if (*cursor + len > data.size()) return false;
    out->assign(data.begin() + *cursor, data.begin() + *cursor + len);
    *cursor += len;
    return true;
}

void PushI64(std::vector<std::uint8_t>& out, std::int64_t value) {
    PushU64(out, static_cast<std::uint64_t>(value));
}

bool ReadI64(const std::vector<std::uint8_t>& data, std::size_t* cursor, std::int64_t* out) {
    std::uint64_t raw = 0;
    if (!ReadU64(data, cursor, &raw)) return false;
    *out = static_cast<std::int64_t>(raw);
    return true;
}

// Doubles travel as decimal text: %.17g round-trips a double exactly and keeps
// the frame byte-identical across compilers/ABIs.
void PushDouble(std::vector<std::uint8_t>& out, double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    PushString(out, buffer);
}

bool ReadDouble(const std::vector<std::uint8_t>& data, std::size_t* cursor, double* out) {
    std::string text;
    if (!ReadString(data, cursor, &text)) return false;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) return false;
    *out = value;
    return true;
}

std::int64_t WallClockNowMs() {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string JsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (const char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(ch)));
                    out += buffer;
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

BossRaceConfig MakeBossRaceConfig(const MatchLifecycleConfig& config) {
    BossRaceConfig boss;
    boss.match_id = config.match_id;
    boss.mode_id = config.mode_id;
    boss.ruleset_version = config.ruleset_version;
    boss.match_seed = config.seed;
    boss.boss_max_hp = config.boss_max_hp;
    boss.tick_rate_hz = config.tick_rate_hz;
    boss.boss_id = config.boss_id;
    boss.loadouts = config.loadouts;
    // boss 目录优先：boss_id 命中目录时以目录 HP 为权威（与 Go/TS 三端一致）。
    // 未配置 boss（默认路径）时保持 config.boss_max_hp 不变，逐字节复现现状。
    if (const danmaku::BossCatalogEntry* entry = danmaku::FindBossCatalogEntry(config.boss_id);
        entry != nullptr) {
        boss.boss_max_hp = entry->boss_max_hp;
    }
    return boss;
}

// 合作模式配置（coop_boss）：HP 由 5000 × 玩家数在开局定，不吃 --boss-hp 覆盖。
CoopBossConfig MakeCoopBossConfig(const MatchLifecycleConfig& config) {
    CoopBossConfig coop;
    coop.match_id = config.match_id;
    coop.mode_id = config.mode_id;
    coop.ruleset_version = config.ruleset_version;
    coop.match_seed = static_cast<std::int64_t>(config.seed);
    coop.boss_max_hp = 0;
    coop.tick_rate_hz = config.tick_rate_hz == 0 ? kCoopBossTickRateHz : config.tick_rate_hz;
    coop.loadouts = config.loadouts;
    return coop;
}

const char* CoopStateName(CoopBossState state) {
    switch (state) {
        case CoopBossState::Waiting: return "waiting";
        case CoopBossState::Running: return "running";
        case CoopBossState::Finished: return "finished";
    }
    return "unknown";
}

// coop 快照负载：字节 12 + JSON（与 race 的 EncodeBossRaceSnapshotPayload 同风格）。
std::vector<std::uint8_t> EncodeCoopBossSnapshotPayload(const CoopBossSnapshot& snapshot) {
    std::string json;
    json.reserve(256 + snapshot.players.size() * 96 + snapshot.bullets.size() * 72);
    json += "{\"tick\":";
    json += std::to_string(snapshot.tick);
    json += ",\"state\":\"";
    json += CoopStateName(snapshot.state);
    json += "\",\"boss_hp\":";
    json += std::to_string(snapshot.boss_hp);
    json += ",\"boss_max_hp\":";
    json += std::to_string(snapshot.boss_max_hp);
    json += ",\"seat_count\":";
    json += std::to_string(snapshot.seat_count);
    json += ",\"shield_dir_mask\":";
    json += std::to_string(snapshot.shield_dir_mask);
    json += ",\"lock_dir\":";
    json += std::to_string(snapshot.lock_dir);
    json += ",\"lock_until_tick\":";
    json += std::to_string(snapshot.lock_until_tick);
    json += ",\"spiral_active\":";
    json += snapshot.spiral_active ? "true" : "false";
    json += ",\"players\":[";
    for (std::size_t i = 0; i < snapshot.players.size(); ++i) {
        const CoopBossPlayerSnapshot& player = snapshot.players[i];
        if (i != 0) json += ',';
        json += "{\"player_id\":\"";
        json += JsonEscape(player.player_id);
        json += "\",\"seat\":";
        json += std::to_string(player.seat);
        json += ",\"x_milli\":";
        json += std::to_string(player.x_milli);
        json += ",\"y_milli\":";
        json += std::to_string(player.y_milli);
        json += ",\"facing_millideg\":";
        json += std::to_string(player.facing_millideg);
        json += ",\"damage_dealt\":";
        json += std::to_string(player.damage_dealt);
        json += ",\"connected\":";
        json += player.connected ? "true" : "false";
        json += ",\"locked\":";
        json += player.locked ? "true" : "false";
        json += ",\"shielded\":";
        json += player.shielded ? "true" : "false";
        json += '}';
    }
    json += "],\"bullets\":[";
    for (std::size_t i = 0; i < snapshot.bullets.size(); ++i) {
        const CoopBossBullet& bullet = snapshot.bullets[i];
        if (i != 0) json += ',';
        json += "{\"bullet_id\":\"";
        json += JsonEscape(bullet.bullet_id);
        json += "\",\"owner_player_id\":\"";
        json += JsonEscape(bullet.owner_player_id);
        json += "\",\"x_milli\":";
        json += std::to_string(bullet.x_milli);
        json += ",\"y_milli\":";
        json += std::to_string(bullet.y_milli);
        json += ",\"vx_milli\":";
        json += std::to_string(bullet.vx_milli);
        json += ",\"vy_milli\":";
        json += std::to_string(bullet.vy_milli);
        json += ",\"radius_milli\":";
        json += std::to_string(bullet.radius_milli);
        json += ",\"kind\":";
        json += std::to_string(bullet.kind);
        json += ",\"pattern_id\":\"";
        json += JsonEscape(bullet.pattern_id);
        json += "\"}";
    }
    json += "],\"state_hash\":\"";
    json += JsonEscape(snapshot.state_hash);
    json += "\"}";

    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(BattlePayloadType::CoopSnapshot));
    out.insert(out.end(), json.begin(), json.end());
    return out;
}

// coop 结果：与 race 同信封（MatchResultPayload → BuildMatchResultJson），
// 差别是 winner_player_id 留空（合作共赢），boss_current_hp 记共享 Boss 余量。
MatchResultPayload BuildCoopMatchResultPayload(const CoopBossSimulation& simulation) {
    MatchResultPayload payload;
    payload.match_id = simulation.Config().match_id;
    payload.mode_id = simulation.Config().mode_id;
    payload.ruleset_version = simulation.Config().ruleset_version;
    payload.match_seed = static_cast<std::uint64_t>(simulation.Config().match_seed);
    payload.winner_player_id = "";
    payload.state_hash = simulation.CanonicalStateHash();
    const CoopBossSnapshot snapshot = simulation.Snapshot();
    payload.winner_tick = snapshot.tick;
    payload.players.reserve(snapshot.players.size());
    for (const CoopBossPlayerSnapshot& player : snapshot.players) {
        MatchPlayerResult entry;
        entry.player_id = player.player_id;
        entry.damage_dealt = player.damage_dealt;
        entry.boss_current_hp = snapshot.boss_hp;
        payload.players.push_back(std::move(entry));
    }
    return payload;
}

// coop 的 .dem 玩家段（loadout 取自房间下发的每玩家自机）。
std::vector<DemPlayer> BuildCoopDemPlayers(
    const CoopBossConfig& config,
    const std::vector<std::string>& player_ids,
    const std::map<std::string, std::string>& user_ids
) {
    std::vector<DemPlayer> players;
    players.reserve(player_ids.size());
    for (const std::string& player_id : player_ids) {
        DemPlayer player;
        player.player_id = player_id;
        const auto it = user_ids.find(player_id);
        if (it != user_ids.end()) {
            player.user_id = it->second;
        }
        for (const PlayerLoadoutSpec& spec : config.loadouts) {
            if (spec.player_id == player_id) {
                player.loadout.character_id = spec.character_id;
                player.loadout.shot_type = spec.shot_type;
                player.loadout.bomb_type = spec.bomb_type;
                break;
            }
        }
        players.push_back(std::move(player));
    }
    return players;
}

KcpServerEndpointConfig MakeEndpointConfig(const MatchLifecycleConfig& config) {
    KcpServerEndpointConfig endpoint;
    endpoint.port = config.port;
    return endpoint;
}

std::string StateName(BossRaceState state) {
    switch (state) {
        case BossRaceState::Waiting: return "waiting";
        case BossRaceState::Running: return "running";
        case BossRaceState::Finished: return "finished";
    }
    return "unknown";
}

bool SendAll(int fd, const char* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t n = ::send(fd, data + sent, size - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// wire codec
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> EncodeBattleInputPayload(const BattleInput& input) {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(BattlePayloadType::Input));
    PushU32(out, static_cast<std::uint32_t>(input.version.protocol_version));
    PushString(out, input.match_id);
    PushString(out, input.player_id);
    PushU64(out, input.tick);
    PushU64(out, input.seq);
    PushU32(out, input.direction_bits);
    std::uint8_t flags = 0;
    if (input.slow) flags |= 0x01u;
    if (input.shoot) flags |= 0x02u;
    if (input.bomb) flags |= 0x04u;
    out.push_back(flags);
    out.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(input.card_slot)));
    PushString(out, input.mode_action_id);
    return out;
}

namespace {

// Presence byte that opens the optional ticket block.
constexpr std::uint8_t kTicketBlockAbsent = 0;
constexpr std::uint8_t kTicketBlockPresent = 1;

// Reads the fixed input body that both codecs share; leaves `*cursor` just past
// `mode_action_id` (i.e. where the optional ticket block starts).
bool ReadBattleInputBody(
    const std::vector<std::uint8_t>& payload,
    std::size_t* cursor,
    BattleInput* out
) {
    std::uint32_t protocol_version = 0;
    if (!ReadU32(payload, cursor, &protocol_version)) return false;

    BattleInput input;
    input.version.protocol_version = static_cast<int>(protocol_version);
    if (!ReadString(payload, cursor, &input.match_id)) return false;
    if (!ReadString(payload, cursor, &input.player_id)) return false;
    if (!ReadU64(payload, cursor, &input.tick)) return false;
    if (!ReadU64(payload, cursor, &input.seq)) return false;
    if (!ReadU32(payload, cursor, &input.direction_bits)) return false;
    if (*cursor >= payload.size()) return false;
    const std::uint8_t flags = payload[(*cursor)++];
    input.slow = (flags & 0x01u) != 0;
    input.shoot = (flags & 0x02u) != 0;
    input.bomb = (flags & 0x04u) != 0;
    if (*cursor >= payload.size()) return false;
    input.card_slot = static_cast<std::int8_t>(payload[(*cursor)++]);
    if (!ReadString(payload, cursor, &input.mode_action_id)) return false;

    *out = std::move(input);
    return true;
}

void WriteTicketBlock(std::vector<std::uint8_t>& out, const SignedBattleTicket& signed_ticket) {
    const BattleTicket& ticket = signed_ticket.ticket;
    PushU32(out, static_cast<std::uint32_t>(ticket.version.protocol_version));
    PushString(out, ticket.version.business_api_version);
    PushString(out, ticket.version.battle_api_version);
    PushString(out, ticket.version.ruleset_version);
    PushString(out, ticket.ticket_id);
    PushString(out, ticket.match_id);
    PushString(out, ticket.user_id);
    PushString(out, ticket.player_id);
    PushString(out, ticket.mode_id);
    PushString(out, ticket.battle_server_id);
    PushString(out, ticket.endpoint);
    PushString(out, ticket.deck_snapshot_hash);
    PushString(out, ticket.ruleset_version);
    PushString(out, ticket.ticket_nonce_hex);
    PushI64(out, ticket.issued_at_ms);
    PushI64(out, ticket.expires_at_ms);
    PushString(out, ticket.business_session_id);
    PushU32(out, static_cast<std::uint32_t>(ticket.hand_card_ids.size()));
    for (const std::string& card_id : ticket.hand_card_ids) {
        PushString(out, card_id);
    }
    PushU32(out, static_cast<std::uint32_t>(ticket.deck_card_ids.size()));
    for (const std::string& card_id : ticket.deck_card_ids) {
        PushString(out, card_id);
    }
    PushDouble(out, ticket.initial_energy);
    PushString(out, ticket.server_seed_decimal);
    PushString(out, signed_ticket.signature_alg);
    PushString(out, signed_ticket.key_id);
    PushString(out, signed_ticket.signature_hex);
    PushString(out, signed_ticket.public_key_hex);
    out.push_back(signed_ticket.server_authoritative ? 1u : 0u);
}

bool ReadTicketBlock(
    const std::vector<std::uint8_t>& payload,
    std::size_t* cursor,
    SignedBattleTicket* out
) {
    BattleTicket& ticket = out->ticket;
    std::uint32_t protocol_version = 0;
    if (!ReadU32(payload, cursor, &protocol_version)) return false;
    ticket.version.protocol_version = static_cast<int>(protocol_version);
    if (!ReadString(payload, cursor, &ticket.version.business_api_version)) return false;
    if (!ReadString(payload, cursor, &ticket.version.battle_api_version)) return false;
    if (!ReadString(payload, cursor, &ticket.version.ruleset_version)) return false;
    if (!ReadString(payload, cursor, &ticket.ticket_id)) return false;
    if (!ReadString(payload, cursor, &ticket.match_id)) return false;
    if (!ReadString(payload, cursor, &ticket.user_id)) return false;
    if (!ReadString(payload, cursor, &ticket.player_id)) return false;
    if (!ReadString(payload, cursor, &ticket.mode_id)) return false;
    if (!ReadString(payload, cursor, &ticket.battle_server_id)) return false;
    if (!ReadString(payload, cursor, &ticket.endpoint)) return false;
    if (!ReadString(payload, cursor, &ticket.deck_snapshot_hash)) return false;
    if (!ReadString(payload, cursor, &ticket.ruleset_version)) return false;
    if (!ReadString(payload, cursor, &ticket.ticket_nonce_hex)) return false;
    if (!ReadI64(payload, cursor, &ticket.issued_at_ms)) return false;
    if (!ReadI64(payload, cursor, &ticket.expires_at_ms)) return false;
    if (!ReadString(payload, cursor, &ticket.business_session_id)) return false;
    std::uint32_t hand_count = 0;
    if (!ReadU32(payload, cursor, &hand_count)) return false;
    if (hand_count > 64u) return false;
    ticket.hand_card_ids.clear();
    for (std::uint32_t i = 0; i < hand_count; ++i) {
        std::string card_id;
        if (!ReadString(payload, cursor, &card_id)) return false;
        ticket.hand_card_ids.push_back(std::move(card_id));
    }
    std::uint32_t deck_count = 0;
    if (!ReadU32(payload, cursor, &deck_count)) return false;
    if (deck_count > 64u) return false;
    ticket.deck_card_ids.clear();
    for (std::uint32_t i = 0; i < deck_count; ++i) {
        std::string card_id;
        if (!ReadString(payload, cursor, &card_id)) return false;
        ticket.deck_card_ids.push_back(std::move(card_id));
    }
    if (!ReadDouble(payload, cursor, &ticket.initial_energy)) return false;
    if (!ReadString(payload, cursor, &ticket.server_seed_decimal)) return false;
    if (!ReadString(payload, cursor, &out->signature_alg)) return false;
    if (!ReadString(payload, cursor, &out->key_id)) return false;
    if (!ReadString(payload, cursor, &out->signature_hex)) return false;
    if (!ReadString(payload, cursor, &out->public_key_hex)) return false;
    if (*cursor >= payload.size()) return false;
    out->server_authoritative = payload[(*cursor)++] != 0;
    return true;
}

}  // namespace

bool DecodeBattleInputPayload(const std::vector<std::uint8_t>& payload, BattleInput* out) {
    if (out == nullptr || payload.empty()) return false;
    if (payload[0] != static_cast<std::uint8_t>(BattlePayloadType::Input)) return false;

    std::size_t cursor = 1;
    return ReadBattleInputBody(payload, &cursor, out);
}

std::vector<std::uint8_t> EncodeBattleInputWithTicketPayload(
    const BattleInput& input,
    const SignedBattleTicket& ticket
) {
    std::vector<std::uint8_t> out = EncodeBattleInputPayload(input);
    out.push_back(kTicketBlockPresent);
    WriteTicketBlock(out, ticket);
    return out;
}

bool DecodeBattleInputTicketPayload(const std::vector<std::uint8_t>& payload, SignedBattleTicket* out) {
    if (out == nullptr || payload.empty()) return false;
    if (payload[0] != static_cast<std::uint8_t>(BattlePayloadType::Input)) return false;

    std::size_t cursor = 1;
    BattleInput input;
    if (!ReadBattleInputBody(payload, &cursor, &input)) return false;
    if (cursor >= payload.size()) return false;  // legacy frame: no ticket block

    const std::uint8_t flag = payload[cursor++];
    if (flag == kTicketBlockAbsent) return false;
    if (flag != kTicketBlockPresent) return false;

    SignedBattleTicket decoded;
    if (!ReadTicketBlock(payload, &cursor, &decoded)) return false;
    *out = std::move(decoded);
    return true;
}

std::vector<std::uint8_t> EncodeBossRaceSnapshotPayload(const BossRaceSnapshot& snapshot) {
    std::string json;
    json.reserve(256 + snapshot.bullets.size() * 64);
    json += "{\"tick\":";
    json += std::to_string(snapshot.tick);
    json += ",\"state\":\"";
    json += StateName(snapshot.state);
    json += "\",\"winner_player_id\":\"";
    json += JsonEscape(snapshot.winner_player_id);
    json += "\",\"winner_tick\":";
    json += std::to_string(snapshot.winner_tick);
    json += ",\"state_hash\":\"";
    json += JsonEscape(snapshot.state_hash);
    json += "\",\"players\":[";
    for (std::size_t i = 0; i < snapshot.players.size(); ++i) {
        const BossRacePlayerSnapshot& player = snapshot.players[i];
        if (i != 0) json += ',';
        json += "{\"player_id\":\"";
        json += JsonEscape(player.player_id);
        json += "\",\"x_milli\":";
        json += std::to_string(player.x_milli);
        json += ",\"y_milli\":";
        json += std::to_string(player.y_milli);
        json += ",\"boss_current_hp\":";
        json += std::to_string(player.boss_current_hp);
        json += ",\"damage_dealt\":";
        json += std::to_string(player.damage_dealt);
        json += ",\"connected\":";
        json += player.connected ? "true" : "false";
        json += '}';
    }
    json += "],\"bullets\":[";
    for (std::size_t i = 0; i < snapshot.bullets.size(); ++i) {
        const BossRaceBullet& bullet = snapshot.bullets[i];
        if (i != 0) json += ',';
        json += "{\"bullet_id\":\"";
        json += JsonEscape(bullet.bullet_id);
        json += "\",\"owner_player_id\":\"";
        json += JsonEscape(bullet.owner_player_id);
        json += "\",\"x_milli\":";
        json += std::to_string(bullet.x_milli);
        json += ",\"y_milli\":";
        json += std::to_string(bullet.y_milli);
        json += ",\"vx_milli\":";
        json += std::to_string(bullet.vx_milli);
        json += ",\"vy_milli\":";
        json += std::to_string(bullet.vy_milli);
        json += ",\"radius_milli\":";
        json += std::to_string(bullet.radius_milli);
        json += ",\"bullet_kind\":";
        json += std::to_string(bullet.bullet_kind);
        json += ",\"motion_func\":";
        json += std::to_string(bullet.motion_func);
        json += ",\"pattern_id\":\"";
        json += JsonEscape(bullet.pattern_id);
        json += "\"}";
    }
    json += "]}";

    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(BattlePayloadType::Snapshot));
    out.insert(out.end(), json.begin(), json.end());
    return out;
}

// ---------------------------------------------------------------------------
// result payload + JSON
// ---------------------------------------------------------------------------
MatchResultPayload BuildMatchResultPayload(const BossRaceSimulation& simulation) {
    MatchResultPayload payload;
    payload.match_id = simulation.Config().match_id;
    payload.mode_id = simulation.Config().mode_id;
    payload.ruleset_version = simulation.Config().ruleset_version;
    payload.match_seed = simulation.Config().match_seed;
    payload.winner_player_id = simulation.WinnerPlayerId();
    payload.state_hash = simulation.CanonicalStateHash();

    const BossRaceSnapshot snapshot = simulation.Snapshot();
    payload.winner_tick = snapshot.winner_tick;
    payload.players.reserve(snapshot.players.size());
    for (const BossRacePlayerSnapshot& player : snapshot.players) {
        MatchPlayerResult entry;
        entry.player_id = player.player_id;
        entry.damage_dealt = player.damage_dealt;
        entry.boss_current_hp = player.boss_current_hp;
        payload.players.push_back(std::move(entry));
    }
    return payload;
}

std::string BuildMatchResultJson(const MatchResultPayload& payload) {
    std::string json;
    json.reserve(256);
    json += "{\"match_id\":\"";
    json += JsonEscape(payload.match_id);
    json += "\",\"mode_id\":\"";
    json += JsonEscape(payload.mode_id);
    json += "\",\"ruleset_version\":\"";
    json += JsonEscape(payload.ruleset_version);
    json += "\",\"match_seed\":";
    json += std::to_string(payload.match_seed);
    json += ",\"winner_player_id\":\"";
    json += JsonEscape(payload.winner_player_id);
    json += "\",\"winner_tick\":";
    json += std::to_string(payload.winner_tick);
    json += ",\"state_hash\":\"";
    json += JsonEscape(payload.state_hash);
    json += "\",\"players\":[";
    for (std::size_t i = 0; i < payload.players.size(); ++i) {
        const MatchPlayerResult& player = payload.players[i];
        if (i != 0) json += ',';
        json += "{\"player_id\":\"";
        json += JsonEscape(player.player_id);
        json += "\",\"damage_dealt\":";
        json += std::to_string(player.damage_dealt);
        json += ",\"boss_current_hp\":";
        json += std::to_string(player.boss_current_hp);
        json += '}';
    }
    json += "]}";
    return json;
}

// ---------------------------------------------------------------------------
// HTTP client
// ---------------------------------------------------------------------------
bool ParseHostPort(const std::string& endpoint, std::string* host, std::uint16_t* port) {
    if (host == nullptr || port == nullptr) return false;
    const auto colon = endpoint.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= endpoint.size()) return false;
    const std::string port_text = endpoint.substr(colon + 1);
    const long parsed = std::strtol(port_text.c_str(), nullptr, 10);
    if (parsed <= 0 || parsed > 65535) return false;
    *host = endpoint.substr(0, colon);
    *port = static_cast<std::uint16_t>(parsed);
    return true;
}

HttpPostResult HttpPostJson(
    const std::string& host,
    std::uint16_t port,
    const std::string& path,
    const std::string& json_body,
    int timeout_ms,
    const std::vector<std::pair<std::string, std::string>>& extra_headers
) {
    HttpPostResult result;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* resolved = nullptr;
    const std::string port_text = std::to_string(port);
    const int gai = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &resolved);
    if (gai != 0 || resolved == nullptr) {
        result.error = std::string("getaddrinfo_failed: ") + gai_strerror(gai);
        return result;
    }

    const int fd = ::socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
    if (fd < 0) {
        result.error = "socket_failed";
        ::freeaddrinfo(resolved);
        return result;
    }

    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (::connect(fd, resolved->ai_addr, resolved->ai_addrlen) != 0) {
        result.error = std::string("connect_failed: ") + std::strerror(errno);
        ::close(fd);
        ::freeaddrinfo(resolved);
        return result;
    }
    ::freeaddrinfo(resolved);

    std::string request;
    request += "POST " + path + " HTTP/1.1\r\n";
    request += "Host: " + host + ":" + port_text + "\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(json_body.size()) + "\r\n";
    for (const auto& header : extra_headers) {
        request += header.first + ": " + header.second + "\r\n";
    }
    request += "Connection: close\r\n";
    request += "\r\n";
    request += json_body;

    if (!SendAll(fd, request.data(), request.size())) {
        result.error = "send_failed";
        ::close(fd);
        return result;
    }

    std::string response;
    char buffer[4096];
    for (;;) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            response.append(buffer, static_cast<std::size_t>(n));
            continue;
        }
        break;
    }
    ::close(fd);

    if (response.empty()) {
        result.error = "empty_response";
        return result;
    }

    const auto status_end = response.find("\r\n");
    const std::string status_line = response.substr(0, status_end == std::string::npos ? response.size() : status_end);
    const auto first_space = status_line.find(' ');
    if (first_space != std::string::npos) {
        result.status_code = std::atoi(status_line.c_str() + first_space + 1);
    }
    const auto body_start = response.find("\r\n\r\n");
    if (body_start != std::string::npos) {
        result.body = response.substr(body_start + 4);
    }
    result.ok = result.status_code >= 200 && result.status_code < 300;
    if (!result.ok && result.error.empty()) {
        result.error = "http_status_" + std::to_string(result.status_code);
    }
    return result;
}

// ---------------------------------------------------------------------------
// MatchServer
// ---------------------------------------------------------------------------
const char* MatchTicketRequirementName(MatchTicketRequirement value) {
    switch (value) {
        case MatchTicketRequirement::Off: return "off";
        case MatchTicketRequirement::Warn: return "warn";
        case MatchTicketRequirement::On: return "on";
    }
    return "unknown";
}

MatchServer::MatchServer(MatchLifecycleConfig config)
    : config_(std::move(config)),
      simulation_(MakeBossRaceConfig(config_)),
      endpoint_(MakeEndpointConfig(config_)) {
    // coop_boss 分支（加法式）：race 路径不受影响。
    coop_mode_ = config_.coop_mode || IsCoopBossMode(config_.mode_id);
    if (coop_mode_) {
        coop_ = std::make_unique<CoopBossSimulation>(MakeCoopBossConfig(config_));
    }
    // .dem genesis 条目：链的第一条恒为 match_seed（tick 0，battle_derived）。
    dem_recorder_.RecordMatchSeed(config_.seed);
}

bool MatchServer::Start(std::string* error) {
    if (started_) return true;
    if (config_.match_id.empty()) {
        if (error != nullptr) *error = "match_id_required";
        return false;
    }
    if (config_.player_ids.empty()) {
        if (error != nullptr) *error = "players_required";
        return false;
    }

    if (coop_mode_) {
        // 座位 = sorted(player_ids)（契约 §2.2）：2 人上下相对，4 人四向。
        std::vector<std::string> seats = config_.player_ids;
        std::sort(seats.begin(), seats.end());
        for (std::size_t i = 0; i < seats.size(); ++i) {
            std::int32_t x = 0;
            std::int32_t y = 0;
            CoopSeatSpawnMilli(seats.size(), i, &x, &y);
            if (!coop_->AddPlayer(seats[i], x, y)) {
                if (error != nullptr) *error = "add_player_failed";
                return false;
            }
        }
    } else {
        for (std::size_t i = 0; i < config_.player_ids.size(); ++i) {
            const std::int32_t x = (i % 2 == 0) ? -20000 : 20000;
            if (!simulation_.AddPlayer(config_.player_ids[i], x, 60000)) {
                if (error != nullptr) *error = "add_player_failed";
                return false;
            }
        }
    }

    endpoint_.SetMessageCallback(
        [this](const KcpSessionInfo& session, const std::vector<std::uint8_t>& payload) {
            HandleSessionPayload(session, payload);
        });

    if (!endpoint_.Bind(error)) {
        return false;
    }
    started_ = true;
    return true;
}

void MatchServer::Stop() {
    endpoint_.Close();
    started_ = false;
}

std::uint16_t MatchServer::Port() const {
    return endpoint_.port();
}

const MatchLifecycleConfig& MatchServer::Config() const {
    return config_;
}

const BossRaceSimulation& MatchServer::Simulation() const {
    return simulation_;
}

BossRaceSimulation& MatchServer::MutableSimulation() {
    return simulation_;
}

std::size_t MatchServer::BoundPlayerCount() const {
    return player_by_conv_.size();
}

void MatchServer::SetResultSubmitter(ResultSubmitter submitter) {
    submitter_ = std::move(submitter);
}

void MatchServer::SetEventsSubmitter(EventsSubmitter submitter) {
    events_submitter_ = std::move(submitter);
}

void MatchServer::ProcessNetwork(int poll_timeout_ms) {
    if (!started_) return;
    endpoint_.Poll(poll_timeout_ms);
}

void MatchServer::AdvanceTick() {
    if (!started_) return;
    const bool finished = coop_mode_
        ? coop_->State() == CoopBossState::Finished
        : simulation_.State() == BossRaceState::Finished;
    if (finished) {
        // 终局强制 flush 一次：跳过冷却，把收集器里剩下的行全部送出。
        FlushEventsIfNeeded(true);
        SubmitResultIfNeeded();
        return;
    }
    if (coop_mode_) {
        const CoopBossSnapshot snapshot = coop_->Tick();
        BroadcastSnapshotPayload(EncodeCoopBossSnapshotPayload(snapshot));
    } else {
        const BossRaceSnapshot snapshot = simulation_.Tick();
        BroadcastSnapshotPayload(EncodeBossRaceSnapshotPayload(snapshot));
    }
    DetectAllPlayersDisconnected();
    FlushEventsIfNeeded(false);
    SubmitResultIfNeeded();
}

void MatchServer::RunOnce() {
    ProcessNetwork(0);
    AdvanceTick();
}

bool MatchServer::InjectPayload(std::uint32_t conv, const std::vector<std::uint8_t>& payload) {
    if (!started_) return false;
    KcpSessionInfo session;
    session.conv = conv;
    session.remote_endpoint = "inject:" + std::to_string(conv);
    HandleSessionPayload(session, payload);
    return true;
}

void MatchServer::HandleSessionPayload(
    const KcpSessionInfo& session,
    const std::vector<std::uint8_t>& payload
) {
    BattleInput input;
    if (!DecodeBattleInputPayload(payload, &input)) {
        return;
    }

    SignedBattleTicket ticket;
    const bool has_ticket = DecodeBattleInputTicketPayload(payload, &ticket);

    const auto bound = player_by_conv_.find(session.conv);
    if (bound == player_by_conv_.end()) {
        if (!BindSession(session, input, ticket, has_ticket)) {
            return;
        }
        if (has_ticket) {
            // header players[].user_id 来源：已验证 ticket 的 user_id。
            dem_recorder_.NoteUserId(input.player_id, ticket.ticket.user_id);
        }
    } else if (bound->second != input.player_id) {
        RecordTicketAudit(session.conv, input.player_id, "", "player_conv_mismatch", true);
        return;
    }

    const bool input_accepted = coop_mode_ ? coop_->SubmitInput(input) : simulation_.SubmitInput(input);
    if (input_accepted) {
        accepted_inputs_ += 1;
        // F3：记录最近活动 tick，全断线判定（DetectAllPlayersDisconnected）用。
        last_input_tick_[input.player_id] =
            coop_mode_ ? coop_->CurrentTick() : simulation_.CurrentTick();
        // 链序 = 接受序：只在 SubmitInput 验证通过后入链。
        dem_recorder_.RecordInput(input);
    }
}

bool MatchServer::BindSession(
    const KcpSessionInfo& session,
    const BattleInput& input,
    const SignedBattleTicket& ticket,
    bool has_ticket
) {
    const MatchTicketRequirement requirement = config_.require_ticket;
    const bool enforcing = requirement == MatchTicketRequirement::On;
    // Warn mode keeps the legacy (insecure) binding as a fallback so live
    // matches are never interrupted while the lobby wiring is being rolled out.
    const bool fallback = requirement != MatchTicketRequirement::On;
    const std::string& ticket_id = has_ticket ? ticket.ticket.ticket_id : std::string();

    if (requirement == MatchTicketRequirement::Off) {
        return BindPlayerByRoster(session, input);
    }

    if (!has_ticket) {
        RecordTicketAudit(session.conv, input.player_id, "", "ticket_missing", enforcing);
        return fallback && BindPlayerByRoster(session, input);
    }

    TicketVerificationOptions options;
    options.now_ms = config_.now_ms > 0 ? config_.now_ms : WallClockNowMs();
    options.required_battle_server_id = config_.battle_server_id;
    options.required_key_id = config_.ticket_key_id;
    const VerificationResult verification = ticket_verifier_.Verify(ticket, options);
    if (!verification.ok) {
        RecordTicketAudit(session.conv, input.player_id, ticket_id, verification.reason, enforcing);
        return fallback && BindPlayerByRoster(session, input);
    }

    // The app process serves exactly one match, so a valid ticket for *another*
    // match must never bind here.
    if (ticket.ticket.match_id != config_.match_id) {
        RecordTicketAudit(session.conv, input.player_id, ticket_id, "ticket_match_mismatch", enforcing);
        return fallback && BindPlayerByRoster(session, input);
    }
    if (ticket.ticket.player_id != input.player_id) {
        RecordTicketAudit(session.conv, input.player_id, ticket_id, "ticket_player_mismatch", enforcing);
        return fallback && BindPlayerByRoster(session, input);
    }
    if (consumed_ticket_ids_.find(ticket_id) != consumed_ticket_ids_.end()) {
        RecordTicketAudit(session.conv, input.player_id, ticket_id, "ticket_replay", enforcing);
        return fallback && BindPlayerByRoster(session, input);
    }

    if (!BindPlayerByRoster(session, input)) {
        return false;
    }
    consumed_ticket_ids_.insert(ticket_id);
    RecordTicketAudit(session.conv, input.player_id, ticket_id, "ok", false);
    return true;
}

bool MatchServer::BindPlayerByRoster(const KcpSessionInfo& session, const BattleInput& input) {
    const bool known = std::find(
        config_.player_ids.begin(), config_.player_ids.end(), input.player_id) != config_.player_ids.end();
    if (!known) {
        // Never drop silently again: an unknown player id used to vanish here,
        // which is why "no snapshots" was so hard to diagnose.
        RecordTicketAudit(session.conv, input.player_id, "", "player_not_in_roster", true);
        return false;
    }
    player_by_conv_[session.conv] = input.player_id;
    conv_by_player_[input.player_id] = session.conv;
    return true;
}

void MatchServer::RecordTicketAudit(
    std::uint32_t conv,
    const std::string& player_id,
    const std::string& ticket_id,
    const std::string& reason,
    bool rejected
) {
    if (rejected) {
        ++ticket_rejections_;
    }
    if (ticket_audit_.size() < kMatchTicketAuditLimit) {
        MatchTicketAudit entry;
        entry.conv = conv;
        entry.player_id = player_id;
        entry.ticket_id = ticket_id;
        entry.reason = reason;
        entry.rejected = rejected;
        ticket_audit_.push_back(std::move(entry));
    }
    if (ticket_audit_logged_ < kMatchTicketLogLimit) {
        ++ticket_audit_logged_;
        std::fprintf(
            stderr,
            "[match-ticket] match=%s conv=%u player=%s ticket=%s action=%s reason=%s\n",
            config_.match_id.c_str(),
            conv,
            player_id.c_str(),
            ticket_id.c_str(),
            rejected ? "reject" : "accept",
            reason.c_str());
    } else if (ticket_audit_logged_ == kMatchTicketLogLimit) {
        ++ticket_audit_logged_;
        std::fprintf(
            stderr,
            "[match-ticket] match=%s audit_log_suppressed rejections=%zu\n",
            config_.match_id.c_str(),
            ticket_rejections_);
    }
}

void MatchServer::BroadcastSnapshotPayload(const std::vector<std::uint8_t>& payload) {
    if (player_by_conv_.empty()) return;
    for (const auto& entry : player_by_conv_) {
        endpoint_.Send(entry.first, payload);
    }
    snapshot_broadcasts_ += 1;
}

void MatchServer::DetectAllPlayersDisconnected() {
    if (all_players_disconnected_emitted_) return;
    // 从未有玩家活动（空场/ Waiting）：不判定，避免空闲进程误发事件。
    if (last_input_tick_.empty()) return;

    const std::uint64_t now = coop_mode_ ? coop_->CurrentTick() : simulation_.CurrentTick();
    const std::uint64_t tick_rate = config_.tick_rate_hz == 0
        ? static_cast<std::uint64_t>(kBossRaceTickRateHz)
        : static_cast<std::uint64_t>(config_.tick_rate_hz);
    const std::uint64_t silence_ticks = kPlayerDisconnectSilenceSeconds * tick_rate;

    // 座位 = 名册（config.player_ids）；名册为空时退化为「活动过的玩家集合」。
    std::vector<std::string> seats;
    if (!config_.player_ids.empty()) {
        seats = config_.player_ids;
    } else {
        seats.reserve(last_input_tick_.size());
        for (const auto& entry : last_input_tick_) {
            seats.push_back(entry.first);
        }
    }
    if (seats.empty()) return;

    bool all_disconnected = true;
    for (const std::string& seat : seats) {
        const auto it = last_input_tick_.find(seat);
        if (it == last_input_tick_.end()) {
            continue;  // 从未活动 = 掉线
        }
        if (now - it->second < silence_ticks) {
            all_disconnected = false;
            break;
        }
    }
    if (!all_disconnected) return;

    all_players_disconnected_emitted_ = true;  // 只推一次，避免刷屏。
    CardCastEvent row;
    row.event_type = kAllPlayersDisconnectedEventType;
    row.tick = static_cast<std::int64_t>(now);
    // 对局级事件：不带 user_id / card_id（大厅 isMatchLevelBattleEvent 约束）。
    if (coop_mode_) {
        coop_->PushEvent(std::move(row));
    } else {
        simulation_.PushEvent(std::move(row));
    }
}

void MatchServer::FlushEventsIfNeeded(bool force) {
    const std::uint64_t now = coop_mode_ ? coop_->CurrentTick() : simulation_.CurrentTick();
    if (!force && now < next_events_flush_tick_) return;
    // 无回传目标（无 test submitter 且未配置大厅）→ no-op，收集器原样保留。
    if (!events_submitter_ && config_.lobby_endpoint.empty()) return;

    const std::vector<CardCastEvent> rows = coop_mode_
        ? coop_->TakeEvents(kMaxBattleEventsPerSubmit)
        : simulation_.TakeEvents(kMaxBattleEventsPerSubmit);
    if (rows.empty()) {
        if (force) return;
        next_events_flush_tick_ = now + kEventsFlushCooldownTicks;
        return;
    }

    // battle_server_id 为空时用 "battle-local" 兜底（大厅校验非空）。
    const std::string server_id = config_.battle_server_id.empty()
        ? std::string("battle-local")
        : config_.battle_server_id;
    const std::string json = BuildBattleEventsJson(server_id, config_.match_id, rows);

    HttpPostResult result;
    if (events_submitter_) {
        result = events_submitter_(json);
    } else {
        std::string host;
        std::uint16_t port = 0;
        if (ParseHostPort(config_.lobby_endpoint, &host, &port)) {
            // T6 三头（公开两头 + ts/nonce/签名）与 submit-result 同一守卫。
            result = HttpPostJson(
                host,
                port,
                config_.events_path,
                json,
                3000,
                BattleCallbackHeaders(json, config_.callback_secret));
        } else {
            result.ok = false;
            result.error = "bad_lobby_endpoint";
        }
    }

    if (!result.ok) {
        // 失败放回队首不丢，并延长冷却（退避）。
        if (coop_mode_) {
            coop_->RestoreEvents(rows);
        } else {
            simulation_.RestoreEvents(rows);
        }
        next_events_flush_tick_ = now + kEventsFlushCooldownTicks * kEventsFlushFailureBackoff;
        return;
    }
    next_events_flush_tick_ = now + kEventsFlushCooldownTicks;
}

void MatchServer::SubmitResultIfNeeded() {
    const bool finished = coop_mode_
        ? coop_->State() == CoopBossState::Finished
        : simulation_.State() == BossRaceState::Finished;
    if (result_submitted_ || !finished) {
        return;
    }
    const std::string json = ResultJson();
    if (!config_.submit_result) {
        // 结算权在 battle-agent 的签名 RPC（battle.result.submit）：这里只
        // 保留结果内容供 stdout/本地读取，绝不 POST /internal/battle/result。
        // 两条路径同时结算会让结果取决于谁先到（先到者写 replay_ 前缀的
        // 原始 hash，后到者 409），因此这里必须一个字节都不发。
        last_submission_.ok = true;
        last_submission_.status_code = 0;
        last_submission_.body = json;
        std::fprintf(
            stderr,
            "[result] submit disabled (--no-result-submit): signed RPC is the settlement authority match=%s\n",
            config_.match_id.c_str());
    } else if (submitter_) {
        last_submission_ = submitter_(json);
    } else if (!config_.lobby_endpoint.empty()) {
        std::string host;
        std::uint16_t port = 0;
        if (ParseHostPort(config_.lobby_endpoint, &host, &port)) {
            // T6：公开两头 + 对精确 body 的 HMAC 签名（callback_secret 为空时
            // 只发公开两头，大厅 403，错配立即可见）。
            last_submission_ = HttpPostJson(
                host,
                port,
                config_.result_path,
                json,
                3000,
                BattleCallbackHeaders(json, config_.callback_secret));
        } else {
            last_submission_.ok = false;
            last_submission_.error = "bad_lobby_endpoint";
        }
    } else {
        last_submission_.ok = true;
        last_submission_.status_code = 0;
        last_submission_.body = json;
    }
    result_submitted_ = true;
    // 结算完成后录制 .dem（本地落盘 + 上传；失败不阻塞对局，见
    // WriteAndUploadDem）。关闭 result POST 时这条路径仍然执行：agent 只
    // 负责结算，回放容器依旧由对局服上传。
    WriteAndUploadDem();
}

std::size_t MatchServer::DemEntryCount() const {
    return dem_recorder_.Size();
}

DemHeader MatchServer::BuildDemHeader() const {
    if (coop_mode_) {
        const CoopBossConfig& cfg = coop_->Config();
        DemHeader header;
        header.match_id = cfg.match_id;
        header.mode_id = cfg.mode_id;
        header.boss_id = "";
        header.ruleset_version = cfg.ruleset_version;
        header.server_seed = std::to_string(cfg.match_seed);
        header.tick_rate_hz = cfg.tick_rate_hz;
        header.final_tick = static_cast<std::int64_t>(coop_->CurrentTick());
        header.final_state_hash = coop_->CanonicalStateHash();
        header.boss_max_hp = cfg.boss_max_hp;
        header.players = BuildCoopDemPlayers(cfg, config_.player_ids, dem_recorder_.UserIds());
        header.created_at = static_cast<std::int64_t>(WallClockNowMs() / 1000);
        header.entry_count = dem_recorder_.Size();
        header.chain_head_digest = dem_recorder_.Chain().HeadDigest();
        return header;
    }
    const BossRaceConfig& cfg = simulation_.Config();
    DemHeader header;
    header.match_id = cfg.match_id;
    header.mode_id = cfg.mode_id;
    header.boss_id = DemBossIdFromConfig(cfg);
    header.ruleset_version = cfg.ruleset_version;
    // 十进制字符串（对齐大厅 loadout server_seed 口径 / C++ match_seed）。
    header.server_seed = std::to_string(cfg.match_seed);
    header.tick_rate_hz = cfg.tick_rate_hz;
    header.final_tick = static_cast<std::int64_t>(simulation_.CurrentTick());
    header.final_state_hash = simulation_.CanonicalStateHash();
    // boss 满血值进 state hash，重放必须拿到同一个值（--boss-hp 才有意义）。
    header.boss_max_hp = cfg.boss_max_hp;
    header.players = BuildDemPlayers(cfg, config_.player_ids, dem_recorder_.UserIds());
    header.created_at = static_cast<std::int64_t>(WallClockNowMs() / 1000);
    header.entry_count = dem_recorder_.Size();
    header.chain_head_digest = dem_recorder_.Chain().HeadDigest();
    return header;
}

DemWriteResult MatchServer::FinalizeDem() const {
    return dem_recorder_.Finalize(BuildDemHeader());
}

void MatchServer::WriteAndUploadDem() {
    if (dem_written_) return;
    dem_written_ = true;
    const DemWriteResult written = FinalizeDem();
    if (!written.ok) {
        std::fprintf(stderr, "dem finalize failed: %s\n", written.reason.c_str());
        return;
    }
    const std::string path = DemLocalPath(config_.match_id);
    if (!WriteDemFile(path, written.bytes)) {
        // 上传失败时本地文件是回退校验来源，写失败必须可见。
        std::fprintf(stderr, "dem write failed: %s\n", path.c_str());
    }
    if (!DemUploadEnabled()) {
        return;
    }
    std::string host;
    std::uint16_t port = 0;
    if (config_.lobby_endpoint.empty() || !ParseHostPort(config_.lobby_endpoint, &host, &port)) {
        std::fprintf(stderr, "dem upload skipped: no usable lobby endpoint\n");
        return;
    }
    // replay_id 优先取结果提交响应里大厅分配的 id；取不到回落到 match_id
    // （大厅 dem 存储以 match_id 为键，下载路由两者都接受）。
    const std::string replay_id =
        DemExtractJsonStringField(last_submission_.body, "replay_id");
    DemUploadOptions options;
    options.host = host;
    options.port = port;
    options.match_id = config_.match_id;
    options.replay_id = replay_id.empty() ? config_.match_id : replay_id;
    options.battle_server_id = config_.battle_server_id;
    options.callback_secret = config_.callback_secret;
    const DemUploadResult upload = UploadDemBytes(options, written.bytes);
    if (!upload.ok) {
        std::fprintf(stderr, "dem upload failed: %s\n", upload.reason.c_str());
    }
}

bool MatchServer::finished() const {
    if (coop_mode_) {
        return coop_->State() == CoopBossState::Finished;
    }
    return simulation_.State() == BossRaceState::Finished;
}

bool MatchServer::ResultSubmitted() const {
    return result_submitted_;
}

const HttpPostResult& MatchServer::LastSubmission() const {
    return last_submission_;
}

MatchResultPayload MatchServer::ResultPayload() const {
    if (coop_mode_) {
        return BuildCoopMatchResultPayload(*coop_);
    }
    return BuildMatchResultPayload(simulation_);
}

std::string MatchServer::ResultJson() const {
    return BuildMatchResultJson(ResultPayload());
}

std::uint64_t MatchServer::SnapshotBroadcastCount() const {
    return snapshot_broadcasts_;
}

std::uint64_t MatchServer::AcceptedInputCount() const {
    return accepted_inputs_;
}

const std::vector<MatchTicketAudit>& MatchServer::TicketAuditLog() const {
    return ticket_audit_;
}

std::size_t MatchServer::TicketRejectionCount() const {
    return ticket_rejections_;
}

}  // namespace phk::battle
