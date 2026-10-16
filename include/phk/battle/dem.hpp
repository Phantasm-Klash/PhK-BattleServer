// .dem v1 对战录制容器（thread B / dem 基建）。
//
// 容器格式（COORDINATION.md §0.2，双端共识，逐字节冻结）：
//   [8B magic "PHDEM001"][u32 header_len][header JSON][u64 body_len]
//   [zlib(body)][32B sha256(header+body)]
//   - header：match_id / mode_id / boss_id / ruleset_version /
//     server_seed（十进制字符串）/ tick_rate_hz / final_tick /
//     final_state_hash / players[{player_id,user_id,
//     loadout{character_id,shot_type,bomb_type}}] / created_at /
//     entry_count / chain_head_digest
//   - body：复用 OperationChainLog 条目序列化
//     [u64 seq][i64 tick][u16 len][player_id][u16 len][action_type]
//     [u16 len][payload]，input payload = CanonicalChainInputPayload
//     规范化字节（digest 链保持可验）
//
// 确定性只复用既有原语：digest 口径 = chain_log.hpp / lobby_rng.hpp
// （ShortHashHex = SHA-256 前 4 字节 8 位小写十六进制），不新造哈希。
// 整段 sha256 同为 SHA-256（64 位十六进制），由 lobby_rng 的 Sha256Bytes 输出。
//
// 上传（COORDINATION §0.2 + §0.1.1）：对局结束生成容器 → 本地落盘 →
// 经 RPC battle.replay.upload 上传（分块 base64 ≤512KB/块 + 每块 md5 +
// 整体 sha256，幂等）。服务间鉴权沿用 battle.agent.endpoint（3daf021）/
// /v1/battle/events 的服务回调头先例（X-PhK-Service-Origin +
// X-PhK-Battle-Callback），C++ 侧通道复用 SubmitResultIfNeeded 已有的
// lobby HTTP 管道（lobby_endpoint，原生 TCP socket，无 libcurl）。
//
// 版权：东方同人逆向成果，仅用于自研玩法。
#ifndef PHK_BATTLE_DEM_HPP
#define PHK_BATTLE_DEM_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "phk/battle/boss_race.hpp"  // BossRaceConfig：boss_id/loadouts 字段存在才读
#include "phk/battle/chain_log.hpp"  // ChainEntry / OperationChainLog / 摘要原语
#include "phk/battle/protocol.hpp"   // BattleInput / BattleModeAction

