#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "phk/battle/boss_race.hpp"
#include "phk/battle/kcp_server_endpoint.hpp"

// Per-match process lifecycle for the C++ battle server.
//
// Responsibilities:
//   * own one BossRaceSimulation plus one KcpServerEndpoint,
//   * decode client input frames and feed the simulation at a fixed tick rate,
//   * broadcast snapshots back to connected sessions,
//   * on race completion, build the canonical result body and POST it to the
//     lobby over a hand-written HTTP/1.1 client (no libcurl dependency).
namespace phk::battle {

// ---------------------------------------------------------------------------
// Application wire framing (KCP payload = 1 type byte + body)
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<std::uint8_t> EncodeBattleInputPayload(const BattleInput& input);
[[nodiscard]] bool DecodeBattleInputPayload(
    const std::vector<std::uint8_t>& payload,
    BattleInput* out
);
[[nodiscard]] std::vector<std::uint8_t> EncodeBossRaceSnapshotPayload(const BossRaceSnapshot& snapshot);

// ---------------------------------------------------------------------------
// Result payload + deterministic JSON
// ---------------------------------------------------------------------------
struct MatchPlayerResult {
    std::string player_id;
    std::uint64_t damage_dealt = 0;
    std::uint64_t boss_current_hp = 0;
};

struct MatchResultPayload {
    std::string match_id;
    std::string mode_id = kMvpBossRaceModeId;
    std::string ruleset_version = "mvp-boss-race-s0";
    std::uint64_t match_seed = 0;
    std::string winner_player_id;
    std::uint64_t winner_tick = 0;
    std::string state_hash;
    std::vector<MatchPlayerResult> players;
};

[[nodiscard]] MatchResultPayload BuildMatchResultPayload(const BossRaceSimulation& simulation);
[[nodiscard]] std::string BuildMatchResultJson(const MatchResultPayload& payload);

// ---------------------------------------------------------------------------
// Minimal HTTP/1.1 JSON POST client (native TCP socket)
// ---------------------------------------------------------------------------
struct HttpPostResult {
    bool ok = false;
    int status_code = 0;
    std::string body;
    std::string error;
};

[[nodiscard]] bool ParseHostPort(
    const std::string& endpoint,
    std::string* host,
    std::uint16_t* port
);

[[nodiscard]] HttpPostResult HttpPostJson(
    const std::string& host,
    std::uint16_t port,
    const std::string& path,
    const std::string& json_body,
    int timeout_ms = 3000
);

// ---------------------------------------------------------------------------
// Match lifecycle runner
// ---------------------------------------------------------------------------
struct MatchLifecycleConfig {
    std::uint16_t port = 0;  // 0 = ephemeral
    std::string match_id;
    std::string mode_id = kMvpBossRaceModeId;
    std::uint64_t seed = 0;
    std::string ruleset_version = "mvp-boss-race-s0";
    std::vector<std::string> player_ids;
    std::string lobby_endpoint;  // "host:port"; empty disables submission
    std::string result_path = "/internal/battle/result";
    std::uint64_t boss_max_hp = kBossRaceDefaultBossHp;
    std::uint32_t tick_rate_hz = kBossRaceTickRateHz;
    std::uint64_t max_ticks = 0;  // 0 = no cap (lobby kills the process)
};

class MatchServer {
public:
    using ResultSubmitter = std::function<HttpPostResult(const std::string& json_body)>;

    explicit MatchServer(MatchLifecycleConfig config);

    bool Start(std::string* error = nullptr);
    void Stop();

    [[nodiscard]] std::uint16_t Port() const;
    [[nodiscard]] const MatchLifecycleConfig& Config() const;
    [[nodiscard]] const BossRaceSimulation& Simulation() const;
    [[nodiscard]] BossRaceSimulation& MutableSimulation();
    [[nodiscard]] std::size_t BoundPlayerCount() const;

    // Overrides the HTTP submission (used by tests).
    void SetResultSubmitter(ResultSubmitter submitter);

    // Processes pending datagrams and dispatches decoded inputs.
    void ProcessNetwork(int poll_timeout_ms);

    // Advances the simulation one fixed tick and broadcasts a snapshot.
    void AdvanceTick();

    // ProcessNetwork(0) followed by AdvanceTick().
    void RunOnce();

    // Feeds a raw KCP application payload as if it arrived on `conv`.
    // Returns false if the frame could not be decoded or bound.
    bool InjectPayload(std::uint32_t conv, const std::vector<std::uint8_t>& payload);

    [[nodiscard]] bool finished() const;
    [[nodiscard]] bool ResultSubmitted() const;
    [[nodiscard]] const HttpPostResult& LastSubmission() const;
    [[nodiscard]] MatchResultPayload ResultPayload() const;
    [[nodiscard]] std::string ResultJson() const;

    // Number of snapshot frames broadcast so far (diagnostics/tests).
    [[nodiscard]] std::uint64_t SnapshotBroadcastCount() const;
    [[nodiscard]] std::uint64_t AcceptedInputCount() const;

private:
    void HandleSessionPayload(const KcpSessionInfo& session, const std::vector<std::uint8_t>& payload);
    void BroadcastSnapshot(const BossRaceSnapshot& snapshot);
    void SubmitResultIfNeeded();

    MatchLifecycleConfig config_;
    BossRaceSimulation simulation_;
    KcpServerEndpoint endpoint_;
    std::map<std::uint32_t, std::string> player_by_conv_;
    std::map<std::string, std::uint32_t> conv_by_player_;
    ResultSubmitter submitter_;
    HttpPostResult last_submission_;
    bool result_submitted_ = false;
    std::uint64_t snapshot_broadcasts_ = 0;
    std::uint64_t accepted_inputs_ = 0;
    bool started_ = false;
};

}  // namespace phk::battle
