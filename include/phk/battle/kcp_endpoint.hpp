#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace phk::battle {

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

}  // namespace phk::battle
