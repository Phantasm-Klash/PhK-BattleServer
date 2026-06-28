#pragma once

#include <string>
#include <string_view>

#include "phk/v1/manifest.hpp"

namespace phk::battle {

inline constexpr int kProtocolVersion = phk::v1::kProtocolVersion;
inline constexpr std::string_view kBusinessApiVersion = phk::v1::kBusinessApiVersion;
inline constexpr std::string_view kBattleApiVersion = phk::v1::kBattleApiVersion;
inline constexpr std::string_view kDefaultRulesetVersion = phk::v1::kRulesetVersion;
inline constexpr std::string_view kRulesetHash = phk::v1::kRulesetHash;
inline constexpr std::string_view kProtocolSourceDigestSha256 = phk::v1::kSourceDigestSha256;

struct VersionStamp {
    int protocol_version = kProtocolVersion;
    std::string business_api_version = std::string(kBusinessApiVersion);
    std::string battle_api_version = std::string(kBattleApiVersion);
    std::string ruleset_version = std::string(kDefaultRulesetVersion);

    [[nodiscard]] bool IsCompatible() const {
        return protocol_version == kProtocolVersion &&
            battle_api_version == kBattleApiVersion;
    }
};

}  // namespace phk::battle
