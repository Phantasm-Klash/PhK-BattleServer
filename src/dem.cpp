// .dem v1 容器实现（thread B / dem 基建）。格式真值：COORDINATION.md §0.2
// 与 include/phk/battle/dem.hpp 头注释（逐字节冻结）。
//
// 确定性原语全部复用：digest = chain_log.hpp（ShortHashHex），整段 sha256 =
// lobby_rng.hpp Sha256Bytes。MD5（RFC 1321）仅用于上传分块校验，与确定性
// 哈希口径无关。HTTP 复用 match_lifecycle 的 HttpPostJson（不写第二个 client，
// 同 card_events_report.hpp 的既有约定）。
//
// 版权：东方同人逆向成果，仅用于自研玩法。

#include "phk/battle/dem.hpp"

#include "phk/battle/lobby_rng.hpp"
#include "phk/battle/card_events_report.hpp"  // kBattleCallbackSignatureHeader（T6 签名头）
#include "phk/battle/match_lifecycle.hpp"  // HttpPostJson / ParseHostPort（复用既有 client）

#include <sys/stat.h>
#include <sys/types.h>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <type_traits>

#ifdef _WIN32
#include <direct.h>
#endif

namespace phk::battle {
namespace {

// ---------------------------------------------------------------------------
// little-endian 字节助手（body / 容器定长字段）
// ---------------------------------------------------------------------------
void DemPutU16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xffu));
    out.push_back(static_cast<char>((value >> 8) & 0xffu));
}

void DemPutU32(std::string& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xffu));
    }
}

void DemPutU64(std::string& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xffu));
    }
}

void DemPutI64(std::string& out, std::int64_t value) {
    DemPutU64(out, static_cast<std::uint64_t>(value));
}

void DemPutBytes(std::string& out, const std::string& value) {
    DemPutU16(out, static_cast<std::uint16_t>(value.size()));
    out += value;
}

bool DemGetU16(const std::string& data, std::size_t* cursor, std::uint16_t* out) {
    if (*cursor + 2 > data.size()) return false;
    *out = static_cast<std::uint16_t>(
        static_cast<std::uint8_t>(data[*cursor]) |
        (static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[*cursor + 1])) << 8));
    *cursor += 2;
    return true;
}

bool DemGetU32(const std::string& data, std::size_t* cursor, std::uint32_t* out) {
    if (*cursor + 4 > data.size()) return false;
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[*cursor + i])) << (8 * i);
    }
    *cursor += 4;
    *out = value;
    return true;
}

bool DemGetU64(const std::string& data, std::size_t* cursor, std::uint64_t* out) {
    if (*cursor + 8 > data.size()) return false;
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[*cursor + i])) << (8 * i);
    }
    *cursor += 8;
    *out = value;
    return true;
}

bool DemGetI64(const std::string& data, std::size_t* cursor, std::int64_t* out) {
    std::uint64_t raw = 0;
    if (!DemGetU64(data, cursor, &raw)) return false;
    *out = static_cast<std::int64_t>(raw);
    return true;
}

bool DemGetBytes(const std::string& data, std::size_t* cursor, std::string* out) {
    std::uint16_t len = 0;
    if (!DemGetU16(data, cursor, &len)) return false;
    if (*cursor + len > data.size()) return false;
    out->assign(data, *cursor, len);
    *cursor += len;
    return true;
}

// ---------------------------------------------------------------------------
// JSON 转义（Go encoding/json 字符串规则子集：引号/反斜杠/控制字符）
// ---------------------------------------------------------------------------
std::string DemJsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    static const char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(static_cast<unsigned char>(ch) >> 4) & 0x0fu];
                    out += kHex[static_cast<unsigned char>(ch) & 0x0fu];
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

std::string DemJsonString(const std::string& value) {
    return "\"" + DemJsonEscape(value) + "\"";
}

}  // namespace

// ---------------------------------------------------------------------------
// header JSON 编码（字段顺序 = §0.2 声明顺序，逐字节冻结）
// ---------------------------------------------------------------------------
std::string EncodeDemHeaderJson(const DemHeader& header) {
    std::string out;
    out.reserve(512 + header.players.size() * 256);
    out += "{\"match_id\":" + DemJsonString(header.match_id);
    out += ",\"mode_id\":" + DemJsonString(header.mode_id);
    out += ",\"boss_id\":" + DemJsonString(header.boss_id);
    out += ",\"ruleset_version\":" + DemJsonString(header.ruleset_version);
    out += ",\"server_seed\":" + DemJsonString(header.server_seed);
    out += ",\"tick_rate_hz\":" + std::to_string(header.tick_rate_hz);
    out += ",\"final_tick\":" + std::to_string(header.final_tick);
    out += ",\"final_state_hash\":" + DemJsonString(header.final_state_hash);
    // 只在记录到时写出（0 = 沿用重放端默认），这样老容器字节不变。
    if (header.boss_max_hp > 0) {
        out += ",\"boss_max_hp\":" + std::to_string(header.boss_max_hp);
    }
    out += ",\"players\":[";
    for (std::size_t i = 0; i < header.players.size(); ++i) {
        const DemPlayer& player = header.players[i];
        if (i != 0) out += ',';
        out += "{\"player_id\":" + DemJsonString(player.player_id);
        out += ",\"user_id\":" + DemJsonString(player.user_id);
        out += ",\"loadout\":{\"character_id\":" + DemJsonString(player.loadout.character_id);
        out += ",\"shot_type\":" + DemJsonString(player.loadout.shot_type);
        out += ",\"bomb_type\":" + DemJsonString(player.loadout.bomb_type);
        out += "}}";
    }
    out += "]";
    out += ",\"created_at\":" + std::to_string(header.created_at);
    out += ",\"entry_count\":" + std::to_string(header.entry_count);
    out += ",\"chain_head_digest\":" + DemJsonString(header.chain_head_digest);
    out += "}";
    return out;
}

