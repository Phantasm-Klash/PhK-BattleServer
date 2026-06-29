#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "phk/battle/protocol.hpp"

namespace phk::battle {

class BattleServer;

struct UdpDatagram {
    std::string remote_endpoint;
    std::vector<std::uint8_t> payload;
};

struct KcpEndpointStats {
    std::uint64_t datagrams_in = 0;
    std::uint64_t datagrams_out = 0;
    std::uint64_t bytes_in = 0;
    std::uint64_t bytes_out = 0;
};

class KcpEndpoint {
public:
    virtual ~KcpEndpoint() = default;
    virtual std::vector<UdpDatagram> ProcessDatagram(const UdpDatagram& datagram) = 0;
    [[nodiscard]] virtual KcpEndpointStats Stats() const = 0;
};

class KcpEchoEndpoint final : public KcpEndpoint {
public:
    std::vector<UdpDatagram> ProcessDatagram(const UdpDatagram& datagram) override;
    [[nodiscard]] KcpEndpointStats Stats() const override;

private:
    KcpEndpointStats stats_;
};

struct KcpAeadAdapterResult {
    bool ok = false;
    std::string reason;
    DispatchResult dispatch;
    std::vector<UdpDatagram> replies;
};

class KcpAeadPacketAdapter final {
public:
    KcpAeadPacketAdapter(BattleServer& server, KcpEndpoint& endpoint);

    KcpAeadAdapterResult ProcessEncryptedDatagram(
        const BattleEncryptedPacket& packet,
        const UdpDatagram& datagram
    );

private:
    BattleServer& server_;
    KcpEndpoint& endpoint_;
};

}  // namespace phk::battle
