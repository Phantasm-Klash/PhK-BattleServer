// 96 字节发射指令 SpawnCommand 的适配层。
//
// 权威定义在 PhK-Protocol 生成契约 `phk/v1/danmaku.hpp` 的 `BulletSpawnCommand`
// （对应原版 Enemy::CreateBulletCommand @0x37bacc，28 字段）。本头文件只做别名，
// 保证服务端与协议/客户端使用同一 POD，避免布局漂移。
//
// 关键约定：角度单位 = 度；spawn_x/spawn_y == -1.0f 为「跟随发射者」哨兵；
// count == 0 → 本次不发射。
#pragma once

#include "phk/v1/danmaku.hpp"

namespace phk::battle::danmaku {

using SpawnCommand = phk::v1::BulletSpawnCommand;

// 原版 createBullets1 中 spawnX/spawnY 的哨兵值。
inline constexpr float kSpawnFollowEmitter = -1.0f;

static_assert(sizeof(SpawnCommand) == 96, "SpawnCommand must match the original 96-byte layout");

}  // namespace phk::battle::danmaku