namespace {

// ---------------------------------------------------------------------------
// 迷你 JSON DOM + 递归下降解析器（只服务 header 反序列化）
// ---------------------------------------------------------------------------
struct DemJsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool bool_value = false;
    std::string number_text;
    std::string string_value;
    std::vector<DemJsonValue> array;
    std::vector<std::pair<std::string, DemJsonValue>> object;
};

class DemJsonParser {
public:
    explicit DemJsonParser(const std::string& text) : text_(text) {}

    bool Parse(DemJsonValue* out) {
        SkipWhitespace();
        if (!ParseValue(out, 0)) return false;
        SkipWhitespace();
        return cursor_ == text_.size();
    }

private:
    static constexpr int kMaxDepth = 32;

    void SkipWhitespace() {
        while (cursor_ < text_.size()) {
            const char ch = text_[cursor_];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                ++cursor_;
                continue;
            }
            break;
        }
    }

    bool Literal(const char* word) {
        const std::size_t len = std::strlen(word);
        if (text_.compare(cursor_, len, word) != 0) return false;
        cursor_ += len;
        return true;
    }

    bool ParseValue(DemJsonValue* out, int depth) {
        if (depth > kMaxDepth) return false;
        if (cursor_ >= text_.size()) return false;
        const char ch = text_[cursor_];
        if (ch == '{') return ParseObject(out, depth);
        if (ch == '[') return ParseArray(out, depth);
        if (ch == '"') {
            out->kind = DemJsonValue::Kind::String;
            return ParseString(&out->string_value);
        }
        if (ch == 't') {
            if (!Literal("true")) return false;
            out->kind = DemJsonValue::Kind::Bool;
            out->bool_value = true;
            return true;
        }
        if (ch == 'f') {
            if (!Literal("false")) return false;
            out->kind = DemJsonValue::Kind::Bool;
            out->bool_value = false;
            return true;
        }
        if (ch == 'n') {
            if (!Literal("null")) return false;
            out->kind = DemJsonValue::Kind::Null;
            return true;
        }
        return ParseNumber(out);
    }

    bool ParseObject(DemJsonValue* out, int depth) {
        out->kind = DemJsonValue::Kind::Object;
        ++cursor_;  // '{'
        SkipWhitespace();
        if (cursor_ < text_.size() && text_[cursor_] == '}') {
            ++cursor_;
            return true;
        }
        for (;;) {
            SkipWhitespace();
            std::string key;
            if (!ParseString(&key)) return false;
            SkipWhitespace();
            if (cursor_ >= text_.size() || text_[cursor_] != ':') return false;
            ++cursor_;
            SkipWhitespace();
            DemJsonValue value;
            if (!ParseValue(&value, depth + 1)) return false;
            out->object.emplace_back(std::move(key), std::move(value));
            SkipWhitespace();
            if (cursor_ >= text_.size()) return false;
            if (text_[cursor_] == ',') {
                ++cursor_;
                continue;
            }
            if (text_[cursor_] == '}') {
                ++cursor_;
                return true;
            }
            return false;
        }
    }

    bool ParseArray(DemJsonValue* out, int depth) {
        out->kind = DemJsonValue::Kind::Array;
        ++cursor_;  // '['
        SkipWhitespace();
        if (cursor_ < text_.size() && text_[cursor_] == ']') {
            ++cursor_;
            return true;
        }
        for (;;) {
            SkipWhitespace();
            DemJsonValue value;
            if (!ParseValue(&value, depth + 1)) return false;
            out->array.push_back(std::move(value));
            SkipWhitespace();
            if (cursor_ >= text_.size()) return false;
            if (text_[cursor_] == ',') {
                ++cursor_;
                continue;
            }
            if (text_[cursor_] == ']') {
                ++cursor_;
                return true;
            }
            return false;
        }
    }

    void AppendUtf8(std::string* out, std::uint32_t code_point) {
        if (code_point <= 0x7Fu) {
            *out += static_cast<char>(code_point);
        } else if (code_point <= 0x7FFu) {
            *out += static_cast<char>(0xC0u | (code_point >> 6));
            *out += static_cast<char>(0x80u | (code_point & 0x3Fu));
        } else if (code_point <= 0xFFFFu) {
            *out += static_cast<char>(0xE0u | (code_point >> 12));
            *out += static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu));
            *out += static_cast<char>(0x80u | (code_point & 0x3Fu));
        } else {
            *out += static_cast<char>(0xF0u | (code_point >> 18));
            *out += static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu));
            *out += static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu));
            *out += static_cast<char>(0x80u | (code_point & 0x3Fu));
        }
    }

    bool ParseHex4(std::uint32_t* out) {
        if (cursor_ + 4 > text_.size()) return false;
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char ch = text_[cursor_ + i];
            value <<= 4;
            if (ch >= '0' && ch <= '9') {
                value |= static_cast<std::uint32_t>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                value |= static_cast<std::uint32_t>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                value |= static_cast<std::uint32_t>(ch - 'A' + 10);
            } else {
                return false;
            }
        }
        cursor_ += 4;
        *out = value;
        return true;
    }

    bool ParseString(std::string* out) {
        if (cursor_ >= text_.size() || text_[cursor_] != '"') return false;
        ++cursor_;
        out->clear();
        while (cursor_ < text_.size()) {
            const char ch = text_[cursor_];
            if (ch == '"') {
                ++cursor_;
                return true;
            }
            if (static_cast<unsigned char>(ch) < 0x20) return false;  // 原始控制字符非法
            if (ch != '\\') {
                *out += ch;
                ++cursor_;
                continue;
            }
            ++cursor_;  // '\\'
            if (cursor_ >= text_.size()) return false;
            const char esc = text_[cursor_++];
            switch (esc) {
                case '"': *out += '"'; break;
                case '\\': *out += '\\'; break;
                case '/': *out += '/'; break;
                case 'b': *out += '\b'; break;
                case 'f': *out += '\f'; break;
                case 'n': *out += '\n'; break;
                case 'r': *out += '\r'; break;
                case 't': *out += '\t'; break;
                case 'u': {
                    std::uint32_t code_point = 0;
                    if (!ParseHex4(&code_point)) return false;
                    if (code_point >= 0xD800u && code_point <= 0xDBFFu) {
                        // 代理对：须跟随低代理
                        if (cursor_ + 2 > text_.size() || text_[cursor_] != '\\' || text_[cursor_ + 1] != 'u') {
                            return false;
                        }
                        cursor_ += 2;
                        std::uint32_t low = 0;
                        if (!ParseHex4(&low)) return false;
                        if (low < 0xDC00u || low > 0xDFFFu) return false;
                        code_point = 0x10000u + ((code_point - 0xD800u) << 10) + (low - 0xDC00u);
                    } else if (code_point >= 0xDC00u && code_point <= 0xDFFFu) {
                        return false;  // 孤立低代理
                    }
                    AppendUtf8(out, code_point);
                    break;
                }
                default:
                    return false;
            }
        }
        return false;  // 未闭合
    }

    bool ParseNumber(DemJsonValue* out) {
        const std::size_t start = cursor_;
        if (cursor_ < text_.size() && text_[cursor_] == '-') ++cursor_;
        const std::size_t int_start = cursor_;
        while (cursor_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor_]))) {
            ++cursor_;
        }
        if (cursor_ == int_start) return false;
        if (cursor_ < text_.size() && text_[cursor_] == '.') {
            ++cursor_;
            const std::size_t frac_start = cursor_;
            while (cursor_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor_]))) {
                ++cursor_;
            }
            if (cursor_ == frac_start) return false;
        }
        if (cursor_ < text_.size() && (text_[cursor_] == 'e' || text_[cursor_] == 'E')) {
            ++cursor_;
            if (cursor_ < text_.size() && (text_[cursor_] == '+' || text_[cursor_] == '-')) ++cursor_;
            const std::size_t exp_start = cursor_;
            while (cursor_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor_]))) {
                ++cursor_;
            }
            if (cursor_ == exp_start) return false;
        }
        out->kind = DemJsonValue::Kind::Number;
        out->number_text = text_.substr(start, cursor_ - start);
        return true;
    }

    const std::string& text_;
    std::size_t cursor_ = 0;
};

