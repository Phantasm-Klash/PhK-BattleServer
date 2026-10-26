// phk_battle_server - per-match battle server process.
//
// Lifecycle:
//   1. parse CLI args (match id, seed, players, lobby endpoint, port),
//   2. bind a UDP port and speak KCP with the two racing clients,
//   3. advance the BossRaceSimulation at a fixed 64 Hz tick rate,
//   4. on completion POST the result to the lobby and exit,
//   5. exit cleanly on SIGTERM / SIGINT (the lobby kills the process).

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "phk/battle/match_lifecycle.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void HandleSignal(int) {
    g_stop = 1;
}

std::vector<std::string> SplitComma(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    std::istringstream stream(text);
    while (std::getline(stream, current, ',')) {
        if (!current.empty()) {
            parts.push_back(current);
        }
    }
    return parts;
}

// 冒号分隔，**保留空字段**（自机未选时 shot/bomb 为空串）。
std::vector<std::string> SplitColon(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    std::istringstream stream(text);
    while (std::getline(stream, current, ':')) {
        parts.push_back(current);
    }
    // std::getline 不返回尾随空字段；按需补一个。
    if (!text.empty() && text.back() == ':') {
        parts.push_back("");
    }
    return parts;
}

// --loadout <p:char:shot:bomb>[,...] → PlayerLoadoutSpec 列表。
// 每条必须 4 段且 player_id 非空；格式不符的条目忽略（battle-agent 已保证格式）。
std::vector<phk::battle::PlayerLoadoutSpec> ParseLoadouts(const std::string& text) {
    std::vector<phk::battle::PlayerLoadoutSpec> out;
    for (const std::string& entry : SplitComma(text)) {
        const std::vector<std::string> fields = SplitColon(entry);
        if (fields.size() != 4 || fields[0].empty()) {
            continue;
        }
        phk::battle::PlayerLoadoutSpec loadout;
        loadout.player_id = fields[0];
        loadout.character_id = fields[1];
        loadout.shot_type = fields[2];
        loadout.bomb_type = fields[3];
        out.push_back(std::move(loadout));
    }
    return out;
}

bool ParseRequireTicket(const std::string& value, phk::battle::MatchTicketRequirement* out) {
    if (value == "off") {
        *out = phk::battle::MatchTicketRequirement::Off;
        return true;
    }
    if (value == "warn") {
        *out = phk::battle::MatchTicketRequirement::Warn;
        return true;
    }
    if (value == "on") {
        *out = phk::battle::MatchTicketRequirement::On;
        return true;
    }
    std::cerr << "invalid --require-ticket value: " << value << " (expected off|warn|on)\n";
    return false;
}

void PrintUsage() {
    std::cout
        << "usage: phk_battle_server [options]\n"
        << "  --port <P>            UDP port to bind (0 = ephemeral)\n"
        << "  --match-id <M>        match identifier\n"
        << "  --seed <S>            deterministic match seed\n"
        << "  --players <a,b>       comma separated player ids\n"
        << "  --lobby <host:port>   lobby endpoint for result POST\n"
        << "  --ruleset <id>        ruleset version (default mvp-boss-race-s0)\n"
        << "  --mode <id>           match mode (default mvp_boss_race; coop_boss = 合作模式)\n"
        << "  --boss-hp <N>         boss max hp override\n"
        << "  --boss-id <id>        room boss selection (tian|hui; empty = default timeline)\n"
        << "  --loadout <specs>     per-player loadout: p:char:shot:bomb[,...]\n"
        << "  --max-ticks <N>       safety tick cap (0 = unlimited)\n"
        << "  --tick-rate <Hz>      server tick rate (64 default; 60/128 accepted for tooling)\n"
        << "  --tick-scale <f>      test pacing: scales the wall-clock tick interval\n"
        << "                        (1 = real time, 0 = run ticks as fast as the CPU allows)\n"
        << "  --callback-secret <hex>  battle callback HMAC secret (registration response)\n"
        << "  --events-path <path>  battle events POST path (default /v1/battle/events)\n"
        << "  --require-ticket=<M>  signed ticket enforcement: off|warn|on (default warn)\n"
        << "  --battle-server-id <id>  pin tickets to this battle server id\n"
        << "  --ticket-key-id <id>  pin tickets to this lobby signing key id\n"
        << "  --no-result-submit    do not POST the result; the battle-agent's\n"
        << "                        signed RPC settles the match (default: submit)\n"
        << "  --help                show this help\n";
}

