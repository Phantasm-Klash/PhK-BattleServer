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

// T6 服务回调签名头：hex(HMAC-SHA256(callback_secret, 原始 body))。
// 大厅侧 verifyBattleCallbackSignature（httpapi/handler.go）对精确原始
// body 恒定时间校验；callback_secret 由注册响应下发（见 server.hpp /
// match_lifecycle.hpp 的 callback_secret 字段注释）。
inline constexpr const char* kBattleCallbackSignatureHeader = "X-PhK-Battle-Signature";

// B4-7.1 回调新鲜度头（与 Go 大厅 out/B4-7.1_replay_go.md §4 逐字节对齐）：
// 两头必须**成对**发送——只给其一，大厅回 403 battle_callback_freshness_incomplete。
// 两头的值都是**裸文本**（去首尾空白），不是 JSON 转义后的字符串。
inline constexpr const char* kBattleTimestampHeader = "X-PhK-Battle-Timestamp";
inline constexpr const char* kBattleNonceHeader = "X-PhK-Battle-Nonce";
// 大厅 nonce 上限（>64 字节回 403 battle_callback_nonce_invalid）。
inline constexpr std::size_t kBattleNonceMaxBytes = 64;

[[nodiscard]] std::vector<std::pair<std::string, std::string>> BattleEventsCallbackHeaders();

// 当前 unix 秒的十进制文本（大厅新鲜度窗口 ±300s，取真实墙钟）。
[[nodiscard]] std::string BattleCallbackTimestampNow();

// 生成一条**每条回调唯一**的 nonce（≤ kBattleNonceMaxBytes）。唯一性论证见
// card_events_report.cpp：进程随机前缀 + 进程内单调计数器 ⇒ 同进程绝不重复，
// 跨进程重启以 2^-64 概率避免碰撞，故 600s 保留窗内不重复。
[[nodiscard]] std::string NewBattleCallbackNonce();

// B4-7.1 签名材料：timestamp + "\n" + nonce + "\n" + body（LF 分隔，裸文本）。
[[nodiscard]] std::string BattleCallbackSignedMaterial(
    const std::string& timestamp,
    const std::string& nonce,
    const std::string& body
);

// 确定性入口（KAT/测试用）：显式 ts/nonce → {Timestamp, Nonce, Signature} 三头；
// callback_secret 为空时返回空（不签名）。
[[nodiscard]] std::vector<std::pair<std::string, std::string>> BattleCallbackSignedHeadersFor(
    const std::string& body,
    const std::string& callback_secret,
    const std::string& timestamp,
    const std::string& nonce
);

// 生产入口：现取 unix 秒 + 新 nonce，返回三头；callback_secret 为空时返回空。
// events / results / dem 三条回传路径共用（dem 侧此前是复制实现，B4-7.1 收敛）。
[[nodiscard]] std::vector<std::pair<std::string, std::string>> BattleCallbackSignedHeaders(
    const std::string& body,
    const std::string& callback_secret
);

// 通用 service-callback 头：公开两头（预过滤，语义不变）+ 三头新鲜度签名
// （B4-7.1：Timestamp / Nonce / Signature）。events 与 result 两条回传路径共用；
// callback_secret 为空时只发公开两头——大厅会以 403 battle_callback_signature_invalid
// 拒绝，部署错配立即可见（不会静默回退到旧的无签名行为）。
[[nodiscard]] std::vector<std::pair<std::string, std::string>> BattleCallbackHeaders(
    const std::string& body,
    const std::string& callback_secret
);

// BattleEventsSubmitRequest 的确定性 JSON（字段名与 Go struct tag 一致）：
//   battle_server_id / match_id / events[]
// 行字段（battle_events.go:41-50 的 omitempty 语义逐项照抄）：
//   type / tick / user_id / card_id / value 恒发；
//   target_user_id / reason 空则省略；slot <= 0 省略（Go omitempty 对 0 省略，
//   CardCastEvent 以 -1 表示"未设置"，两者在 JSON 上同形）。
//   例外：B3 接触行 hit / graze 带 user_id + value 但**省略 card_id**
//   （大厅 battle_events.go:249-256 对两者早退，不检查 card_id）。
[[nodiscard]] std::string BuildBattleEventsJson(
    const std::string& battle_server_id,
    const std::string& match_id,
    const std::vector<CardCastEvent>& rows
);

}  // namespace phk::battle

#endif  // PHK_BATTLE_CARD_EVENTS_REPORT_HPP