const DemJsonValue* DemField(const DemJsonValue& object, const std::string& key) {
    if (object.kind != DemJsonValue::Kind::Object) return nullptr;
    for (const auto& entry : object.object) {
        if (entry.first == key) return &entry.second;
    }
    return nullptr;
}

bool DemBindInt(const DemJsonValue* value, std::int64_t* out) {
    if (value == nullptr || value->kind != DemJsonValue::Kind::Number) return false;
    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(value->number_text.c_str(), &end, 10);
    if (errno != 0 || end == value->number_text.c_str() || *end != '\0') return false;
    *out = static_cast<std::int64_t>(parsed);
    return true;
}

bool DemBindUint(const DemJsonValue* value, std::uint64_t* out) {
    if (value == nullptr || value->kind != DemJsonValue::Kind::Number) return false;
    if (!value->number_text.empty() && value->number_text[0] == '-') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value->number_text.c_str(), &end, 10);
    if (errno != 0 || end == value->number_text.c_str() || *end != '\0') return false;
    *out = static_cast<std::uint64_t>(parsed);
    return true;
}

bool DemBindString(const DemJsonValue* value, std::string* out) {
    if (value == nullptr || value->kind != DemJsonValue::Kind::String) return false;
    *out = value->string_value;
    return true;
}

}  // namespace

bool DecodeDemHeaderJson(const std::string& json, DemHeader* out) {
    DemJsonParser parser(json);
    DemJsonValue root;
    if (!parser.Parse(&root)) return false;
    if (root.kind != DemJsonValue::Kind::Object) return false;

    DemHeader header;
    const DemJsonValue* field = nullptr;
    if (!DemBindString(DemField(root, "match_id"), &header.match_id)) return false;
    if (!DemBindString(DemField(root, "mode_id"), &header.mode_id)) return false;
    if (!DemBindString(DemField(root, "boss_id"), &header.boss_id)) return false;
    if (!DemBindString(DemField(root, "ruleset_version"), &header.ruleset_version)) return false;
    if (!DemBindString(DemField(root, "server_seed"), &header.server_seed)) return false;
    std::uint64_t tick_rate_hz = 0;
    if (!DemBindUint(DemField(root, "tick_rate_hz"), &tick_rate_hz)) return false;
    if (tick_rate_hz > std::numeric_limits<std::uint32_t>::max()) return false;
    header.tick_rate_hz = static_cast<std::uint32_t>(tick_rate_hz);
    if (!DemBindInt(DemField(root, "final_tick"), &header.final_tick)) return false;
    if (!DemBindString(DemField(root, "final_state_hash"), &header.final_state_hash)) return false;
    // 可选：容器不写这个键时（老容器 / 未记录）保持 0，重放端沿用自己的默认。
    if (DemField(root, "boss_max_hp") != nullptr) {
        std::uint64_t boss_max_hp = 0;
        if (!DemBindUint(DemField(root, "boss_max_hp"), &boss_max_hp)) return false;
        header.boss_max_hp = boss_max_hp;
    }
    if (!DemBindInt(DemField(root, "created_at"), &header.created_at)) return false;
    if (!DemBindUint(DemField(root, "entry_count"), &header.entry_count)) return false;
    if (!DemBindString(DemField(root, "chain_head_digest"), &header.chain_head_digest)) return false;

    field = DemField(root, "players");
    if (field == nullptr || field->kind != DemJsonValue::Kind::Array) return false;
    for (const DemJsonValue& item : field->array) {
        DemPlayer player;
        if (!DemBindString(DemField(item, "player_id"), &player.player_id)) return false;
        if (!DemBindString(DemField(item, "user_id"), &player.user_id)) return false;
        const DemJsonValue* loadout = DemField(item, "loadout");
        if (loadout == nullptr) return false;
        if (!DemBindString(DemField(*loadout, "character_id"), &player.loadout.character_id)) return false;
        if (!DemBindString(DemField(*loadout, "shot_type"), &player.loadout.shot_type)) return false;
        if (!DemBindString(DemField(*loadout, "bomb_type"), &player.loadout.bomb_type)) return false;
        header.players.push_back(std::move(player));
    }
    *out = std::move(header);
    return true;
}

