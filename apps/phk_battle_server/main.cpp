#include <iostream>

#include "phk/battle/server.hpp"

int main(int argc, char** argv) {
    phk::battle::BattleServerConfig config;
    if (argc > 1) {
        config.server_id = argv[1];
    }
    phk::battle::BattleServer server(config);
    std::cout << "phk_battle_server skeleton\n"
              << "server_id=" << server.Config().server_id << "\n"
              << "endpoint=" << server.Config().endpoint << "\n"
              << "build_id=" << server.Config().build_id << "\n"
              << "active_sessions=" << server.ActiveSessionCount() << "\n";
    return 0;
}
