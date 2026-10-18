#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "phk/battle/boss_race.hpp"
#include "phk/battle/dem.hpp"
#include "phk/battle/kcp_server_endpoint.hpp"
#include "phk/battle/ticket.hpp"

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

// The signed battle ticket rides the input frame as an optional trailing block.
// A frame produced by a client that predates ticket support simply ends after
// `mode_action_id`, so `DecodeBattleInputTicketPayload` reports "no ticket"
// instead of failing — that is what lets `--require-ticket` be rolled out
// gradually (off -> warn -> on) without breaking older clients mid-match.
[[nodiscard]] std::vector<std::uint8_t> EncodeBattleInputWithTicketPayload(
    const BattleInput& input,
    const SignedBattleTicket& ticket
);
// Returns false when the frame carries no ticket block, or when the block is
// malformed. `*out` is only written when the function returns true.
[[nodiscard]] bool DecodeBattleInputTicketPayload(
    const std::vector<std::uint8_t>& payload,
    SignedBattleTicket* out
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

// 既有 submit-result 路径使用不带额外头的默认形态（向后兼容）；
// service-callback 路由（如 /v1/battle/events）经 extra_headers 传鉴权头
// （见 card_events_report.hpp 的 BattleEventsCallbackHeaders）。
[[nodiscard]] HttpPostResult HttpPostJson(
    const std::string& host,
    std::uint16_t port,
    const std::string& path,
    const std::string& json_body,
    int timeout_ms = 3000,
    const std::vector<std::pair<std::string, std::string>>& extra_headers = {}
);

// ---------------------------------------------------------------------------
// Match lifecycle runner
// ---------------------------------------------------------------------------
// How strictly the per-match (app path) process enforces the signed battle
// ticket before binding a KCP conversation to a player id.
//
//   Off  - legacy behaviour: bind on `player_id` membership alone.
//   Warn - verify and log, but still bind on verification failure. Use this to
//          observe real traffic before turning enforcement on.
//   On   - reject the binding (and the input) when the ticket is missing or
//          does not verify for this match/player.
//
// The default is Warn: battle-agent does not pass a ticket yet, so `On` would
// break every live match, while `Off` would keep the injection window silent.
enum class MatchTicketRequirement {
    Off = 0,
    Warn = 1,
    On = 2,
};

[[nodiscard]] const char* MatchTicketRequirementName(MatchTicketRequirement value);

// Diagnostics caps: a UDP flood must not turn the audit trail into an OOM or a
// full disk. Rejections are always counted in `TicketRejectionCount()`.
inline constexpr std::size_t kMatchTicketAuditLimit = 256;
inline constexpr std::size_t kMatchTicketLogLimit = 512;

// One entry per binding decision, so a rejected session is never silently
// dropped (the old `if (!known) return;` was invisible in the logs and made
// "snapshots=0" investigations take hours).
struct MatchTicketAudit {
    std::uint32_t conv = 0;
    std::string player_id;
    std::string ticket_id;
    std::string reason;
    bool rejected = false;
};

struct MatchLifecycleConfig {
    std::uint16_t port = 0;  // 0 = ephemeral
    std::string match_id;
    std::string mode_id = kMvpBossRaceModeId;
    std::uint64_t seed = 0;
    std::string ruleset_version = "mvp-boss-race-s0";
    std::vector<std::string> player_ids;
    std::string lobby_endpoint;  // "host:port"; empty disables submission
    std::string result_path = "/internal/battle/result";
    // 结算权归谁：true = 本进程 POST result_path（历史行为）；false = 只把
    // 结果写到 stdout/内存，由 battle-agent 的签名 RPC battle.result.submit
    // 结算（`--no-result-submit`）。默认 true，老调用方行为不变。
    bool submit_result = true;
    // T6：结果回传签名密钥（同 BattleServerConfig::callback_secret）。结果
    // body 以它为密钥签 X-PhK-Battle-Signature（大厅 HandleBattleResult 对
    // 精确原始 body 校验）；为空时大厅 403 battle_callback_signature_invalid。
    std::string callback_secret;
    std::uint64_t boss_max_hp = kBossRaceDefaultBossHp;
    std::uint32_t tick_rate_hz = kBossRaceTickRateHz;
    std::uint64_t max_ticks = 0;  // 0 = no cap (lobby kills the process)
    // 房间 boss 选择（仅房主可设；空 = 默认 timeline）+ 每玩家自机 loadout。
    std::string boss_id;
    std::vector<PlayerLoadoutSpec> loadouts;

    // --- signed ticket enforcement (app path) -----------------------------
    MatchTicketRequirement require_ticket = MatchTicketRequirement::Warn;
    // Optional pinning; empty means "do not check".
    std::string battle_server_id;
    std::string ticket_key_id;
    // Fixed clock for tests; 0 means "use the wall clock".
    std::int64_t now_ms = 0;
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

    // Binding decisions, oldest first. Capped at kMatchTicketAuditLimit
    // entries; `TicketRejectionCount()` counts every rejection regardless.
    [[nodiscard]] const std::vector<MatchTicketAudit>& TicketAuditLog() const;
    [[nodiscard]] std::size_t TicketRejectionCount() const;

    // --- .dem 录制（thread B / COORDINATION §0.1.1）--------------------------
    // 已入链条目数（含 genesis match_seed），诊断/单测用。
    [[nodiscard]] std::size_t DemEntryCount() const;
    // 按当前 config + simulation 生成 .dem 容器字节（不落盘、不上传）；单测用。
    [[nodiscard]] DemWriteResult FinalizeDem() const;

private:
    void HandleSessionPayload(const KcpSessionInfo& session, const std::vector<std::uint8_t>& payload);
    // Verifies (per `require_ticket`) and binds `session.conv` -> input.player_id.
    // Returns false when the session must not be bound.
    bool BindSession(
        const KcpSessionInfo& session,
        const BattleInput& input,
        const SignedBattleTicket& ticket,
        bool has_ticket
    );
    bool BindPlayerByRoster(const KcpSessionInfo& session, const BattleInput& input);
    void RecordTicketAudit(
        std::uint32_t conv,
        const std::string& player_id,
        const std::string& ticket_id,
        const std::string& reason,
        bool rejected
    );
    void BroadcastSnapshot(const BossRaceSnapshot& snapshot);
    void SubmitResultIfNeeded();
    // 结果提交后生成 .dem：Finalize → 本地落盘 → 上传（失败只记日志，保留本地文件）。
    void WriteAndUploadDem();
    [[nodiscard]] DemHeader BuildDemHeader() const;

    MatchLifecycleConfig config_;
    BossRaceSimulation simulation_;
    DemRecorder dem_recorder_;
    bool dem_written_ = false;
    KcpServerEndpoint endpoint_;
    TicketVerifier ticket_verifier_;
    std::map<std::uint32_t, std::string> player_by_conv_;
    std::map<std::string, std::uint32_t> conv_by_player_;
    // Anti-replay: the app process lives exactly as long as one match, so a
    // process-local set of consumed ticket ids is enough (no need to port the
    // BattleServer session table over).
    std::set<std::string> consumed_ticket_ids_;
    std::vector<MatchTicketAudit> ticket_audit_;
    std::size_t ticket_rejections_ = 0;
    std::size_t ticket_audit_logged_ = 0;
    ResultSubmitter submitter_;
    HttpPostResult last_submission_;
    bool result_submitted_ = false;
    std::uint64_t snapshot_broadcasts_ = 0;
    std::uint64_t accepted_inputs_ = 0;
    bool started_ = false;
};

}  // namespace phk::battle