// ---------------------------------------------------------------------------
// body 编解码
// ---------------------------------------------------------------------------
std::string EncodeDemBody(const std::vector<DemBodyEntry>& entries) {
    std::string out;
    for (const DemBodyEntry& entry : entries) {
        DemPutU64(out, entry.seq);
        DemPutI64(out, entry.tick);
        DemPutBytes(out, entry.player_id);
        DemPutBytes(out, entry.action_type);
        DemPutBytes(out, entry.payload);
    }
    return out;
}

bool DecodeDemBody(const std::string& body, std::vector<DemBodyEntry>* out) {
    out->clear();
    std::size_t cursor = 0;
    while (cursor < body.size()) {
        DemBodyEntry entry;
        if (!DemGetU64(body, &cursor, &entry.seq)) return false;
        if (!DemGetI64(body, &cursor, &entry.tick)) return false;
        if (!DemGetBytes(body, &cursor, &entry.player_id)) return false;
        if (!DemGetBytes(body, &cursor, &entry.action_type)) return false;
        if (!DemGetBytes(body, &cursor, &entry.payload)) return false;
        out->push_back(std::move(entry));
    }
    return true;
}

// ---------------------------------------------------------------------------
// zlib（容器 body 压缩；TS 侧 pako.inflate 兼容）
// ---------------------------------------------------------------------------
std::string DemZlibCompress(const std::string& raw) {
    const uLongf bound = compressBound(static_cast<uLong>(raw.size()));
    std::string out(static_cast<std::size_t>(bound), '\0');
    uLongf dest_len = bound;
    const int rc = compress2(
        reinterpret_cast<Bytef*>(out.empty() ? nullptr : &out[0]),
        &dest_len,
        reinterpret_cast<const Bytef*>(raw.data()),
        static_cast<uLong>(raw.size()),
        Z_DEFAULT_COMPRESSION
    );
    if (rc != Z_OK) return {};
    out.resize(static_cast<std::size_t>(dest_len));
    return out;
}

bool DemZlibDecompress(const std::string& compressed, std::string* out) {
    if (compressed.empty()) {
        out->clear();
        return false;  // 合法 zlib 流不可能是空字节
    }
    std::size_t capacity = std::max<std::size_t>(compressed.size() * 4u, 1024u);
    constexpr std::size_t kMaxCapacity = 256u * 1024u * 1024u;
    for (int attempt = 0; attempt < 10; ++attempt) {
        out->resize(capacity);
        uLongf dest_len = static_cast<uLongf>(out->size());
        const int rc = uncompress(
            reinterpret_cast<Bytef*>(&(*out)[0]),
            &dest_len,
            reinterpret_cast<const Bytef*>(compressed.data()),
            static_cast<uLong>(compressed.size())
        );
        if (rc == Z_OK) {
            out->resize(static_cast<std::size_t>(dest_len));
            return true;
        }
        if (rc != Z_BUF_ERROR) return false;
        if (capacity >= kMaxCapacity) return false;
        capacity = std::min(capacity * 4u, kMaxCapacity);
    }
    return false;
}

// ---------------------------------------------------------------------------
// 摘要 / base64
// ---------------------------------------------------------------------------
namespace {

std::string BytesToHex(const unsigned char* bytes, std::size_t size) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out += kHex[(bytes[i] >> 4) & 0x0fu];
        out += kHex[bytes[i] & 0x0fu];
    }
    return out;
}

}  // namespace

std::string DemSha256Hex(const std::string& bytes) {
    const std::array<std::uint8_t, 32> digest = Sha256Bytes(bytes);
    return BytesToHex(digest.data(), digest.size());
}

namespace {

std::uint32_t DemRotateLeft32(std::uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

}  // namespace

// RFC 1321 MD5（仅上传分块校验用；不影响任何确定性哈希口径）。
std::string DemMd5Hex(const std::string& input) {
    static const std::uint32_t kK[64] = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
        0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
        0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
        0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
        0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
        0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
        0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
        0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
        0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
        0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
        0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
        0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
        0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
        0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
        0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
        0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
    };
    static const int kS[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
    };

    std::string message = input;
    const std::uint64_t bit_length = static_cast<std::uint64_t>(input.size()) * 8u;
    message.push_back(static_cast<char>(0x80));
    while (message.size() % 64 != 56) {
        message.push_back('\0');
    }
    for (int i = 0; i < 8; ++i) {
        message.push_back(static_cast<char>((bit_length >> (8 * i)) & 0xffu));
    }

    std::uint32_t a0 = 0x67452301u;
    std::uint32_t b0 = 0xefcdab89u;
    std::uint32_t c0 = 0x98badcfeu;
    std::uint32_t d0 = 0x10325476u;

