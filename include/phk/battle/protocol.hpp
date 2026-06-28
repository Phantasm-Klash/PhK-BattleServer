#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "phk/battle/version.hpp"

namespace phk::battle {

enum class BattlePayloadType : std::uint8_t {
    Unspecified = 0,
    HandshakeHello = 1,
    HandshakeAccept = 2,
    Input = 3,
    Snapshot = 4,
    Event = 5,
    Ping = 6,
    Reconnect = 7,
    Result = 8,
};

struct BattlePacketHeader {
    VersionStamp version;
    std::string match_id;
    std::string player_id;
    std::uint64_t tick = 0;
    std::uint64_t seq = 0;
    std::uint64_t ack = 0;
    BattlePayloadType payload_type = BattlePayloadType::Unspecified;
    std::string key_id;
    std::string nonce_hex;
};

struct BattleInput {
    VersionStamp version;
    std::string match_id;
    std::string player_id;
    std::uint64_t tick = 0;
    std::uint64_t seq = 0;
    std::uint32_t direction_bits = 0;
    bool slow = false;
    bool shoot = false;
    bool bomb = false;
    int card_slot = -1;
    std::string mode_action_id;
};

struct BattlePlayerSnapshot {
    std::string player_id;
    std::int32_t x_milli = 0;
    std::int32_t y_milli = 0;
    bool connected = true;
    std::uint32_t hand_size = 0;
};

struct BattleBulletDelta {
    std::string bullet_id;
    std::string op = "upsert";
    std::int32_t x_milli = 0;
    std::int32_t y_milli = 0;
    std::int32_t vx_milli = 0;
    std::int32_t vy_milli = 0;
    std::uint32_t radius_milli = 0;
    std::string pattern_id;
    std::string color;
};

struct BattleSnapshot {
    VersionStamp version;
    std::string match_id;
    std::uint64_t snapshot_tick = 0;
    std::string snapshot_kind = "full";
    std::string state_hash;
    std::vector<BattlePlayerSnapshot> players;
    std::vector<BattleBulletDelta> bullets_delta;
    std::map<std::string, std::string> mode_state;
    std::uint64_t event_cursor = 0;
};

struct DispatchResult {
    bool ok = false;
    std::string reason;
    BattlePayloadType payload_type = BattlePayloadType::Unspecified;
    std::string response_kind;
};

class BattleDispatcher {
public:
    DispatchResult Dispatch(
        const BattlePacketHeader& header,
        const std::vector<std::uint8_t>& plaintext_payload
    );

private:
    std::map<std::string, std::uint64_t> last_seq_by_player_;
    std::map<std::string, std::uint64_t> last_tick_by_match_;
};

[[nodiscard]] std::string PayloadTypeName(BattlePayloadType type);

}  // namespace phk::battle
