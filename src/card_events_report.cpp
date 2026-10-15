// 对战服 → 大厅卡牌事件回传 payload 构造（纯函数，无网络）。
// 对照 Gensoulkyo/runtime/core/battle_events.go:41-171。
//
// 版权：东方同人逆向成果，仅用于自研玩法。

#include "phk/battle/card_events_report.hpp"

#include <string>
#include <utility>

#include "phk/battle/lobby_rng.hpp"  // HmacSha256Hex

namespace phk::battle {
namespace {

// JSON 字符串转义（Go encoding/json 的字符串规则子集：引号/反斜杠/控制字符）。
std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char ch : value) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    static const char* kHex = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(static_cast<unsigned char>(ch) >> 4) & 0x0fu];
                    out += kHex[static_cast<unsigned char>(ch) & 0x0fu];
                } else {
                    out += ch;
                }
                break;
        }
    }
    return out;
}

std::string JsonString(const std::string& value) {
    return "\"" + JsonEscape(value) + "\"";
}

}  // namespace

std::vector<std::pair<std::string, std::string>> BattleEventsCallbackHeaders() {
    return {
        {kBattleEventsServiceOriginHeader, kBattleEventsServiceOriginValue},
        {kBattleEventsCallbackHeader, kBattleEventsCallbackValue},
    };
}

std::vector<std::pair<std::string, std::string>> BattleCallbackHeaders(
    const std::string& body,
    const std::string& callback_secret
) {
    std::vector<std::pair<std::string, std::string>> headers = BattleEventsCallbackHeaders();
    if (callback_secret.empty()) {
        return headers;
    }
    headers.emplace_back(kBattleCallbackSignatureHeader, HmacSha256Hex(callback_secret, body));
    return headers;
}

std::string BuildBattleEventsJson(
    const std::string& battle_server_id,
    const std::string& match_id,
    const std::vector<CardCastEvent>& rows
) {
    std::string out = "{\"battle_server_id\":" + JsonString(battle_server_id) +
        ",\"match_id\":" + JsonString(match_id) +
        ",\"events\":[";
    bool first = true;
    for (const CardCastEvent& row : rows) {
        if (!first) {
            out += ",";
        }
        first = false;
        out += "{\"type\":" + JsonString(row.event_type);
        out += ",\"tick\":" + std::to_string(row.tick);
        out += ",\"user_id\":" + JsonString(row.player_id);
        out += ",\"card_id\":" + JsonString(row.card_id);
        out += ",\"value\":" + std::to_string(row.event_value);
        if (!row.target_user_id.empty()) {
            out += ",\"target_user_id\":" + JsonString(row.target_user_id);
        }
        // Go omitempty：slot 0 省略；CardCastEvent 以 -1 表示未设置，同形省略。
        if (row.slot > 0) {
            out += ",\"slot\":" + std::to_string(row.slot);
        }
        if (!row.reason.empty()) {
            out += ",\"reason\":" + JsonString(row.reason);
        }
        out += "}";
    }
    out += "]}";
    return out;
}

}  // namespace phk::battle