    for (std::size_t offset = 0; offset < message.size(); offset += 64) {
        std::uint32_t m[16];
        for (int i = 0; i < 16; ++i) {
            const std::size_t base = offset + static_cast<std::size_t>(i) * 4;
            m[i] = static_cast<std::uint32_t>(static_cast<std::uint8_t>(message[base])) |
                (static_cast<std::uint32_t>(static_cast<std::uint8_t>(message[base + 1])) << 8) |
                (static_cast<std::uint32_t>(static_cast<std::uint8_t>(message[base + 2])) << 16) |
                (static_cast<std::uint32_t>(static_cast<std::uint8_t>(message[base + 3])) << 24);
        }
        std::uint32_t a = a0;
        std::uint32_t b = b0;
        std::uint32_t c = c0;
        std::uint32_t d = d0;
        for (int i = 0; i < 64; ++i) {
            std::uint32_t f = 0;
            int g = 0;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            const std::uint32_t temp = d;
            d = c;
            c = b;
            b = b + DemRotateLeft32(a + f + kK[i] + m[g], kS[i]);
            a = temp;
        }
        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    }

    unsigned char digest[16];
    for (int i = 0; i < 4; ++i) {
        digest[i] = static_cast<unsigned char>((a0 >> (8 * i)) & 0xffu);
        digest[4 + i] = static_cast<unsigned char>((b0 >> (8 * i)) & 0xffu);
        digest[8 + i] = static_cast<unsigned char>((c0 >> (8 * i)) & 0xffu);
        digest[12 + i] = static_cast<unsigned char>((d0 >> (8 * i)) & 0xffu);
    }
    return BytesToHex(digest, sizeof(digest));
}

std::string DemBase64Encode(const std::string& bytes) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < bytes.size()) {
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i])) << 16) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i + 1])) << 8) |
            static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i + 2]));
        out += kAlphabet[(triple >> 18) & 0x3fu];
        out += kAlphabet[(triple >> 12) & 0x3fu];
        out += kAlphabet[(triple >> 6) & 0x3fu];
        out += kAlphabet[triple & 0x3fu];
        i += 3;
    }
    const std::size_t remaining = bytes.size() - i;
    if (remaining == 1) {
        const std::uint32_t triple = static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i])) << 16;
        out += kAlphabet[(triple >> 18) & 0x3fu];
        out += kAlphabet[(triple >> 12) & 0x3fu];
        out += '=';
        out += '=';
    } else if (remaining == 2) {
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i])) << 16) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i + 1])) << 8);
        out += kAlphabet[(triple >> 18) & 0x3fu];
        out += kAlphabet[(triple >> 12) & 0x3fu];
        out += kAlphabet[(triple >> 6) & 0x3fu];
        out += '=';
    }
    return out;
}

// ---------------------------------------------------------------------------
// BossRaceConfig 适配（线程 A 加字段中：存在才读）
//
// 这些检测必须是模板：非模板函数里的 if constexpr 丢弃分支仍会被完整编译，
// BossRaceConfig 尚无 loadouts 成员时非模板写法直接编译失败。实例化时条件
// 为 false 的分支才真正丢弃——A 定稿加字段后自动切换读取路径。
// ---------------------------------------------------------------------------
namespace {

template <typename T, typename = void>
struct DemHasBossId : std::false_type {};
template <typename T>
struct DemHasBossId<T, std::void_t<decltype(std::declval<const T&>().boss_id)>> : std::true_type {};

template <typename T, typename = void>
struct DemHasLoadouts : std::false_type {};
template <typename T>
struct DemHasLoadouts<T, std::void_t<decltype(std::declval<const T&>().loadouts)>> : std::true_type {};

template <typename E, typename = void>
struct DemHasCharacterId : std::false_type {};
template <typename E>
struct DemHasCharacterId<E, std::void_t<decltype(std::declval<const E&>().character_id)>> : std::true_type {};

template <typename E, typename = void>
struct DemHasShotType : std::false_type {};
template <typename E>
struct DemHasShotType<E, std::void_t<decltype(std::declval<const E&>().shot_type)>> : std::true_type {};

template <typename E, typename = void>
struct DemHasBombType : std::false_type {};
template <typename E>
struct DemHasBombType<E, std::void_t<decltype(std::declval<const E&>().bomb_type)>> : std::true_type {};

template <typename E, typename = void>
struct DemHasEntryPlayerId : std::false_type {};
template <typename E>
struct DemHasEntryPlayerId<E, std::void_t<decltype(std::declval<const E&>().player_id)>> : std::true_type {};

template <typename ConfigT>
DemPlayerLoadout DemLoadoutForPlayerImpl(const ConfigT& config, const std::string& player_id) {
    DemPlayerLoadout loadout;
    if constexpr (DemHasLoadouts<ConfigT>::value) {
        for (const auto& entry : config.loadouts) {
            if constexpr (DemHasEntryPlayerId<decltype(entry)>::value) {
                if (entry.player_id != player_id) continue;
            }
            if constexpr (DemHasCharacterId<decltype(entry)>::value) {
                loadout.character_id = entry.character_id;
            }
            if constexpr (DemHasShotType<decltype(entry)>::value) {
                loadout.shot_type = entry.shot_type;
            }
            if constexpr (DemHasBombType<decltype(entry)>::value) {
                loadout.bomb_type = entry.bomb_type;
            }
            break;
        }
    } else {
        (void)config;
        (void)player_id;
    }
    return loadout;
}

// boss_id 读取必须走模板：非模板函数里 if constexpr 的丢弃分支仍会被完整
// 编译，BossRaceConfig 尚无 boss_id 成员时直接编译失败。
template <typename ConfigT>
std::string DemBossIdImpl(const ConfigT& config) {
    if constexpr (DemHasBossId<ConfigT>::value) {
        return config.boss_id;
    } else {
        (void)config;
        return {};
    }
}

}  // namespace

std::string DemBossIdFromConfig(const BossRaceConfig& config) {
    return DemBossIdImpl(config);
}

