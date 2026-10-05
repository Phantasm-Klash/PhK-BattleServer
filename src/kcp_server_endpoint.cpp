#include "phk/battle/kcp_server_endpoint.hpp"

#include "ikcp.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <utility>

namespace phk::battle {
namespace {

std::string SockaddrToKey(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {0};
    if (::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip)) == nullptr) {
        return {};
    }
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

bool KeyToSockaddr(const std::string& key, sockaddr_in* out) {
    if (out == nullptr) {
        return false;
    }
    const auto colon = key.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= key.size()) {
        return false;
    }
    const std::string host = key.substr(0, colon);
    const std::string port_text = key.substr(colon + 1);
    std::memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    if (::inet_pton(AF_INET, host.c_str(), &out->sin_addr) != 1) {
        return false;
    }
    const int port = std::atoi(port_text.c_str());
    if (port <= 0 || port > 65535) {
        return false;
    }
    out->sin_port = htons(static_cast<std::uint16_t>(port));
    return true;
}

}  // namespace

struct KcpServerEndpoint::Impl {
    struct Session {
        Impl* owner = nullptr;
        std::uint32_t conv = 0;
        ikcpcb* kcp = nullptr;
        sockaddr_in addr{};
        std::string remote_key;
        std::uint64_t datagrams_in = 0;
        std::uint64_t datagrams_out = 0;
        std::uint64_t messages_in = 0;
        std::uint64_t messages_out = 0;
    };

    KcpServerEndpointConfig config;
    int fd = -1;
    std::uint16_t bound_port = 0;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::map<std::uint32_t, std::unique_ptr<Session>> sessions_by_conv;
    std::map<std::string, std::uint32_t> conv_by_remote;
    std::uint32_t next_server_conv = 0x80000001u;
    KcpEndpointStats stats;
    MessageCallback callback;

    std::uint32_t NowMs() const {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        return static_cast<std::uint32_t>(elapsed.count());
    }

    static int OutputCallback(const char* buf, int len, ikcpcb* kcp, void* user) {
        (void)kcp;
        Session* session = static_cast<Session*>(user);
        if (session == nullptr || session->owner == nullptr) {
            return 0;
        }
        Impl* impl = session->owner;
        if (impl->fd < 0 || len <= 0) {
            return 0;
        }
        const ssize_t sent = ::sendto(
            impl->fd,
            buf,
            static_cast<std::size_t>(len),
            0,
            reinterpret_cast<const sockaddr*>(&session->addr),
            sizeof(session->addr));
        if (sent > 0) {
            session->datagrams_out += 1;
            impl->stats.datagrams_out += 1;
            impl->stats.bytes_out += static_cast<std::uint64_t>(sent);
        }
        return 0;
    }

    Session* CreateSession(std::uint32_t conv, const sockaddr_in& addr, const std::string& key) {
        if (sessions_by_conv.size() >= config.max_sessions) {
            return nullptr;
        }
        if (sessions_by_conv.find(conv) != sessions_by_conv.end()) {
            return sessions_by_conv[conv].get();
        }
        auto session = std::make_unique<Session>();
        session->owner = this;
        session->conv = conv;
        session->addr = addr;
        session->remote_key = key;
        session->kcp = ikcp_create(conv, session.get());
        if (session->kcp == nullptr) {
            return nullptr;
        }
        ikcp_setoutput(session->kcp, &Impl::OutputCallback);
        ikcp_nodelay(
            session->kcp,
            config.no_delay ? 1 : 0,
            config.interval_ms,
            config.fast_resend,
            config.no_congestion ? 1 : 0);
        ikcp_wndsize(session->kcp, config.snd_wnd, config.rcv_wnd);
        ikcp_setmtu(session->kcp, config.mtu);
        session->kcp->stream = 0;

        Session* raw = session.get();
        sessions_by_conv[conv] = std::move(session);
        conv_by_remote[key] = conv;
        return raw;
    }

    void DestroySession(std::uint32_t conv) {
        const auto it = sessions_by_conv.find(conv);
        if (it == sessions_by_conv.end()) {
            return;
        }
        Session* session = it->second.get();
        conv_by_remote.erase(session->remote_key);
        if (session->kcp != nullptr) {
            ikcp_release(session->kcp);
        }
        sessions_by_conv.erase(it);
    }

    void FlushSession(Session* session) {
        ikcp_update(session->kcp, NowMs());
        // Force an immediate flush so ACKs and queued payloads leave without
        // waiting for the next interval boundary.
        ikcp_flush(session->kcp);
    }

    void DispatchRecv(Session* session) {
        std::uint8_t buffer[65536];
        for (;;) {
            const IINT32 n = ikcp_recv(
                session->kcp,
                reinterpret_cast<char*>(buffer),
                static_cast<int>(sizeof(buffer)));
            if (n < 0) {
                break;  // -1 empty, -2 incomplete, -3 too small
            }
            session->messages_in += 1;
            if (callback) {
                KcpSessionInfo info;
                info.conv = session->conv;
                info.remote_endpoint = session->remote_key;
                info.datagrams_in = session->datagrams_in;
                info.datagrams_out = session->datagrams_out;
                info.messages_in = session->messages_in;
                info.messages_out = session->messages_out;
                std::vector<std::uint8_t> payload(buffer, buffer + n);
                callback(info, payload);
            }
        }
    }
};

KcpServerEndpoint::KcpServerEndpoint(KcpServerEndpointConfig config)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = config;
}

KcpServerEndpoint::~KcpServerEndpoint() {
    Close();
}

