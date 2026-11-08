// tna_reference_runner.cpp
// 原版 TNA 弹幕运动基准（gateway 侧模拟环境）。
//
// 基准来源：docs/apk-reverse-notes/patterns/bullet_funcs_reimpl.hpp
//   —— 从 libMyGame_arm64.so AArch64 逐指令翻译的 C++17 零依赖重实现。
//
// 固定随机数：每个 func 开跑前 srand(seed)，HostHooks 全部注入固定目标，
// 因此同一 seed 的轨迹完全可复现。
//
// 输出 JSONL：一行 = 一个 (func, tick) 的完整弹幕状态，字段覆盖
//   位置 x/y、方向 angle/vx/vy、大小 hitDis/halfW/halfH、类型 bulletType。
//
// 用法：tna_reference_runner --out <jsonl> [--ticks N] [--seed S]

#include "bullet_funcs_reimpl.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr float kShipX = 320.0f;
constexpr float kShipY = 600.0f;
constexpr float kEnemyX = 320.0f;
constexpr float kEnemyY = 420.0f;

void GetMyShipPos(float& x, float& y) { x = kShipX; y = kShipY; }
void GetEnemyPos(int, float& x, float& y) { x = kEnemyX; y = kEnemyY; }

float AngleTo(float x, float y, float px, float py) {
    const float dx = px - x;
    const float dy = py - y;
    float deg = std::atan2(dy, dx) * 180.0f / touhou::PI;
    // 与生产 bullet_motion.cpp::AngleToPoint / NormalizeDeg 同一约定 [0,360)，
    // 否则弹飞过自机时 atan2 在 ±180 跳变，限速分支会分叉（func17 实测）。
    deg = std::fmod(deg, 360.0f);
    if (deg < 0.0f) deg += 360.0f;
    return deg;
}
float AngleToMyShip(float x, float y) { return AngleTo(x, y, kShipX, kShipY); }
float AngleToEnemy(float x, float y, int, float) { return AngleTo(x, y, kEnemyX, kEnemyY); }
float AngleToPoint(float x, float y, float px, float py) { return AngleTo(x, y, px, py); }
int GetGlobalState() { return 0; }

void InstallHooks() {
    auto& h = touhou::hooks();
    h.get_my_ship_pos = GetMyShipPos;
    h.get_enemy_pos = GetEnemyPos;
    h.get_angle_to_my_ship = AngleToMyShip;
    h.get_angle_to_enemy = AngleToEnemy;
    h.get_angle_to_point = AngleToPoint;
    h.get_global_state = GetGlobalState;
}

// 与 apps/phk_motion_sheet/main.cpp 完全相同的初始条件，保证两端可比。
touhou::BulletMotion MakeInitial(int func) {
    touhou::BulletMotion b;
    b.x = 320.0f;
    b.y = 480.0f;
    b.angle = 0.0f;
    b.speed = 3.0f;
    b.bulletFuncNo = func;
    b.bulletType = 45;      // 常见 kome 系
    b.hitDis = 4.0f;
    b.halfW = 4.0f;
    b.halfH = 4.0f;
    b.pf1 = 1.0f;
    b.pf2 = 4.0f;
    b.hitHantei = true;
    b.bulletLife = 0;
    b.life = 0;
    b.t = 0.0f;
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_path = "/tmp/tna-reference.jsonl";
    int ticks = 90;
    unsigned seed = 12345;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) out_path = argv[++i];
        else if (arg == "--ticks" && i + 1 < argc) ticks = std::atoi(argv[++i]);
        else if (arg == "--seed" && i + 1 < argc) seed = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    }

    InstallHooks();
    std::FILE* out = std::fopen(out_path.c_str(), "w");
    if (out == nullptr) { std::perror("open"); return 1; }

    for (int func = 0; func <= 85; ++func) {
        std::srand(seed);  // 每个 func 独立、可复现
        touhou::BulletMotion b = MakeInitial(func);
        for (int tick = 0; tick < ticks; ++tick) {
            touhou::update_common(b, 1.0f);
            b.x += b.vx;
            b.y += b.vy;
            b.t += 1.0f;
            std::fprintf(out,
                "{\"func\":%d,\"tick\":%d,\"x\":%.6f,\"y\":%.6f,"
                "\"vx\":%.6f,\"vy\":%.6f,\"angle\":%.6f,\"speed\":%.6f,"
                "\"hit_dis\":%.6f,\"half_w\":%.6f,\"half_h\":%.6f,"
                "\"kind\":%d,\"life\":%d,\"t\":%.1f}\n",
                func, tick, b.x, b.y, b.vx, b.vy, b.angle, b.speed,
                b.hitDis, b.halfW, b.halfH, b.bulletType, b.life, b.t);
        }
    }
    std::fclose(out);
    std::printf("wrote %s (86 funcs x %d ticks, seed=%u)\n", out_path.c_str(), ticks, seed);
    return 0;
}
