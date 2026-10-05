// Tests for the per-match battle server lifecycle:
//   * application wire codec round-trip,
//   * deterministic result payload / JSON construction,
//   * a real UDP + KCP socket smoke test that races two players to a winner,
//   * the hand-written HTTP/1.1 result submission client.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ikcp.h"
#include "phk/battle/match_lifecycle.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& label) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << label << "\n";
    }
}

std::uint32_t NowMs() {
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

phk::battle::BattleInput MakeInput(
    const std::string& match_id,
    const std::string& player_id,
    std::uint64_t tick,
    std::uint64_t seq,
    bool shoot
) {
    phk::battle::BattleInput input;
    input.match_id = match_id;
    input.player_id = player_id;
    input.tick = tick;
    input.seq = seq;
    input.shoot = shoot;
    input.slow = false;
    input.bomb = false;
    input.card_slot = -1;
    input.direction_bits = 0;
    return input;
}

// ---------------------------------------------------------------------------
// 1. wire codec
// ---------------------------------------------------------------------------
void TestInputCodecRoundTrip() {
    phk::battle::BattleInput input;
    input.match_id = "match_codec_1";
    input.player_id = "player_x";
    input.tick = 12345;
    input.seq = 678;
    input.direction_bits = 0x5;
    input.slow = true;
    input.shoot = true;
    input.bomb = false;
    input.card_slot = 3;
    input.mode_action_id = "action_dash";

    const auto payload = phk::battle::EncodeBattleInputPayload(input);
    Check(!payload.empty(), "input payload non-empty");
    Check(payload[0] == static_cast<std::uint8_t>(phk::battle::BattlePayloadType::Input), "input payload type byte");

    phk::battle::BattleInput decoded;
    Check(phk::battle::DecodeBattleInputPayload(payload, &decoded), "input payload decodes");
    Check(decoded.match_id == input.match_id, "codec match_id");
    Check(decoded.player_id == input.player_id, "codec player_id");
    Check(decoded.tick == input.tick, "codec tick");
    Check(decoded.seq == input.seq, "codec seq");
    Check(decoded.direction_bits == input.direction_bits, "codec direction_bits");
    Check(decoded.slow == input.slow && decoded.shoot == input.shoot && decoded.bomb == input.bomb, "codec flags");
    Check(decoded.card_slot == input.card_slot, "codec card_slot");
    Check(decoded.mode_action_id == input.mode_action_id, "codec mode_action_id");

    std::vector<std::uint8_t> garbage = {0x00, 0x01, 0x02};
    Check(!phk::battle::DecodeBattleInputPayload(garbage, &decoded), "codec rejects non-input frame");

    std::vector<std::uint8_t> truncated(payload.begin(), payload.begin() + 4);
    Check(!phk::battle::DecodeBattleInputPayload(truncated, &decoded), "codec rejects truncated frame");
}

// ---------------------------------------------------------------------------
// 2. deterministic result payload + JSON
// ---------------------------------------------------------------------------
phk::battle::MatchResultPayload RunRaceToResult(std::uint64_t seed, std::uint64_t boss_hp) {
    phk::battle::BossRaceConfig config;
    config.match_id = "match_result_000001";
    config.match_seed = seed;
    config.boss_max_hp = boss_hp;

    phk::battle::BossRaceSimulation sim(config);
    sim.AddPlayer("player_a", -20000, 60000);
    sim.AddPlayer("player_b", 20000, 60000);

    std::uint64_t seq_a = 0;
    std::uint64_t seq_b = 0;
    for (std::uint64_t tick = 1; tick <= 200 && sim.State() != phk::battle::BossRaceState::Finished; ++tick) {
        sim.SubmitInput(MakeInput(config.match_id, "player_a", tick, ++seq_a, true));
        sim.SubmitInput(MakeInput(config.match_id, "player_b", tick, ++seq_b, false));
        sim.Tick();
    }
    return phk::battle::BuildMatchResultPayload(sim);
}

void TestResultPayloadAndJson() {
    const auto payload = RunRaceToResult(20260626u, 100);
    Check(payload.match_id == "match_result_000001", "result match_id");
    Check(payload.winner_player_id == "player_a", "result winner");
    Check(payload.winner_tick == 10, "result winner tick (100hp / 10 per tick)");
    Check(!payload.state_hash.empty(), "result state hash present");
    Check(payload.players.size() == 2, "result has two players");
    Check(payload.players[0].player_id == "player_a", "result players sorted deterministically");
    Check(payload.players[0].damage_dealt == 100, "winner damage accounted");
    Check(payload.players[0].boss_current_hp == 0, "winner boss defeated");
    Check(payload.players[1].boss_current_hp == 100, "loser boss intact");

    const std::string json = phk::battle::BuildMatchResultJson(payload);
    Check(json.find("\"match_id\":\"match_result_000001\"") != std::string::npos, "json match_id field");
    Check(json.find("\"winner_player_id\":\"player_a\"") != std::string::npos, "json winner field");
    Check(json.find("\"winner_tick\":10") != std::string::npos, "json winner_tick field");
    Check(json.find("\"state_hash\":\"" + payload.state_hash + "\"") != std::string::npos, "json state_hash field");
    Check(json.find("\"damage_dealt\":100") != std::string::npos, "json damage field");
    Check(json.find("\"boss_current_hp\":0") != std::string::npos, "json boss hp field");
    Check(json.front() == '{' && json.back() == '}', "json is a single object");

    // Determinism: identical inputs must yield identical JSON, byte for byte.
    const auto payload_again = RunRaceToResult(20260626u, 100);
    const std::string json_again = phk::battle::BuildMatchResultJson(payload_again);
    Check(json == json_again, "result json is deterministic");
}

// ---------------------------------------------------------------------------
// 3. real UDP + KCP socket smoke test
// ---------------------------------------------------------------------------
class KcpClient {
public:
    ~KcpClient() {
        if (kcp_ != nullptr) ikcp_release(kcp_);
        if (fd_ >= 0) ::close(fd_);
    }

    bool Open(const std::string& host, std::uint16_t port, std::uint32_t conv) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return false;

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = 0;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) return false;

        const int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags >= 0) ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

        server_addr_.sin_family = AF_INET;
        server_addr_.sin_port = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &server_addr_.sin_addr) != 1) return false;

        kcp_ = ikcp_create(conv, this);
        if (kcp_ == nullptr) return false;
        ikcp_setoutput(kcp_, &KcpClient::OutputCallback);
        ikcp_nodelay(kcp_, 1, 10, 2, 1);
        ikcp_wndsize(kcp_, 128, 128);
        return true;
    }

    void Send(const std::vector<std::uint8_t>& payload) {
        ikcp_send(kcp_, reinterpret_cast<const char*>(payload.data()), static_cast<int>(payload.size()));
        ikcp_update(kcp_, NowMs());
        ikcp_flush(kcp_);
    }

    void Pump() {
        char datagram[65536];
        for (;;) {
            const ssize_t n = ::recv(fd_, datagram, sizeof(datagram), 0);
            if (n <= 0) break;
            ikcp_input(kcp_, datagram, static_cast<long>(n));
        }
        ikcp_update(kcp_, NowMs());
        ikcp_flush(kcp_);

        char message[65536];
        for (;;) {
            const int n = ikcp_recv(kcp_, message, static_cast<int>(sizeof(message)));
            if (n < 0) break;
            received_.emplace_back(message, message + n);
        }
    }

    std::size_t ReceivedCount() const { return received_.size(); }

