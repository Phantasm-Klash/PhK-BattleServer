#include "phk/battle/match_lifecycle.hpp"

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
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
    return boss;
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

bool DecodeBattleInputPayload(const std::vector<std::uint8_t>& payload, BattleInput* out) {
    if (out == nullptr || payload.empty()) return false;
    if (payload[0] != static_cast<std::uint8_t>(BattlePayloadType::Input)) return false;

    std::size_t cursor = 1;
    std::uint32_t protocol_version = 0;
    if (!ReadU32(payload, &cursor, &protocol_version)) return false;

    BattleInput input;
    input.version.protocol_version = static_cast<int>(protocol_version);
    if (!ReadString(payload, &cursor, &input.match_id)) return false;
    if (!ReadString(payload, &cursor, &input.player_id)) return false;
    if (!ReadU64(payload, &cursor, &input.tick)) return false;
    if (!ReadU64(payload, &cursor, &input.seq)) return false;
    if (!ReadU32(payload, &cursor, &input.direction_bits)) return false;
    if (cursor >= payload.size()) return false;
    const std::uint8_t flags = payload[cursor++];
    input.slow = (flags & 0x01u) != 0;
    input.shoot = (flags & 0x02u) != 0;
    input.bomb = (flags & 0x04u) != 0;
    if (cursor >= payload.size()) return false;
    input.card_slot = static_cast<std::int8_t>(payload[cursor++]);
    if (!ReadString(payload, &cursor, &input.mode_action_id)) return false;

    *out = std::move(input);
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
    int timeout_ms
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
MatchServer::MatchServer(MatchLifecycleConfig config)
    : config_(std::move(config)),
      simulation_(MakeBossRaceConfig(config_)),
      endpoint_(MakeEndpointConfig(config_)) {}

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

    for (std::size_t i = 0; i < config_.player_ids.size(); ++i) {
        const std::int32_t x = (i % 2 == 0) ? -20000 : 20000;
        if (!simulation_.AddPlayer(config_.player_ids[i], x, 60000)) {
            if (error != nullptr) *error = "add_player_failed";
            return false;
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

void MatchServer::ProcessNetwork(int poll_timeout_ms) {
    if (!started_) return;
    endpoint_.Poll(poll_timeout_ms);
}

void MatchServer::AdvanceTick() {
    if (!started_) return;
    if (simulation_.State() == BossRaceState::Finished) {
        SubmitResultIfNeeded();
        return;
    }
    const BossRaceSnapshot snapshot = simulation_.Tick();
    BroadcastSnapshot(snapshot);
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

    const auto bound = player_by_conv_.find(session.conv);
    if (bound == player_by_conv_.end()) {
        const bool known = std::find(
            config_.player_ids.begin(), config_.player_ids.end(), input.player_id) != config_.player_ids.end();
        if (!known) return;
        player_by_conv_[session.conv] = input.player_id;
        conv_by_player_[input.player_id] = session.conv;
    } else if (bound->second != input.player_id) {
        return;
    }

    if (simulation_.SubmitInput(input)) {
        accepted_inputs_ += 1;
    }
}

void MatchServer::BroadcastSnapshot(const BossRaceSnapshot& snapshot) {
    if (player_by_conv_.empty()) return;
    const std::vector<std::uint8_t> payload = EncodeBossRaceSnapshotPayload(snapshot);
    for (const auto& entry : player_by_conv_) {
        endpoint_.Send(entry.first, payload);
    }
    snapshot_broadcasts_ += 1;
}

void MatchServer::SubmitResultIfNeeded() {
    if (result_submitted_ || simulation_.State() != BossRaceState::Finished) {
        return;
    }
    const std::string json = ResultJson();
    if (submitter_) {
        last_submission_ = submitter_(json);
    } else if (!config_.lobby_endpoint.empty()) {
        std::string host;
        std::uint16_t port = 0;
        if (ParseHostPort(config_.lobby_endpoint, &host, &port)) {
            last_submission_ = HttpPostJson(host, port, config_.result_path, json);
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
}

bool MatchServer::finished() const {
    return simulation_.State() == BossRaceState::Finished;
}

bool MatchServer::ResultSubmitted() const {
    return result_submitted_;
}

const HttpPostResult& MatchServer::LastSubmission() const {
    return last_submission_;
}

MatchResultPayload MatchServer::ResultPayload() const {
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

}  // namespace phk::battle