std::vector<DemPlayer> BuildDemPlayers(
    const BossRaceConfig& config,
    const std::vector<std::string>& player_ids,
    const std::map<std::string, std::string>& user_ids
) {
    std::vector<DemPlayer> players;
    players.reserve(player_ids.size());
    for (const std::string& player_id : player_ids) {
        DemPlayer player;
        player.player_id = player_id;
        const auto it = user_ids.find(player_id);
        if (it != user_ids.end()) {
            player.user_id = it->second;
        }
        player.loadout = DemLoadoutForPlayerImpl(config, player_id);
        players.push_back(std::move(player));
    }
    return players;
}

// ---------------------------------------------------------------------------
// 容器写 / 读
// ---------------------------------------------------------------------------
DemWriteResult WriteDemDocument(
    const DemHeader& header,
    const std::vector<ChainEntry>& entries,
    const std::vector<std::string>& payloads
) {
    DemWriteResult result;
    if (entries.size() != payloads.size()) {
        result.reason = "dem_payload_count_mismatch";
        return result;
    }
    if (header.entry_count != entries.size()) {
        result.reason = "dem_header_entry_count_mismatch";
        return result;
    }
    const std::string expected_head = entries.empty()
        ? std::string(kChainGenesisPrevDigest)
        : entries.back().entry_digest;
    if (header.chain_head_digest != expected_head) {
        result.reason = "dem_header_head_digest_mismatch";
        return result;
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (ChainPayloadDigest(payloads[i]) != entries[i].payload_digest) {
            result.reason = "dem_payload_digest_mismatch";
            return result;
        }
    }

    std::vector<DemBodyEntry> body_entries;
    body_entries.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const ChainEntry& entry = entries[i];
        const std::string& payload = payloads[i];
        if (entry.player_id.size() > kDemFieldMaxBytes ||
            entry.action_type.size() > kDemFieldMaxBytes ||
            payload.size() > kDemFieldMaxBytes) {
            result.reason = "dem_payload_too_long";
            return result;
        }
        DemBodyEntry body_entry;
        body_entry.seq = entry.seq;
        body_entry.tick = entry.tick;
        body_entry.player_id = entry.player_id;
        body_entry.action_type = entry.action_type;
        body_entry.payload = payload;
        body_entries.push_back(std::move(body_entry));
    }

    const ChainVerificationResult verified = VerifyChain(entries);
    if (!verified.ok) {
        result.reason = "dem_chain_verify_failed:" + verified.reason;
        return result;
    }

    const std::string header_json = EncodeDemHeaderJson(header);
    const std::string body = EncodeDemBody(body_entries);
    const std::string compressed = DemZlibCompress(body);
    if (!body.empty() && compressed.empty()) {
        result.reason = "dem_zlib_compress_failed";
        return result;
    }

    std::string bytes;
    bytes.reserve(8 + 4 + header_json.size() + 8 + compressed.size() + 32);
    bytes.append(kDemMagic, 8);
    DemPutU32(bytes, static_cast<std::uint32_t>(header_json.size()));
    bytes += header_json;
    DemPutU64(bytes, static_cast<std::uint64_t>(compressed.size()));
    bytes += compressed;
    const std::array<std::uint8_t, 32> digest = Sha256Bytes(header_json + compressed);
    bytes.append(reinterpret_cast<const char*>(digest.data()), digest.size());

    result.ok = true;
    result.reason = "ok";
    result.bytes = std::move(bytes);
    return result;
}

DemParseResult ParseDemDocument(const std::string& bytes) {
    DemParseResult result;
    constexpr std::size_t kFixedTail = 8 + 4 + 8 + 32;
    constexpr std::uint32_t kMaxHeaderLen = 4u * 1024u * 1024u;
    constexpr std::uint64_t kMaxBodyLen = 256u * 1024u * 1024u;

    if (bytes.size() < kFixedTail) {
        result.reason = "dem_truncated";
        return result;
    }
    if (bytes.compare(0, 8, kDemMagic, 8) != 0) {
        result.reason = "dem_bad_magic";
        return result;
    }
    std::size_t cursor = 8;
    std::uint32_t header_len = 0;
    if (!DemGetU32(bytes, &cursor, &header_len)) {
        result.reason = "dem_truncated";
        return result;
    }
    if (header_len > kMaxHeaderLen) {
        result.reason = "dem_header_too_large";
        return result;
    }
    if (cursor + header_len + 8 > bytes.size()) {
        result.reason = "dem_truncated";
        return result;
    }
    const std::string header_json = bytes.substr(cursor, header_len);
    cursor += header_len;
    std::uint64_t body_len = 0;
    if (!DemGetU64(bytes, &cursor, &body_len)) {
        result.reason = "dem_truncated";
        return result;
    }
    if (body_len > kMaxBodyLen || cursor + body_len + 32 != bytes.size()) {
        result.reason = "dem_length_mismatch";
        return result;
    }
    const std::string compressed = bytes.substr(cursor, static_cast<std::size_t>(body_len));
    cursor += static_cast<std::size_t>(body_len);
    const std::string trailer = bytes.substr(cursor, 32);

    const std::array<std::uint8_t, 32> expected = Sha256Bytes(header_json + compressed);
    if (trailer.size() != 32 || std::memcmp(trailer.data(), expected.data(), 32) != 0) {
        result.reason = "dem_sha256_mismatch";
        return result;
    }

    std::string body;
    if (!DemZlibDecompress(compressed, &body)) {
        result.reason = "dem_zlib_failed";
        return result;
    }

    std::vector<DemBodyEntry> body_entries;
    if (!DecodeDemBody(body, &body_entries)) {
        result.reason = "dem_body_decode_failed";
        return result;
    }

    // 由 payload 字节重建链条目（body 不存 digest，全部重算）后验证。
    std::vector<ChainEntry> entries;
    entries.reserve(body_entries.size());
    for (const DemBodyEntry& body_entry : body_entries) {
        ChainEntry entry;
        entry.seq = body_entry.seq;
        entry.tick = body_entry.tick;
        entry.player_id = body_entry.player_id;
        entry.action_type = body_entry.action_type;
        entry.payload_digest = ChainPayloadDigest(body_entry.payload);
        entry.prev_digest = entries.empty()
            ? std::string(kChainGenesisPrevDigest)
            : entries.back().entry_digest;
        entry.entry_digest = ComputeChainEntryDigest(entry);
        entries.push_back(std::move(entry));
    }
    const ChainVerificationResult verified = VerifyChain(entries);
    if (!verified.ok) {
        result.reason = "dem_chain_verify_failed:" + verified.reason;
        return result;
    }

    DemHeader header;
    if (!DecodeDemHeaderJson(header_json, &header)) {
        result.reason = "dem_header_decode_failed";
        return result;
    }
    if (header.entry_count != body_entries.size()) {
        result.reason = "dem_header_entry_count_mismatch";
        return result;
    }
    const std::string computed_head = entries.empty()
        ? std::string(kChainGenesisPrevDigest)
        : entries.back().entry_digest;
    if (header.chain_head_digest != computed_head) {
        result.reason = "dem_header_head_digest_mismatch";
        return result;
    }

    result.ok = true;
    result.reason = "ok";
    result.document.header = std::move(header);
    result.document.entries = std::move(body_entries);
    return result;
}