bool KcpServerEndpoint::Bind(std::string* error) {
    if (impl_->fd >= 0) {
        return true;
    }
    impl_->fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (impl_->fd < 0) {
        if (error != nullptr) {
            *error = std::string("socket() failed: ") + std::strerror(errno);
        }
        return false;
    }

    int reuse = 1;
    ::setsockopt(impl_->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(impl_->config.port);
    if (::bind(impl_->fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error != nullptr) {
            *error = std::string("bind() failed: ") + std::strerror(errno);
        }
        ::close(impl_->fd);
        impl_->fd = -1;
        return false;
    }

    sockaddr_in actual{};
    socklen_t actual_len = sizeof(actual);
    if (::getsockname(impl_->fd, reinterpret_cast<sockaddr*>(&actual), &actual_len) == 0) {
        impl_->bound_port = ntohs(actual.sin_port);
    }

    const int flags = ::fcntl(impl_->fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(impl_->fd, F_SETFL, flags | O_NONBLOCK);
    }

    impl_->start = std::chrono::steady_clock::now();
    return true;
}

void KcpServerEndpoint::Close() {
    for (auto& entry : impl_->sessions_by_conv) {
        if (entry.second->kcp != nullptr) {
            ikcp_release(entry.second->kcp);
        }
    }
    impl_->sessions_by_conv.clear();
    impl_->conv_by_remote.clear();
    if (impl_->fd >= 0) {
        ::close(impl_->fd);
        impl_->fd = -1;
    }
    impl_->bound_port = 0;
}

bool KcpServerEndpoint::bound() const {
    return impl_->fd >= 0;
}

std::uint16_t KcpServerEndpoint::port() const {
    return impl_->bound_port;
}

void KcpServerEndpoint::SetMessageCallback(MessageCallback callback) {
    impl_->callback = std::move(callback);
}

int KcpServerEndpoint::Poll(int timeout_ms) {
    if (impl_->fd < 0) {
        return 0;
    }

    int processed = 0;
    bool first = true;
    for (;;) {
        pollfd pfd{};
        pfd.fd = impl_->fd;
        pfd.events = POLLIN;
        const int rc = ::poll(&pfd, 1, first ? timeout_ms : 0);
        first = false;
        if (rc <= 0) {
            break;
        }
        if ((pfd.revents & POLLIN) == 0) {
            break;
        }

        std::uint8_t buffer[65536];
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        const ssize_t n = ::recvfrom(
            impl_->fd,
            buffer,
            sizeof(buffer),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &from_len);
        if (n <= 0) {
            break;
        }
        if (n < IKCP_OVERHEAD) {
            continue;
        }

        const std::uint32_t conv = ikcp_getconv(buffer);
        const std::string key = SockaddrToKey(from);
        auto it = impl_->sessions_by_conv.find(conv);
        Impl::Session* session = (it != impl_->sessions_by_conv.end())
            ? it->second.get()
            : impl_->CreateSession(conv, from, key);
        if (session == nullptr) {
            continue;
        }
        if (session->remote_key != key) {
            impl_->conv_by_remote.erase(session->remote_key);
            session->remote_key = key;
            session->addr = from;
            impl_->conv_by_remote[key] = conv;
        }

        session->datagrams_in += 1;
        impl_->stats.datagrams_in += 1;
        impl_->stats.bytes_in += static_cast<std::uint64_t>(n);
        ikcp_input(session->kcp, reinterpret_cast<const char*>(buffer), static_cast<long>(n));
        processed += 1;
    }

    for (auto& entry : impl_->sessions_by_conv) {
        Impl::Session* session = entry.second.get();
        impl_->FlushSession(session);
        impl_->DispatchRecv(session);
    }
    return processed;
}

void KcpServerEndpoint::Update() {
    if (impl_->fd < 0) {
        return;
    }
    for (auto& entry : impl_->sessions_by_conv) {
        impl_->FlushSession(entry.second.get());
        impl_->DispatchRecv(entry.second.get());
    }
}

bool KcpServerEndpoint::Send(std::uint32_t conv, const std::vector<std::uint8_t>& payload) {
    const auto it = impl_->sessions_by_conv.find(conv);
    if (it == impl_->sessions_by_conv.end() || payload.empty()) {
        return false;
    }
    Impl::Session* session = it->second.get();
    if (ikcp_send(
            session->kcp,
            reinterpret_cast<const char*>(payload.data()),
            static_cast<int>(payload.size())) != 0) {
        return false;
    }
    session->messages_out += 1;
    impl_->FlushSession(session);
    return true;
}

std::uint32_t KcpServerEndpoint::OpenSession(const std::string& remote_endpoint) {
    const auto existing = impl_->conv_by_remote.find(remote_endpoint);
    if (existing != impl_->conv_by_remote.end()) {
        return existing->second;
    }
    sockaddr_in addr{};
    if (!KeyToSockaddr(remote_endpoint, &addr)) {
        return 0;
    }
    const std::uint32_t conv = impl_->next_server_conv++;
    Impl::Session* session = impl_->CreateSession(conv, addr, remote_endpoint);
    return session != nullptr ? conv : 0;
}

std::vector<KcpSessionInfo> KcpServerEndpoint::Sessions() const {
    std::vector<KcpSessionInfo> result;
    result.reserve(impl_->sessions_by_conv.size());
    for (const auto& entry : impl_->sessions_by_conv) {
        const Impl::Session* session = entry.second.get();
        KcpSessionInfo info;
        info.conv = session->conv;
        info.remote_endpoint = session->remote_key;
        info.datagrams_in = session->datagrams_in;
        info.datagrams_out = session->datagrams_out;
        info.messages_in = session->messages_in;
        info.messages_out = session->messages_out;
        result.push_back(std::move(info));
    }
    return result;
}

std::size_t KcpServerEndpoint::SessionCount() const {
    return impl_->sessions_by_conv.size();
}

KcpEndpointStats KcpServerEndpoint::Stats() const {
    return impl_->stats;
}

}  // namespace phk::battle