namespace phk::battle {

// 容器 magic（8 字节，含结尾 '\0' 由调用方按 8 字节写）。
inline constexpr const char* kDemMagic = "PHDEM001";

// body 条目中 player_id / action_type / payload 的 u16 长度字段上限。
inline constexpr std::uint32_t kDemFieldMaxBytes = 0xFFFFu;

// 上传分块阈值：每块 base64 前的明文字节数（COORDINATION §0.2：≤512KB/块）。
inline constexpr std::size_t kDemUploadChunkBytes = 512u * 1024u;

// 本地落盘默认目录（RAKsmart 生产路径；可用环境变量 PHK_DEM_DIR 覆盖）。
inline constexpr const char* kDemDefaultDirectory = "/root/gotouhou/dem";

// 环境变量名：本地目录 / 上传开关。
inline constexpr const char* kDemDirectoryEnvVar = "PHK_DEM_DIR";
inline constexpr const char* kDemUploadEnvVar = "PHK_DEM_UPLOAD";

// 上传路由（httpapi 服务回调面；鉴权头同 /v1/battle/events 先例）。
inline constexpr const char* kDemUploadPath = "/v1/battle/replays/dem";

// service-callback 鉴权头（card_events_report.hpp 先例，常量同值同义）。
inline constexpr const char* kDemServiceOriginHeader = "X-PhK-Service-Origin";
inline constexpr const char* kDemServiceOriginValue = "battle_server";
inline constexpr const char* kDemBattleCallbackHeader = "X-PhK-Battle-Callback";
inline constexpr const char* kDemBattleCallbackValue = "true";

// ---------------------------------------------------------------------------
// header / body / document
// ---------------------------------------------------------------------------
struct DemPlayerLoadout {
    std::string character_id;
    std::string shot_type;
    std::string bomb_type;
};

struct DemPlayer {
    std::string player_id;
    std::string user_id;  // 已验证 ticket 的 user_id；无 ticket 对局为空串
    DemPlayerLoadout loadout;
};

struct DemHeader {
    std::string match_id;
    std::string mode_id;
    std::string boss_id;
    std::string ruleset_version;
    std::string server_seed;  // 十进制字符串（对齐大厅 loadout server_seed 口径）
    std::uint32_t tick_rate_hz = 0;
    std::int64_t final_tick = 0;
    std::string final_state_hash;
    std::vector<DemPlayer> players;
    std::int64_t created_at = 0;  // unix 秒
    std::uint64_t entry_count = 0;
    std::string chain_head_digest;
};

// body 条目：链条目 + 规范化 payload 原始字节（链本身只存 digest）。
struct DemBodyEntry {
    std::uint64_t seq = 0;
    std::int64_t tick = 0;
    std::string player_id;
    std::string action_type;
    std::string payload;
};

struct DemDocument {
    DemHeader header;
    std::vector<DemBodyEntry> entries;
};

// ---------------------------------------------------------------------------
// 容器写 / 读
// ---------------------------------------------------------------------------
struct DemWriteResult {
    bool ok = false;
    std::string reason;  // 失败码（dem_* 前缀）
    std::string bytes;   // 完整 .dem 容器字节
};

struct DemParseResult {
    bool ok = false;
    std::string reason;
    DemDocument document;
};

// 生成容器字节。entries 与 payloads 一一对应（payloads[i] 是 entries[i] 的
// 规范化 payload 字节）；逐项校验 ChainPayloadDigest(payloads[i]) ==
// entries[i].payload_digest，任何字符串字段超 u16 限返回
// dem_payload_too_long，header 对账失败返回 dem_header_*_mismatch。
[[nodiscard]] DemWriteResult WriteDemDocument(
    const DemHeader& header,
    const std::vector<ChainEntry>& entries,
    const std::vector<std::string>& payloads
);

// 解析 + 全量校验：magic / 长度边界 / sha256 尾 / zlib 解压 / 链验证 /
// payload 摘要一致性 / header（entry_count、chain_head_digest）对账。
[[nodiscard]] DemParseResult ParseDemDocument(const std::string& bytes);

// ---------------------------------------------------------------------------
// 编解码原语（暴露给单测 / 黄金向量；字段顺序即字节顺序，逐字节冻结）
// ---------------------------------------------------------------------------
[[nodiscard]] std::string EncodeDemHeaderJson(const DemHeader& header);
[[nodiscard]] bool DecodeDemHeaderJson(const std::string& json, DemHeader* out);
[[nodiscard]] std::string EncodeDemBody(const std::vector<DemBodyEntry>& entries);
[[nodiscard]] bool DecodeDemBody(const std::string& body, std::vector<DemBodyEntry>* out);
[[nodiscard]] std::string DemZlibCompress(const std::string& raw);
[[nodiscard]] bool DemZlibDecompress(const std::string& compressed, std::string* out);

// 摘要 / 编码工具。SHA-256 复用 lobby_rng（不新造哈希）；MD5 仅用于上传
// 分块校验（RFC 1321），与确定性哈希口径无关。
[[nodiscard]] std::string DemSha256Hex(const std::string& bytes);
[[nodiscard]] std::string DemMd5Hex(const std::string& bytes);
[[nodiscard]] std::string DemBase64Encode(const std::string& bytes);

// ---------------------------------------------------------------------------
// 录制器（一场对局一个实例）
// ---------------------------------------------------------------------------
// 生产 app 路径（MatchServer + BossRaceSimulation）没有 OperationChainLog
// （COORDINATION §0.1.1），由本类持有链 + 并行 payload 字节；BattleServer
// 路径（BattleSimulation 自带链）同样可用它持有 payload 侧。
// 接受序即链序：只在验证通过后调用 Record*。
class DemRecorder {
public:
    // genesis 条目（tick 0，seed_kind=battle_derived）。幂等：仅空链时生效。
    void RecordMatchSeed(std::uint64_t seed);