// ---------------------------------------------------------------------------
// 录制器
// ---------------------------------------------------------------------------
void DemRecorder::RecordMatchSeed(std::uint64_t seed) {
    if (!chain_.Empty()) return;
    chain_.AppendMatchSeed(0, kChainSeedKindBattleDerived, seed);
    payloads_.push_back(CanonicalChainSeedPayload(kChainSeedKindBattleDerived, seed));
}

void DemRecorder::RecordInput(const BattleInput& input) {
    chain_.AppendInput(
        static_cast<std::int64_t>(input.tick),
        input.player_id,
        input.seq,
        input.direction_bits,
        input.slow,
        input.shoot,
        input.bomb,
        input.card_slot,
        input.mode_action_id
    );
    payloads_.push_back(CanonicalChainInputPayload(
        input.seq,
        input.direction_bits,
        input.slow,
        input.shoot,
        input.bomb,
        input.card_slot,
        input.mode_action_id
    ));
}

void DemRecorder::RecordModeAction(const BattleModeAction& action) {
    chain_.AppendModeAction(
        static_cast<std::int64_t>(action.tick),
        action.player_id,
        action.action_id,
        action.seq,
        action.action_type,
        action.payload_json
    );
    payloads_.push_back(CanonicalChainModeActionPayload(
        action.action_id,
        action.seq,
        action.payload_json
    ));
}

void DemRecorder::NoteUserId(const std::string& player_id, const std::string& user_id) {
    if (user_id.empty()) return;
    const auto it = user_ids_.find(player_id);
    if (it != user_ids_.end() && !it->second.empty()) return;  // 首个非空胜出
    user_ids_[player_id] = user_id;
}

DemWriteResult DemRecorder::Finalize(const DemHeader& header) const {
    return WriteDemDocument(header, chain_.Entries(), payloads_);
}

// ---------------------------------------------------------------------------
// 本地落盘
// ---------------------------------------------------------------------------
std::string DemDirectoryFromEnv() {
    const char* env = std::getenv(kDemDirectoryEnvVar);
    if (env != nullptr && *env != '\0') {
        return std::string(env);
    }
    return std::string(kDemDefaultDirectory);
}

bool DemUploadEnabled() {
    const char* env = std::getenv(kDemUploadEnvVar);
    if (env == nullptr || *env == '\0') return true;  // 默认开启
    std::string value(env);
    for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return !(value == "0" || value == "false" || value == "off" || value == "no");
}

std::string DemLocalPath(const std::string& match_id) {
    // match_id 来自大厅分配，仍做路径穿越防护：仅保留安全字符。
    std::string safe;
    safe.reserve(match_id.size());
    for (const char ch : match_id) {
        const bool ok = std::isalnum(static_cast<unsigned char>(ch)) || ch == '.' || ch == '_' || ch == '-';
        safe += ok ? ch : '_';
    }
    if (safe.empty() || safe == "." || safe == "..") {
        safe = "unknown_match";
    }
    return DemDirectoryFromEnv() + "/" + safe + ".dem";
}

