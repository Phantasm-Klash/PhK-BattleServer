// phk_motion_sheet —— 把每个 bulletFunc（0..84）跑成一条轨迹，输出 JSONL 供客户端画图。
//
// 目的：肉眼核对「所有运动类型」是否都真的在动、形状是否与原作同类。
// 每个 func 用同一组初始条件（屏幕中心、角度 0°、速度 3px/tick、半径 4px），
// 跑 N tick，每 step tick 记一个点。
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "phk/battle/danmaku/bullet_motion.hpp"

namespace {

constexpr double kStartX = 320.0;
constexpr double kStartY = 480.0;
constexpr int kMaxFunc = 84;
constexpr int kTicks = 90;
constexpr int kSampleEvery = 3;

}  // namespace

int main(int argc, char** argv) {
    std::string out_path = "/tmp/motion_sheet.jsonl";
    int ticks = kTicks;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg == "--ticks" && i + 1 < argc) {
            ticks = std::atoi(argv[++i]);
        }
    }

    std::FILE* out = std::fopen(out_path.c_str(), "w");
    if (out == nullptr) {
        std::perror("open");
        return 1;
    }

    phk::battle::danmaku::MotionTargets targets;
    targets.my_ship_x = 320.0;
    targets.my_ship_y = 780.0;
    targets.has_my_ship = true;
    targets.enemy_x = 320.0;
    targets.enemy_y = 200.0;
    targets.has_enemy = true;

    int emitted = 0;
    for (int func = 0; func <= kMaxFunc; ++func) {
        phk::battle::danmaku::DeterministicRng rng(1234u, static_cast<std::uint64_t>(func) + 1u, 0u, 0u);
        phk::battle::danmaku::BulletMotion bullet;
        bullet.x = kStartX;
        bullet.y = kStartY;
        bullet.angle = 0.0;
        bullet.angle_init = 0.0;
        bullet.speed = 3.0;
        bullet.speed_init = 3.0;
        bullet.spd_init2 = 3.0;
        bullet.pf1 = 1.0;
        bullet.pf2 = 4.0;
        bullet.hit_dis = 4.0;
        bullet.bullet_func_no = func;
        bullet.life = ticks + 10;
        bullet.bullet_life = ticks + 10;

        std::fprintf(out, "{\"func\":%d,\"points\":[", func);
        bool first = true;
        for (int tick = 1; tick <= ticks; ++tick) {
            // UpdateBulletMotion 只更新 vx/vy（与原版一致），位移由调用方积分。
            phk::battle::danmaku::UpdateBulletMotion(bullet, targets, rng);
            bullet.x += bullet.vx;
            bullet.y += bullet.vy;
            if (tick % kSampleEvery == 0 || tick == 1) {
                std::fprintf(out, "%s[%.3f,%.3f]", first ? "" : ",", bullet.x, bullet.y);
                first = false;
            }
        }
        std::fprintf(out, "]}\n");
        ++emitted;
    }

    std::fclose(out);
    std::printf("motion sheet written: %s (funcs=%d, ticks=%d)\n", out_path.c_str(), emitted, ticks);
    return 0;
}
