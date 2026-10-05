// phk_battle_server - per-match battle server process.
//
// Lifecycle:
//   1. parse CLI args (match id, seed, players, lobby endpoint, port),
//   2. bind a UDP port and speak KCP with the two racing clients,
//   3. advance the BossRaceSimulation at a fixed 60 Hz tick rate,
//   4. on completion POST the result to the lobby and exit,
//   5. exit cleanly on SIGTERM / SIGINT (the lobby kills the process).

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
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

void PrintUsage() {
    std::cout
        << "usage: phk_battle_server [options]\n"
        << "  --port <P>            UDP port to bind (0 = ephemeral)\n"
        << "  --match-id <M>        match identifier\n"
        << "  --seed <S>            deterministic match seed\n"
        << "  --players <a,b>       comma separated player ids\n"
        << "  --lobby <host:port>   lobby endpoint for result POST\n"
        << "  --ruleset <id>        ruleset version (default mvp-boss-race-s0)\n"
        << "  --boss-hp <N>         boss max hp override\n"
        << "  --max-ticks <N>       safety tick cap (0 = unlimited)\n"
        << "  --help                show this help\n";
}

bool ParseArgs(int argc, char** argv, phk::battle::MatchLifecycleConfig* config) {
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
        } else if (arg == "--boss-hp") {
            if (!next(&value)) return false;
            config->boss_max_hp = std::strtoull(value.c_str(), nullptr, 10);
        } else if (arg == "--max-ticks") {
            if (!next(&value)) return false;
            config->max_ticks = std::strtoull(value.c_str(), nullptr, 10);
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

    if (!ParseArgs(argc, argv, &config)) {
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
    std::cout << "READY port=" << port << " match=" << config.match_id << std::endl;

    const std::uint32_t tick_rate = server.Config().tick_rate_hz == 0 ? 60 : server.Config().tick_rate_hz;
    const auto tick_interval = std::chrono::microseconds(1000000 / tick_rate);
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
            server.Stop();
            return 0;
        }

        next_tick += tick_interval;
        std::this_thread::sleep_until(next_tick);
    }

    server.Stop();
    std::cout << "SHUTDOWN ticks=" << ticks << std::endl;
    return 0;
}
