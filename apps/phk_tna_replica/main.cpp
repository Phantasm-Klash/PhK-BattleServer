// phk_tna_replica.cpp — 对战端弹幕运动复刻运行器。
//
// 与 gateway 侧 tna_reference_runner 使用**完全相同的初始条件、固定 seed、
// 固定目标钩子**，逐 tick 跑生产路径
//   UpdateBulletMotion + 宿主积分 x+=vx / y+=vy / t+=1
// （与 BossRaceSimulation::AdvanceBullets 同构），输出同格式 JSONL。
//
// 用途：与 /tmp/tna-reference.jsonl 交给 tools/tna_coverage/compare_tna.py
// 逐 (func,tick) 比对，生成「每个弹幕类型是否如期实现」的覆盖矩阵。
//
// 用法：phk_tna_replica --out <jsonl> [--ticks N] [--seed S]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "phk/battle/danmaku/bullet_motion.hpp"

namespace {

using phk::battle::danmaku::BulletMotion;
using phk::battle::danmaku::DeterministicRng;
using phk::battle::danmaku::MotionTargets;
using phk::battle::danmaku::UpdateBulletMotion;

constexpr double kShipX = 320.0;
constexpr double kShipY = 600.0;
constexpr double kEnemyX = 320.0;
constexpr double kEnemyY = 420.0;

MotionTargets MakeTargets() {
    MotionTargets tg;
    tg.has_my_ship = true;
    tg.my_ship_x = kShipX;
    tg.my_ship_y = kShipY;
    tg.has_enemy = true;
    tg.enemy_x = kEnemyX;
    tg.enemy_y = kEnemyY;
    return tg;
}

BulletMotion MakeInitial(int func) {
    BulletMotion b;
    b.x = 320.0;
    b.y = 480.0;
    b.angle = 0.0;
    b.speed = 3.0;
    b.bullet_func_no = func;
    b.bullet_kind = 45;
    b.hit_dis = 4.0;
    b.pf1 = 1.0;
    b.pf2 = 4.0;
    b.hit_hantei = true;
    b.bullet_life = 0;
    b.life = 4;
    b.half_w = 4.0;
    b.half_h = 4.0;
    if (func == 28 || func == 36 || func == 79) {
        b.p1 = 2;
        b.p2 = 8;
    }
    b.t = 0.0;
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_path = "/tmp/tna-replica.jsonl";
    int ticks = 90;
    std::uint64_t seed = 12345;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) out_path = argv[++i];
        else if (arg == "--ticks" && i + 1 < argc) ticks = std::atoi(argv[++i]);
        else if (arg == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
    }

    const MotionTargets targets = MakeTargets();
    std::FILE* out = std::fopen(out_path.c_str(), "w");
    if (out == nullptr) { std::perror("open"); return 1; }

    for (int func = 0; func <= 85; ++func) {
      for (const int life0 : {4, 1, 0}) {
        BulletMotion b = MakeInitial(func);
        b.life = life0;
        for (int tick = 0; tick < ticks; ++tick) {
            DeterministicRng rng(seed, static_cast<std::uint64_t>(tick),
                                 static_cast<std::uint32_t>(func), 0);
            UpdateBulletMotion(b, targets, rng);
            b.x += b.vx;
            b.y += b.vy;
            b.t += 1.0;
            // 原版 halfW/halfH 是贴图半宽/半高（reimpl 里恒为初始 4、无 func 改写），
            // 与 hit_dis（命中半径）不是一回事。生产没有这两个字段，这里如实输出
            // 初始值，避免把 hit_dis 的合法变化误报成「大小不一致」。
            std::fprintf(out,
                "{\"func\":%d,\"tick\":%d,\"life0\":%d,\"x\":%.6f,\"y\":%.6f,"
                "\"vx\":%.6f,\"vy\":%.6f,\"angle\":%.6f,\"speed\":%.6f,"
                "\"hit_dis\":%.6f,\"half_w\":%.6f,\"half_h\":%.6f,"
                "\"kind\":%d,\"life\":%d,\"t\":%.1f,\"opacity\":%d,\"scale\":%.6f}\n",
                func, tick, life0, b.x, b.y, b.vx, b.vy, b.angle, b.speed,
                b.hit_dis, b.half_w, b.half_h, b.bullet_kind, b.life, b.t, b.opacity, b.scale);
        }
      }
    }
    std::fclose(out);
    std::printf("wrote %s (86 funcs x %d ticks, seed=%llu)\n",
                out_path.c_str(), ticks, static_cast<unsigned long long>(seed));
    return 0;
}