private:
    static int OutputCallback(const char* buf, int len, ikcpcb* kcp, void* user) {
        (void)kcp;
        KcpClient* client = static_cast<KcpClient*>(user);
        ::sendto(
            client->fd_,
            buf,
            static_cast<std::size_t>(len),
            0,
            reinterpret_cast<const sockaddr*>(&client->server_addr_),
            sizeof(client->server_addr_));
        return 0;
    }

    int fd_ = -1;
    ikcpcb* kcp_ = nullptr;
    sockaddr_in server_addr_{};
    std::vector<std::vector<std::uint8_t>> received_;
};

void TestSocketSmokeRace() {
    phk::battle::MatchLifecycleConfig config;
    config.port = 0;
    config.match_id = "match_socket_smoke";
    config.seed = 987654321u;
    config.player_ids = {"player_a", "player_b"};
    config.boss_max_hp = 100;

    phk::battle::MatchServer server(config);
    std::string error;
    Check(server.Start(&error), "match server starts (" + error + ")");
    Check(server.Port() != 0, "server bound to an ephemeral port");

    std::string captured_body;
    server.SetResultSubmitter([&](const std::string& json) {
        captured_body = json;
        phk::battle::HttpPostResult result;
        result.ok = true;
        result.status_code = 200;
        result.body = "{}";
        return result;
    });

    KcpClient client_a;
    KcpClient client_b;
    Check(client_a.Open("127.0.0.1", server.Port(), 0x0000A001u), "client_a opens kcp session");
    Check(client_b.Open("127.0.0.1", server.Port(), 0x0000B002u), "client_b opens kcp session");

    std::uint64_t seq_a = 0;
    std::uint64_t seq_b = 0;
    for (std::uint64_t tick = 1; tick <= 400 && !server.finished(); ++tick) {
        client_a.Send(phk::battle::EncodeBattleInputPayload(
            MakeInput(config.match_id, "player_a", tick, ++seq_a, true)));
        client_b.Send(phk::battle::EncodeBattleInputPayload(
            MakeInput(config.match_id, "player_b", tick, ++seq_b, false)));

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        server.ProcessNetwork(0);
        server.AdvanceTick();

        client_a.Pump();
        client_b.Pump();
    }

    Check(server.finished(), "race finished over real sockets");
    Check(server.Simulation().WinnerPlayerId() == "player_a", "socket race winner is player_a");
    Check(server.AcceptedInputCount() > 0, "server accepted kcp inputs");
    Check(server.BoundPlayerCount() == 2, "both players bound to sessions");
    Check(server.SnapshotBroadcastCount() > 0, "snapshots were broadcast");
    Check(server.ResultSubmitted(), "result submitted on finish");
    Check(captured_body.find("\"winner_player_id\":\"player_a\"") != std::string::npos,
          "captured result body has winner");
    Check(captured_body.find("\"match_id\":\"match_socket_smoke\"") != std::string::npos,
          "captured result body has match id");

    server.Stop();
}

