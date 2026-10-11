#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// 千夜追忆 1.40 玩家自机「射击 / 炸弹」参数表（数据驱动）。
//
// 来源：player_shots_disasm.md + player_shots_reimpl.hpp + PhK-Protocol/data/
//       tna_contract.json（player_shots，52 条）。仅用于自研玩法。
//
// 设计：一张表 + 一个引擎。射击与炸弹都是「每轮一组子弹」的数据描述，服务端
// 只按冷却帧发射并查伤害表，不为每个角色写独立代码。
// 角度单位 = 度（90 = 正上方）。
namespace phk::battle::danmaku {

// kMyBullet(type) → 伤害。伤害由 MyBullet::spriteWithFile 内部按 type 查表赋值，
// 不在 createBullets 参数里。返回 -1 表示未确定/不适用。
[[nodiscard]] int PlayerBulletDamage(int type);

// kMyBullet(type) → 贴图名（.png 由引擎补全）。
[[nodiscard]] const char* PlayerBulletTexture(int type);

// kMyBullet(炸弹 type) → 贴图名。
[[nodiscard]] const char* PlayerBombTexture(int type);

struct PlayerShotBullet {
    int type = 0;          // kMyBullet 值
    double angle_deg = 90.0;
    double speed = 0.0;
    double dx = 0.0;       // 相对自机 x 偏移（0 = 循环生成/未确定）
    double dy = 0.0;       // 相对自机 y 偏移
};

struct PlayerShotSpec {
    const char* name = "";
    const char* chara = "";
    char mode = 'A';
    int variant = 1;
    int bullets_per_volley = 0;
    double speed = 0.0;          // 代表速度（该轮最大值）
    double angle_min_deg = 90.0;
    double angle_max_deg = 90.0;
    int damage = -1;             // 主弹伤害
    int cooldown_frames = 8;     // *1=8；*2 默认 12（逐角色特判见 note）
    int tex_type = -1;           // 主弹 type
    bool resolved = false;       // 是否已完全解出
    const PlayerShotBullet* bullets = nullptr;
    int bullet_count = 0;
};

// 52 条射击（ReimuA1 … YuyukoB2，顺序见 player_shots_disasm.md）。
[[nodiscard]] const std::array<PlayerShotSpec, 52>& PlayerShotTable();

struct PlayerBombBullet {
    int type = 0;          // kMyBullet 值（62..82）
    double angle_deg = 90.0;
    int w1 = 0;            // 主弹 > 0（= 伤害）
};

struct PlayerBombSpec {
    const char* name = "";
    int duration_frames = 0;   // bombRemainTime
    int invuln_frames = 0;     // bombMutekiCnt
    int damage = -1;           // 主弹 w1
    int bomb_type = -1;
    const char* texture = "";
    const PlayerBombBullet* bullets = nullptr;
    int bullet_count = 0;
};

// 13 个角色炸弹。
[[nodiscard]] const std::array<PlayerBombSpec, 13>& PlayerBombTable();

}  // namespace phk::battle::danmaku
