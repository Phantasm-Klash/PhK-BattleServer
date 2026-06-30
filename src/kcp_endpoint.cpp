#include "phk/battle/kcp_endpoint.hpp"

#include "phk/battle/server.hpp"

#include <algorithm>

namespace phk::battle {

namespace {

std::string SessionRemoteKey(const BattleEncryptedPacket& packet) {
    if (packet.header.match_id.empty() || packet.header.player_id.empty() || packet.header.key_id.empty()) {
        return {};
    }
    return packet.header.match_id + ":" + packet.header.player_id + ":" + packet.header.key_id;
}

}  // namespace

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
    if (datagram.remote_endpoint.empty()) {
        stats_.rejected_datagrams += 1;
        stats_.malformed_datagrams += 1;
        result.reason = "remote_endpoint_missing";
        result.dispatch.payload_type = packet.header.payload_type;
        result.dispatch.reason = result.reason;
        return result;
    }
    if (datagram.payload.empty()) {
        stats_.rejected_datagrams += 1;
        stats_.malformed_datagrams += 1;
        result.reason = "datagram_payload_missing";
        result.dispatch.payload_type = packet.header.payload_type;
        result.dispatch.reason = result.reason;
        return result;
    }

    const bool remote_rebind_allowed = packet.header.payload_type == BattlePayloadType::Reconnect;
    if (!remote_rebind_allowed &&
        (packet.header.payload_type == BattlePayloadType::Input ||
        packet.header.payload_type == BattlePayloadType::ModeAction ||
        packet.header.payload_type == BattlePayloadType::Ping)) {
        const std::string remote_key = SessionRemoteKey(packet);
        if (!remote_key.empty()) {
            const auto remote_it = remote_endpoint_by_session_.find(remote_key);
            if (remote_it != remote_endpoint_by_session_.end() &&
                remote_it->second != datagram.remote_endpoint) {
                stats_.rejected_datagrams += 1;
                stats_.remote_endpoint_mismatches += 1;
                result.reason = "remote_endpoint_mismatch";
                result.dispatch.payload_type = packet.header.payload_type;
                result.dispatch.reason = result.reason;
                return result;
            }
        }
    }
    if (remote_rebind_allowed) {
        const std::string remote_key = SessionRemoteKey(packet);
        const auto remote_it = remote_endpoint_by_session_.find(remote_key);
        if (!remote_key.empty() &&
            remote_it != remote_endpoint_by_session_.end() &&
            remote_it->second != datagram.remote_endpoint &&
            server_.IsPlayerConnected(packet.header.match_id, packet.header.player_id)) {
            stats_.rejected_datagrams += 1;
            stats_.remote_endpoint_mismatches += 1;
            result.reason = "remote_rebind_player_connected";
            result.dispatch.payload_type = packet.header.payload_type;
            result.dispatch.reason = result.reason;
            return result;
        }
    }

    result.dispatch = server_.DispatchEncrypted(packet);
    result.reason = result.dispatch.reason;
    if (!result.dispatch.ok) {
        stats_.rejected_datagrams += 1;
        return result;
    }

    const std::string remote_key = SessionRemoteKey(packet);
    if (!remote_key.empty()) {
        const auto remote_it = remote_endpoint_by_session_.find(remote_key);
        if (remote_it != remote_endpoint_by_session_.end() &&
            remote_it->second != datagram.remote_endpoint &&
            remote_rebind_allowed) {
            stats_.remote_endpoint_rebinds += 1;
        }
        remote_endpoint_by_session_[remote_key] = datagram.remote_endpoint;
    }

    result.replies = endpoint_.ProcessDatagram(datagram);
    result.ok = true;
    result.reason = result.dispatch.response_kind;
    stats_.accepted_datagrams += 1;
    stats_.bound_sessions = remote_endpoint_by_session_.size();
    return result;
}

KcpAeadAdapterStats KcpAeadPacketAdapter::Stats() const {
    KcpAeadAdapterStats stats = stats_;
    stats.bound_sessions = remote_endpoint_by_session_.size();
    return stats;
}

}  // namespace phk::battle
