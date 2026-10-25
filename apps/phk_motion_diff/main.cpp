// 数理差分测试台 —— C++ 侧运行器（网关）。
//
// 读取 gen_scenarios.py 生成的共享场景，逐 tick 跑**生产**运动学路径
// （UpdateBulletMotion + 宿主积分 x+=vx / y+=vy / t+=1，与
// BossRaceSimulation::AdvanceBullets 同构；RNG 每 tick 按
// (seed, tick, pattern_index, spawn_index) 重建，与该处一致），输出
// motion-cpp.txt。与 TS 侧 motion-ts.txt 交 compare.py 比对。
//
// 用法：phk_motion_diff scenarios.txt motion-cpp.txt

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "phk/battle/danmaku/bullet_motion.hpp"

namespace {

using phk::battle::danmaku::BulletMotion;
using phk::battle::danmaku::DeterministicRng;
using phk::battle::danmaku::MotionTargets;
using phk::battle::danmaku::UpdateBulletMotion;

struct Scenario {
    std::size_t index = 0;
    int func_no = 0;
    int ticks = 0;
    std::uint64_t seed = 0;
    std::uint32_t pattern_index = 0;
    std::uint32_t spawn_index = 0;
    BulletMotion motion;
    MotionTargets targets;
};

void ApplyMotionField(BulletMotion& b, const std::string& name, double value) {
    if (name == "x") b.x = value;
    else if (name == "y") b.y = value;
    else if (name == "vx") b.vx = value;
    else if (name == "vy") b.vy = value;
    else if (name == "angle") b.angle = value;
    else if (name == "speed") b.speed = value;
    else if (name == "accel") b.accel = value;
    else if (name == "t") b.t = value;
    else if (name == "life") b.life = static_cast<int>(value);
    else if (name == "angle_spd") b.angle_spd = value;
    else if (name == "angle_acc") b.angle_acc = value;
    else if (name == "f56c") b.f56c = value;
    else if (name == "speed_cap") b.speed_cap = value;
    else if (name == "angle_init") b.angle_init = value;
    else if (name == "speed_init") b.speed_init = value;
    else if (name == "f598") b.f598 = value;
    else if (name == "prev_angle") b.prev_angle = value;
    else if (name == "spd_init2") b.spd_init2 = value;
    else if (name == "xacc") b.xacc = value;
    else if (name == "xacc_cap") b.xacc_cap = value;
    else if (name == "pf1") b.pf1 = value;
    else if (name == "pf2") b.pf2 = value;
    else if (name == "bullet_kind") b.bullet_kind = static_cast<int>(value);
    else if (name == "rotate_flg") b.rotate_flg = static_cast<int>(value);
    else if (name == "bullet_life") b.bullet_life = static_cast<int>(value);
    else if (name == "bullet_loop") b.bullet_loop = static_cast<int>(value);
    else if (name == "delay") b.delay = static_cast<int>(value);
    else if (name == "vanish_delay") b.vanish_delay = static_cast<int>(value);
    else if (name == "p1") b.p1 = static_cast<int>(value);
    else if (name == "p2") b.p2 = static_cast<int>(value);
    else if (name == "bullet_func_no") b.bullet_func_no = static_cast<int>(value);
    else if (name == "hit_dis") b.hit_dis = value;
    else {
        std::cerr << "unknown motion field: " << name << "\n";
        std::exit(2);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: phk_motion_diff <scenarios.txt> <out.txt>\n");
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    std::vector<Scenario> scenarios;
    std::string token;
    while (in >> token) {
        if (token == "scenario") {
            Scenario scenario;
            std::string seed_lo_hex;
            std::string seed_hi_hex;
            in >> scenario.index >> scenario.func_no >> scenario.ticks >> seed_lo_hex >>
                seed_hi_hex >> scenario.pattern_index >> scenario.spawn_index;
            scenario.seed = (std::stoull(seed_hi_hex, nullptr, 16) << 32) |
                            std::stoull(seed_lo_hex, nullptr, 16);
            scenarios.push_back(std::move(scenario));
            continue;
        }
        if (token == "motion") {
            std::string name;
            double value = 0.0;
            in >> name >> value;
            ApplyMotionField(scenarios.back().motion, name, value);
            continue;
        }
        if (token == "target") {
            std::string which;
            int has = 0;
            double x = 0.0;
            double y = 0.0;
            in >> which >> has >> x >> y;
            if (which == "my_ship") {
                scenarios.back().targets.has_my_ship = has != 0;
                scenarios.back().targets.my_ship_x = x;
                scenarios.back().targets.my_ship_y = y;
            } else {
                scenarios.back().targets.has_enemy = has != 0;
                scenarios.back().targets.enemy_x = x;
                scenarios.back().targets.enemy_y = y;
            }
            continue;
        }
        if (token == "end" || token[0] == '#') {
            continue;
        }
    }

    std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "cannot open %s\n", argv[2]);
        return 1;
    }
    for (const Scenario& scenario : scenarios) {
        BulletMotion b = scenario.motion;
        // 初始速度由 (angle, speed) 合成（宿主在发射时做一次 IntegrateVelocity）。
        b.IntegrateVelocity();
        for (int tick = 1; tick <= scenario.ticks; ++tick) {
            DeterministicRng rng(scenario.seed, static_cast<std::uint64_t>(tick),
                                 scenario.pattern_index, scenario.spawn_index);
            UpdateBulletMotion(b, scenario.targets, rng);
            b.x += b.vx;
            b.y += b.vy;
            b.t += 1.0;
            char line[512];
            std::snprintf(line, sizeof(line),
                          "%zu %d %.17g %.17g %.17g %.17g %.17g %.17g %.17g %d\n",
                          scenario.index, tick, b.x, b.y, b.vx, b.vy, b.angle, b.speed,
                          b.t, b.life);
            out << line;
        }
    }
    out.flush();
    if (!out) {
        std::fprintf(stderr, "write failed\n");
        return 1;
    }
    std::fprintf(stdout, "wrote %s (%zu scenarios)\n", argv[2], scenarios.size());
    return 0;
}