// ---------------------------------------------------------------------------
// 4. HTTP/1.1 POST client
// ---------------------------------------------------------------------------
class TinyHttpServer {
public:
    TinyHttpServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return;
        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        timeval tv{};
        tv.tv_sec = 5;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return;
        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        ::listen(listen_fd_, 4);
        thread_ = std::thread([this] { Serve(); });
    }

    ~TinyHttpServer() { Stop(); }

    std::uint16_t port() const { return port_; }

    std::string body() {
        std::lock_guard<std::mutex> lock(mutex_);
        return body_;
    }

    std::string request_line() {
        std::lock_guard<std::mutex> lock(mutex_);
        return request_line_;
    }

private:
    void Serve() {
        const int client = ::accept(listen_fd_, nullptr, nullptr);
        if (client < 0) return;

        std::string request;
        char buffer[4096];
        std::size_t header_end = std::string::npos;
        std::size_t content_length = 0;
        for (;;) {
            const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            request.append(buffer, static_cast<std::size_t>(n));

            if (header_end == std::string::npos) {
                header_end = request.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    const std::string headers = request.substr(0, header_end);
                    const std::string key = "Content-Length:";
                    const auto pos = headers.find(key);
                    if (pos != std::string::npos) {
                        content_length = static_cast<std::size_t>(
                            std::strtoul(headers.c_str() + pos + key.size(), nullptr, 10));
                    }
                }
            }
            if (header_end != std::string::npos &&
                request.size() - (header_end + 4) >= content_length) {
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto line_end = request.find("\r\n");
            request_line_ = request.substr(0, line_end);
            if (header_end != std::string::npos) {
                body_ = request.substr(header_end + 4);
            }
        }

        const char* response =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
        ::send(client, response, std::strlen(response), 0);
        ::close(client);
    }

    void Stop() {
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::string body_;
    std::string request_line_;
};

void TestHttpPostClient() {
    TinyHttpServer http;
    Check(http.port() != 0, "tiny http server listening");

    const std::string body = "{\"match_id\":\"m1\",\"winner_player_id\":\"player_a\"}";
    const auto result = phk::battle::HttpPostJson("127.0.0.1", http.port(), "/internal/battle/result", body);
    Check(result.ok, "http post succeeded (" + result.error + ")");
    Check(result.status_code == 200, "http status 200");
    Check(http.request_line() == "POST /internal/battle/result HTTP/1.1", "http request line");
    Check(http.body() == body, "http server received exact json body");

    std::string host;
    std::uint16_t port = 0;
    Check(phk::battle::ParseHostPort("127.0.0.1:8080", &host, &port), "parse host:port");
    Check(host == "127.0.0.1" && port == 8080, "parse host:port values");
    Check(!phk::battle::ParseHostPort("no-port", &host, &port), "reject endpoint without port");
}

}  // namespace

int main() {
    TestInputCodecRoundTrip();
    TestResultPayloadAndJson();
    TestSocketSmokeRace();
    TestHttpPostClient();

    if (g_failures != 0) {
        std::cerr << g_failures << " lifecycle test(s) failed\n";
        return 1;
    }
    std::cout << "battle lifecycle tests passed\n";
    return 0;
}
