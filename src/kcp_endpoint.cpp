#include "phk/battle/kcp_endpoint.hpp"

#include "phk/battle/server.hpp"

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

KcpAeadPacketAdapter::KcpAeadPacketAdapter(BattleServer& server, KcpEndpoint& endpoint)
    : server_(server), endpoint_(endpoint) {}

KcpAeadAdapterResult KcpAeadPacketAdapter::ProcessEncryptedDatagram(
    const BattleEncryptedPacket& packet,
    const UdpDatagram& datagram
) {
    KcpAeadAdapterResult result;
    result.dispatch = server_.DispatchEncrypted(packet);
    result.reason = result.dispatch.reason;
    if (!result.dispatch.ok) {
        return result;
    }

    result.replies = endpoint_.ProcessDatagram(datagram);
    result.ok = true;
    result.reason = result.dispatch.response_kind;
    return result;
}

}  // namespace phk::battle