bool ParseArgs(
    int argc,
    char** argv,
    phk::battle::MatchLifecycleConfig* config,
    double* tick_scale
) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](std::string* out) -> bool {
            if (i + 1 >= argc) return false;
            *out = argv[++i];
            return true;
        };
        std::string value;
        if (arg == "--port") {
            if (!next(&value)) return false;
            config->port = static_cast<std::uint16_t>(std::atoi(value.c_str()));
        } else if (arg == "--match-id") {
            if (!next(&value)) return false;
            config->match_id = value;
        } else if (arg == "--seed") {
            if (!next(&value)) return false;
            config->seed = std::strtoull(value.c_str(), nullptr, 10);
        } else if (arg == "--players") {
            if (!next(&value)) return false;
            config->player_ids = SplitComma(value);
        } else if (arg == "--lobby") {
            if (!next(&value)) return false;
            config->lobby_endpoint = value;
        } else if (arg == "--ruleset") {
            if (!next(&value)) return false;
            config->ruleset_version = value;
        } else if (arg == "--mode") {
            if (!next(&value)) return false;
            config->mode_id = value;
            config->coop_mode = phk::battle::IsCoopBossMode(value);
        } else if (arg == "--boss-hp") {
            if (!next(&value)) return false;
            config->boss_max_hp = std::strtoull(value.c_str(), nullptr, 10);
        } else if (arg == "--boss-id") {
            if (!next(&value)) return false;
            config->boss_id = value;
        } else if (arg == "--loadout") {
            if (!next(&value)) return false;
            config->loadouts = ParseLoadouts(value);
        } else if (arg == "--max-ticks") {
            if (!next(&value)) return false;
            config->max_ticks = std::strtoull(value.c_str(), nullptr, 10);
        } else if (arg == "--tick-rate") {
            if (!next(&value)) return false;
            const long hz = std::strtol(value.c_str(), nullptr, 10);
            if (hz < 0 || hz > 1000) {
                std::cerr << "invalid --tick-rate: " << value << "\n";
                return false;
            }
            config->tick_rate_hz = static_cast<std::uint32_t>(hz);
        } else if (arg == "--tick-scale") {
            if (!next(&value)) return false;
            const double scale = std::strtod(value.c_str(), nullptr);
            if (scale < 0.0) {
                std::cerr << "invalid --tick-scale: " << value << "\n";
                return false;
            }
            *tick_scale = scale;
        } else if (arg == "--battle-server-id") {
            if (!next(&value)) return false;
            config->battle_server_id = value;
        } else if (arg == "--ticket-key-id") {
            if (!next(&value)) return false;
            config->ticket_key_id = value;
        } else if (arg == "--session-key-id") {
            if (!next(&value)) return false;
            // P2：本对局会话密钥的 id。服务端用它挡掉密钥轮换期到达的旧包。
            config->session_key_id = value;
        } else if (arg == "--session-key") {
            if (!next(&value)) return false;
            // P2：player_id=<64 hex>，可重复出现 —— 每个玩家每局一把。
            const auto eq = value.find('=');
            if (eq == std::string::npos || eq == 0) {
                std::cerr << "invalid --session-key (want player_id=<64 hex>): " << value << "\n";
                return false;
            }
            const std::string player_id = value.substr(0, eq);
            const std::string key_hex = value.substr(eq + 1);
            if (key_hex.size() != 64) {
                std::cerr << "invalid --session-key (want 64 hex chars): " << value << "\n";
                return false;
            }
            std::vector<std::uint8_t> key;
            for (std::size_t i = 0; i + 1 < key_hex.size(); i += 2) {
                key.push_back(
                    static_cast<std::uint8_t>(std::stoul(key_hex.substr(i, 2), nullptr, 16)));
            }
            config->session_keys_by_player[player_id] = key;
        } else if (arg == "--callback-secret") {
            if (!next(&value)) return false;
            // T6：结果回传 HMAC 密钥（注册响应的 callback_secret）。
            config->callback_secret = value;
        } else if (arg == "--events-path") {
            if (!next(&value)) return false;
            // F3：事件回传路径（默认 /v1/battle/events）。
            config->events_path = value;
        } else if (arg == "--require-ticket") {
            if (!next(&value)) return false;
            if (!ParseRequireTicket(value, &config->require_ticket)) return false;
        } else if (arg.rfind("--require-ticket=", 0) == 0) {
            if (!ParseRequireTicket(arg.substr(std::strlen("--require-ticket=")), &config->require_ticket)) {
                return false;
            }
        } else if (arg == "--no-result-submit") {
            // 结算权交给 battle-agent 的签名 RPC；本进程只录 .dem。
            config->submit_result = false;
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage();
            std::exit(0);
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    phk::battle::MatchLifecycleConfig config;
    config.match_id = "match_local_000001";
    config.player_ids = {"player_a", "player_b"};
    // Wall-clock pacing only: 1.0 keeps the real tick rate, anything lower runs
    // the same simulation faster, which is what automated runs need.
    double tick_scale = 1.0;

    if (!ParseArgs(argc, argv, &config, &tick_scale)) {
        PrintUsage();
        return 2;
    }

    std::signal(SIGTERM, HandleSignal);
    std::signal(SIGINT, HandleSignal);

    phk::battle::MatchServer server(config);
    std::string error;
    if (!server.Start(&error)) {
        std::cerr << "ERROR failed_to_start reason=" << error << "\n";
        return 1;
    }

    const std::uint16_t port = server.Port();
    std::cout << "READY port=" << port << " match=" << config.match_id
              << " require_ticket=" << phk::battle::MatchTicketRequirementName(config.require_ticket)
              << std::endl;

    // 64 tick 基准（计划 §2.1）：默认值跟 kBattleTickRateHz 走，别在这里写死数字。
    const std::uint32_t tick_rate = server.Config().tick_rate_hz == 0
        ? phk::battle::kBossRaceTickRateHz
        : server.Config().tick_rate_hz;
    const auto tick_interval = std::chrono::microseconds(1000000 / tick_rate);
    // The simulation is per-tick, so pacing is wall-clock only: --tick-scale
    // shrinks the interval (0 = no pacing at all) and automated runs can walk a
    // full match in a fraction of the time without changing a single bullet.
    const auto paced_interval = std::chrono::microseconds(
        static_cast<std::int64_t>(tick_interval.count() * tick_scale));
    auto next_tick = std::chrono::steady_clock::now();

    std::uint64_t ticks = 0;
    while (g_stop == 0) {
        server.ProcessNetwork(0);
        server.AdvanceTick();
        ++ticks;

        if (server.finished()) {
            std::cout << "RESULT " << server.ResultJson() << std::endl;
            const auto& submission = server.LastSubmission();
            std::cout << "SUBMIT ok=" << (submission.ok ? 1 : 0)
                      << " status=" << submission.status_code;
            if (!submission.ok && !submission.error.empty()) {
                std::cout << " error=" << submission.error;
            }
            std::cout << std::endl;
            server.Stop();
            return 0;
        }

        if (config.max_ticks != 0 && ticks >= config.max_ticks) {
            std::cout << "TIMEOUT ticks=" << ticks << std::endl;
            // A match that runs out of ticks still produced a result, and the
            // agent only submits what it can read from a RESULT line. Without
            // this the process exits with nothing to submit, the lobby never
            // settles the match, no replay is written and no rewards are
            // applied -- the only trace is "exited without a result line" in the
            // agent log. ResultJson() is built from the live simulation, so it is
            // meaningful mid-match; the winner is whoever leads, and nobody
            // leading (an empty winner) settles as a draw on the lobby side.
            std::cout << "RESULT " << server.ResultJson() << std::endl;
            server.Stop();
            return 0;
        }

        if (paced_interval.count() > 0) {
            next_tick += paced_interval;
            std::this_thread::sleep_until(next_tick);
        }
    }

    server.Stop();
    std::cout << "SHUTDOWN ticks=" << ticks << std::endl;
    return 0;
}
