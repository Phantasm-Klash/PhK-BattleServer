// 对战服 → 大厅卡牌事件回传（H-1）：行收集 → JSON payload → POST /v1/battle/events。
//
// 真值来源（2026-10-16 对照）：
//   - Gensoulkyo/runtime/core/battle_events.go:41-171（BattleEventRow /
//     BattleEventsSubmitRequest / SubmitBattleEvents / validateBattleEventRowLocked）
//   - Gensoulkyo/runtime/httpapi/handler.go:1064-1088（battleEvents 路由守卫）
//   - Gensoulkyo/runtime/httpapi/handler.go:17-18 / types.go:27-33（callback 头）
//
// 设计：payload 构造是纯函数（无网络），单测直接对大厅的字段名/范围/白名单
// 断言；HTTP 复用 match_lifecycle 的 HttpPostJson（不写第二个 client）。
//
// 版权：东方同人逆向成果，仅用于自研玩法。
#ifndef PHK_BATTLE_CARD_EVENTS_REPORT_HPP
#define PHK_BATTLE_CARD_EVENTS_REPORT_HPP

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "phk/battle/simulation.hpp"  // CardCastEvent

namespace phk::battle {

// lobby maxBattleEventsPerSubmit（battle_events.go:23）：一次提交的行数上限。
inline constexpr std::size_t kMaxBattleEventsPerSubmit = 64;

// service-callback 鉴权头（与 submit-result 家族同一守卫
// serviceCallbackHeadersMatch，handler.go:1309；常量 types.go:27-33）。
// X-PhK-Service-Origin: battle_server + X-PhK-Battle-Callback: true
inline constexpr const char* kBattleEventsServiceOriginHeader = "X-PhK-Service-Origin";
inline constexpr const char* kBattleEventsServiceOriginValue = "battle_server";
inline constexpr const char* kBattleEventsCallbackHeader = "X-PhK-Battle-Callback";
inline constexpr const char* kBattleEventsCallbackValue = "true";

// 默认回传路径（handler.go:266 路由 POST /v1/battle/events）。
inline constexpr const char* kBattleEventsPath = "/v1/battle/events";

[[nodiscard]] std::vector<std::pair<std::string, std::string>> BattleEventsCallbackHeaders();

// BattleEventsSubmitRequest 的确定性 JSON（字段名与 Go struct tag 一致）：
//   battle_server_id / match_id / events[]
// 行字段（battle_events.go:41-50 的 omitempty 语义逐项照抄）：
//   type / tick / user_id / card_id / value 恒发；
//   target_user_id / reason 空则省略；slot <= 0 省略（Go omitempty 对 0 省略，
//   CardCastEvent 以 -1 表示"未设置"，两者在 JSON 上同形）。
[[nodiscard]] std::string BuildBattleEventsJson(
    const std::string& battle_server_id,
    const std::string& match_id,
    const std::vector<CardCastEvent>& rows
);

}  // namespace phk::battle

#endif  // PHK_BATTLE_CARD_EVENTS_REPORT_HPP
