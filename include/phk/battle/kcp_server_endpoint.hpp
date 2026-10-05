#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "phk/battle/kcp_endpoint.hpp"

// Real KCP/UDP transport for the per-match battle server.
//
// The endpoint owns a UDP socket and one KCP control block per conversation
// id (conv). Incoming datagrams are demultiplexed by conv, fed into KCP, and
// complete application messages are handed to the registered callback.
//
// This is intentionally additive: the legacy KcpEchoEndpoint in
// kcp_endpoint.hpp is left untouched and keeps compiling.
namespace phk::battle {

struct KcpServerEndpointConfig {
    std::uint16_t port = 0;  // 0 = ephemeral
    int mtu = 1400;
    int snd_wnd = 128;
    int rcv_wnd = 128;
    bool no_delay = true;
    int interval_ms = 10;
    int fast_resend = 2;
    bool no_congestion = true;
    std::size_t max_sessions = 8;
};

struct KcpSessionInfo {
    std::uint32_t conv = 0;
    std::string remote_endpoint;  // "ip:port"
    std::uint64_t datagrams_in = 0;
    std::uint64_t datagrams_out = 0;
    std::uint64_t messages_in = 0;
    std::uint64_t messages_out = 0;
};

class KcpServerEndpoint {
public:
    using MessageCallback = std::function<void(
        const KcpSessionInfo& session,
        const std::vector<std::uint8_t>& payload
    )>;

    explicit KcpServerEndpoint(KcpServerEndpointConfig config = {});
    ~KcpServerEndpoint();

    KcpServerEndpoint(const KcpServerEndpoint&) = delete;
    KcpServerEndpoint& operator=(const KcpServerEndpoint&) = delete;

    bool Bind(std::string* error = nullptr);
    void Close();

    [[nodiscard]] bool bound() const;
    [[nodiscard]] std::uint16_t port() const;

    void SetMessageCallback(MessageCallback callback);

    // Receives pending datagrams (up to timeout_ms), feeds them into KCP,
    // advances every session (update + flush) and dispatches complete
    // application messages. Safe to call every tick; pass 0 for a non-blocking
    // poll. Returns the number of datagrams processed.
    int Poll(int timeout_ms);

    // Advances every session's KCP state without touching the socket.
    void Update();

    // Queues and flushes an application payload on the given session.
    bool Send(std::uint32_t conv, const std::vector<std::uint8_t>& payload);

    // Creates a session for a client that has not sent anything yet (server
    // initiated). Returns the allocated conv, or 0 on failure.
    std::uint32_t OpenSession(const std::string& remote_endpoint);

    [[nodiscard]] std::vector<KcpSessionInfo> Sessions() const;
    [[nodiscard]] std::size_t SessionCount() const;
    [[nodiscard]] KcpEndpointStats Stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace phk::battle