namespace {

bool DemMakeDirectories(const std::string& directory) {
    if (directory.empty()) return false;
    std::string path;
    std::size_t cursor = 0;
    if (directory[0] == '/') {
        path = "/";
        cursor = 1;
    }
    while (cursor < directory.size()) {
        const std::size_t slash = directory.find('/', cursor);
        const std::size_t end = slash == std::string::npos ? directory.size() : slash;
        if (end > cursor) {
            if (!path.empty() && path.back() != '/') path += '/';
            path += directory.substr(cursor, end - cursor);
#ifdef _WIN32
            ::_mkdir(path.c_str());
#else
            ::mkdir(path.c_str(), 0755);
#endif
        }
        if (slash == std::string::npos) break;
        cursor = slash + 1;
    }
    struct stat st {};
    return ::stat(directory.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

}  // namespace

bool WriteDemFile(const std::string& path, const std::string& bytes) {
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        DemMakeDirectories(path.substr(0, slash));
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    return static_cast<bool>(out);
}

// ---------------------------------------------------------------------------
// 上传（分块 base64 + md5 + sha256；复用 HttpPostJson）
// ---------------------------------------------------------------------------
namespace {

std::vector<std::pair<std::string, std::string>> DemCallbackHeaders(
    const std::string& body,
    const std::string& callback_secret
) {
    std::vector<std::pair<std::string, std::string>> headers = {
        {kDemServiceOriginHeader, kDemServiceOriginValue},
        {kDemBattleCallbackHeader, kDemBattleCallbackValue}
    };
    // 同 BattleCallbackHeaders（card_events_report.cpp:71）：secret 为空时不加
    // 签名头，大厅会以 403 battle_callback_signature_invalid 拒绝——部署错配
    // 立即可见，不会静默回退到无签名上传。
    if (!callback_secret.empty()) {
        headers.emplace_back(kBattleCallbackSignatureHeader, HmacSha256Hex(callback_secret, body));
    }
    return headers;
}

std::string BuildDemUploadChunkJson(
    const DemUploadOptions& options,
    std::size_t dem_size,
    const std::string& dem_sha256,
    std::size_t chunk_index,
    std::size_t chunk_total,
    const std::string& chunk
) {
    std::string out;
    out += "{\"battle_server_id\":" + DemJsonString(options.battle_server_id);
    out += ",\"replay_id\":" + DemJsonString(options.replay_id);
    out += ",\"match_id\":" + DemJsonString(options.match_id);
    out += ",\"dem_size\":" + std::to_string(dem_size);
    out += ",\"dem_sha256\":" + DemJsonString(dem_sha256);
    out += ",\"chunk_index\":" + std::to_string(chunk_index);
    out += ",\"chunk_total\":" + std::to_string(chunk_total);
    out += ",\"chunk_md5\":" + DemJsonString(DemMd5Hex(chunk));
    out += ",\"chunk_base64\":" + DemJsonString(DemBase64Encode(chunk));
    out += "}";
    return out;
}

}  // namespace

DemUploadResult UploadDemBytes(const DemUploadOptions& options, const std::string& dem_bytes) {
    DemUploadResult result;
    if (options.host.empty() || options.port == 0) {
        result.reason = "dem_upload_no_endpoint";
        return result;
    }
    if (options.match_id.empty()) {
        result.reason = "dem_upload_match_id_required";
        return result;
    }
    if (options.battle_server_id.empty()) {
        result.reason = "dem_upload_battle_server_id_required";
        return result;
    }

    const std::string dem_sha256 = DemSha256Hex(dem_bytes);
    const std::size_t total = std::max<std::size_t>(
        1, (dem_bytes.size() + kDemUploadChunkBytes - 1) / kDemUploadChunkBytes);
    for (std::size_t index = 0; index < total; ++index) {
        const std::size_t begin = index * kDemUploadChunkBytes;
        const std::size_t end = std::min(dem_bytes.size(), begin + kDemUploadChunkBytes);
        const std::string chunk = dem_bytes.substr(begin, end - begin);
        const std::string json = BuildDemUploadChunkJson(
            options, dem_bytes.size(), dem_sha256, index, total, chunk);
        // 签名覆盖该请求的精确 raw body 字节（每个分块单独签名）。
        const auto headers = DemCallbackHeaders(json, options.callback_secret);
        const HttpPostResult post = HttpPostJson(
            options.host, options.port, options.path, json, options.timeout_ms, headers);
        result.response_body = post.body;
        if (!post.ok) {
            result.reason = "dem_upload_http_" + std::to_string(post.status_code);
            if (!post.error.empty()) {
                result.reason += ":" + post.error;
            }
            return result;
        }
        if (post.body.find("\"ok\":true") == std::string::npos) {
            result.reason = "dem_upload_rejected";
            return result;
        }
    }
    result.ok = true;
    result.reason = "ok";
    return result;
}

std::string DemExtractJsonStringField(const std::string& json, const std::string& field) {
    const std::string needle = "\"" + field + "\"";
    std::size_t cursor = json.find(needle);
    if (cursor == std::string::npos) return {};
    cursor += needle.size();
    while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor]))) ++cursor;
    if (cursor >= json.size() || json[cursor] != ':') return {};
    ++cursor;
    while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor]))) ++cursor;
    if (cursor >= json.size() || json[cursor] != '"') return {};
    ++cursor;
    std::string value;
    while (cursor < json.size()) {
        const char ch = json[cursor];
        if (ch == '"') return value;
        if (ch == '\\' && cursor + 1 < json.size()) {
            const char esc = json[cursor + 1];
            cursor += 2;
            switch (esc) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case 'r': value += '\r'; break;
                case 'b': value += '\b'; break;
                case 'f': value += '\f'; break;
                case '"': value += '"'; break;
                case '\\': value += '\\'; break;
                case '/': value += '/'; break;
                case 'u': {
                    if (cursor + 4 > json.size()) return {};
                    std::uint32_t code_point = 0;
                    bool ok = true;
                    for (int i = 0; i < 4; ++i) {
                        const char hex = json[cursor + i];
                        code_point <<= 4;
                        if (hex >= '0' && hex <= '9') {
                            code_point |= static_cast<std::uint32_t>(hex - '0');
                        } else if (hex >= 'a' && hex <= 'f') {
                            code_point |= static_cast<std::uint32_t>(hex - 'a' + 10);
                        } else if (hex >= 'A' && hex <= 'F') {
                            code_point |= static_cast<std::uint32_t>(hex - 'A' + 10);
                        } else {
                            ok = false;
                            break;
                        }
                    }
                    if (!ok) return {};
                    cursor += 4;
                    if (code_point <= 0x7Fu) {
                        value += static_cast<char>(code_point);
                    } else if (code_point <= 0x7FFu) {
                        value += static_cast<char>(0xC0u | (code_point >> 6));
                        value += static_cast<char>(0x80u | (code_point & 0x3Fu));
                    } else {
                        value += static_cast<char>(0xE0u | (code_point >> 12));
                        value += static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu));
                        value += static_cast<char>(0x80u | (code_point & 0x3Fu));
                    }
                    break;
                }
                default:
                    return {};
            }
            continue;
        }
        value += ch;
        ++cursor;
    }
    return {};
}

}  // namespace phk::battle