    // 接受的 BattleInput。tick 取 input.tick（语义对齐 simulation.cpp:764
    // BattleSimulation::AcceptInput 的入链口径）。
    void RecordInput(const BattleInput& input);

    // 接受的 BattleModeAction（语义对齐 simulation.cpp:1053）。
    void RecordModeAction(const BattleModeAction& action);

    // 已验证 ticket 的 player_id -> user_id（header players[].user_id 来源）。
    void NoteUserId(const std::string& player_id, const std::string& user_id);

    [[nodiscard]] const OperationChainLog& Chain() const { return chain_; }
    [[nodiscard]] const std::vector<std::string>& Payloads() const { return payloads_; }
    [[nodiscard]] const std::map<std::string, std::string>& UserIds() const { return user_ids_; }
    [[nodiscard]] std::size_t Size() const { return chain_.Size(); }
    [[nodiscard]] bool Empty() const { return chain_.Empty(); }

    // 生成容器字节。header.entry_count / chain_head_digest 必须与链一致，
    // 否则 dem_header_entry_count_mismatch / dem_header_head_digest_mismatch。
    [[nodiscard]] DemWriteResult Finalize(const DemHeader& header) const;

private:
    OperationChainLog chain_;
    std::vector<std::string> payloads_;
    std::map<std::string, std::string> user_ids_;
};

// ---------------------------------------------------------------------------
// BossRaceConfig 适配（线程 A 正在加 boss_id / loadouts，字段存在才读）
// ---------------------------------------------------------------------------
// 线程A 定稿前 BossRaceConfig 无这两个字段：SFINAE 检测保证本模块在前后两版
// 结构体下均可编译；A 定稿后按实际字段形态对齐（COORDINATION §0.1.1，
// lead 会通知）。字段缺失时 boss_id 返回空串、loadout 返回空值。
[[nodiscard]] std::string DemBossIdFromConfig(const BossRaceConfig& config);
[[nodiscard]] std::vector<DemPlayer> BuildDemPlayers(
    const BossRaceConfig& config,
    const std::vector<std::string>& player_ids,
    const std::map<std::string, std::string>& user_ids
);

// ---------------------------------------------------------------------------
// 本地落盘 + 上传
// ---------------------------------------------------------------------------
// PHK_DEM_DIR 覆盖默认目录；目录不存在时尝试创建（best-effort）。
[[nodiscard]] std::string DemDirectoryFromEnv();
[[nodiscard]] std::string DemLocalPath(const std::string& match_id);
[[nodiscard]] bool WriteDemFile(const std::string& path, const std::string& bytes);
// PHK_DEM_UPLOAD=0/false/off/no 时跳过上传（本地文件照写）。
[[nodiscard]] bool DemUploadEnabled();

struct DemUploadOptions {
    std::string host;
    std::uint16_t port = 0;
    std::string path = kDemUploadPath;
    std::string replay_id;  // 信息字段（审计 / 日志）；存储以 match_id 为键
    std::string match_id;
    // 服务回调鉴权（同 /v1/battle/events）：battle_server_id 供大厅派生每服
    // HMAC 密钥并校验对局归属；callback_secret 用于对每个分块 raw body 签名。
    std::string battle_server_id;
    std::string callback_secret;
    int timeout_ms = 10000;
};

struct DemUploadResult {
    bool ok = false;
    std::string reason;
    std::string response_body;
};

// 分块 base64（每块明文字节数 ≤ kDemUploadChunkBytes）+ 每块 md5 +
// 整体 sha256；任一块 HTTP/业务失败即中止（后续块不再发送）。
[[nodiscard]] DemUploadResult UploadDemBytes(
    const DemUploadOptions& options,
    const std::string& dem_bytes
);

// 从结果回调响应 JSON 中取字符串字段（如 replay_id）；取不到返回空串。
[[nodiscard]] std::string DemExtractJsonStringField(
    const std::string& json,
    const std::string& field
);

}  // namespace phk::battle

#endif  // PHK_BATTLE_DEM_HPP
