#include "phk/battle/kcp_endpoint.hpp"

#include <algorithm>

namespace phk::battle {

std::vector<UdpDatagram> KcpEchoEndpoint::ProcessDatagram(const UdpDatagram& datagram) {
    stats_.datagrams_in += 1;
    stats_.bytes_in += datagram.payload.size();

    UdpDatagram response;
    response.remote_endpoint = datagram.remote_endpoint;
    const char prefix[] = {'k', 'c', 'p', '_', 'e', 'c', 'h', 'o', ':'};
    response.payload.assign(std::begin(prefix), std::end(prefix));
    response.payload.insert(response.payload.end(), datagram.payload.begin(), datagram.payload.end());

    stats_.datagrams_out += 1;
    stats_.bytes_out += response.payload.size();
    return {response};
}

KcpEndpointStats KcpEchoEndpoint::Stats() const {
    return stats_;
}

}  // namespace phk::battle
