// ChainNode 数据表（数据 + 纯函数展开）。
//
// 当前唯一条目：setBossBullet21 抽样转写（boss_id "boss21"，对应原版
// Enemy::setBossBullet21 @0x4d6f28，40440 字节 / 10110 指令 / 146 节点 [实测统计]）。
//
// **抽样范围声明（禁止编造）**：
// boss_pattern_tables.md 对 #21 只有聚合统计，没有逐节点解码；已逐指令解码的节点
// 属于 #11（473 节点的样板函数）。本文件按任务书转写「可实证的节点」= #11 已解码
// 触发块的结构与数值，作为 #21 时间线的抽样样板：
//   * 触发块 A（§4.2，frame % 300 == 0）：头节点（id 0xfa=250，空载荷 count 0）+
//     载荷节点（id 0x32=50，count 1，偏移 ±cos(a)·30 / ±sin(a)·30 或 ±30）——全字段实测；
//   * 触发块 B（§4.3，frame % 200 ≤ 59 且 frame % 16 == 0）：触发时刻与
//     getAngleToMyShip 瞄准实证；节点 id / count / 偏移**未能确定** → count=0 占位
//     （不发射），角度表达式与 72.0/−0.1 参数记录在溯源字段。
// 其余约 140 个 #21 节点未转写（逆向表未解码）。#21 自身的周期/阶段门控/节点 id
// 同样未解码——块 A/B 的周期与门控值取自 #11 解码，标注为样板来源。
// 节点尺寸 0x40 为任务书指定假定（#11 实测值；#21 尺寸逆向表未给出）。
#include "phk/battle/danmaku/chain_node.hpp"
#include "phk/battle/danmaku/bullet_mode_map.hpp"
#include "phk/battle/danmaku/bullet_motion.hpp"
#include "phk/battle/danmaku/tna_boss21.hpp"

#include <cmath>
#include <cstddef>
#include <utility>

#include "phk/battle/boss_race.hpp"  // kRulesetDanmakuMechanics（权威常量）

namespace phk::battle::danmaku {
namespace {

// 原版模式 id → (w2, w3)（bullet_mode_map 双跳转表，字段赋意为[推断]）。
// 50 → (53, 182)：w2=53 作 bullet_func_no；w3=182 的字段赋值未验证，
// 故 motion_override.bullet_kind 留 0 = 发射时回退 cmd.bullet_type（50 → kome 纹理，
// 与 bullet_kind_table 的纹理表语义一致，实测）。
constexpr int kBlockAPayloadModeId = 0x32;   // 50 [实测：#11 §4.2]
constexpr int kBlockAHeadModeId = 0xfa;      // 250 [实测：#11 §4.2 头节点]
constexpr int kBlockAPayloadFuncNo = 53;     // w2 [推断：BulletModeForId(50)]

// 触发块 A 的载荷节点偏移（#11 §4.2 [实测]）：
//   a = random(1, 360)
//   s8 = (A*0.2 > enemyX) ? ±30 : ±(cos(a)*30)     // A = Enemy+0x630 循环计数（语义推断）
//   s9 = (A*0.1 > enemyY) ? ±30 : ±(sin(a)*30)     // B = Enemy+0x634 循环计数（语义推断）
// 随机量与 fcsel 分支无法静态化为数据 → spawn_x/spawn_y 留「跟随发射者」哨兵，
// 原始表达式记入 offset_expr（人读）。±30 的轴对齐分支同样只在运行期可知。
constexpr float kBlockAOffsetMagnitude = 30.0f;

// 触发块 B（#11 §4.3）：帧条件 frame % 200 ≤ 59 且 frame % 16 == 0。
// frame % 16 == 0 在 200 窗内的落点 = {0,16,32,48}（均 ≤ 59）。
constexpr std::uint32_t kBlockBPeriod = 200;
constexpr std::uint32_t kBlockBOffsets[] = {0, 16, 32, 48};

ChainNode MakeBlockAHeadNode() {
    ChainNode node;
    node.trigger_period = 300;  // frame % 300 == 0 [实测：#11 §4.2]
    node.trigger_tick = 0;
    // 头节点：空载荷（原版 p1[0x28]=0xfa，p1[0x10..0x24]=0）→ count 0 = 不发射。
    node.spawn_command.bullet_type = kBlockAHeadModeId;
    node.spawn_command.count = 0;
    node.provenance = "boss_pattern_tables.md §4.2 触发块 A 头节点 [实测]（#11 解码样板）";
    return node;
}

/**
 * 原版角度（cocos2d，y 轴向上）→ 本项目角度（屏幕系，y 轴向下）。
 *
 * ★ 坐标系必须在这里换一次，否则整屏弹幕会朝反方向飞。
 *
 * 原版是 cocos2d-x：节点的 y 轴**向上**，且
 *   `GameLayer::getAngleToMyShip`（0x535d28）= atan2f(shipY - thisY, shipX - thisX)
 *   速度合成 = (vx, vy) = speed · (cos a, sin a)
 * 两式自洽 ⇒ 同一个 y 语义下弹一定朝自机飞。
 *
 * 本项目的运动学（bullet_motion.cpp 的 IntegrateVelocity / AngleToPoint）用的是
 * **屏幕系**（y 向下，自机在下、boss 在上）。同一个 270° 在两套语义里指向相反：
 *   原版 y-up 的 270°：sin(270°) = -1 ⇒ vy<0 ⇒ y 减小 ⇒ cocos2d 里是屏幕【下方】
 *                      ⇒ 朝自机 ✔
 *   本项目 y-down 的 270°：sin(270°) = -1 ⇒ vy<0 ⇒ y 减小 ⇒ 屏幕【上方】
 *                      ⇒ 背离自机 ✘
 *
 * y 轴翻转等价于角度取负（(vx, vy_up) → (vx, -vy_up) = speed·(cos(-a), sin(-a))），
 * 所以 y-up 角 → y-down 角 = (-a) mod 360。
 *
 * state14 的 `base_angle = 270 + random(-10,10)` 与环形 spawn 的
 * `offset_angle` 都取自**原版静态表**（y-up 语义），进本项目前必须过这个转换。
 */
double FromOriginalAngleDeg(double angle_up_deg) {
    double flipped = -angle_up_deg;
    flipped = std::fmod(flipped, 360.0);
    if (flipped < 0.0) {
        flipped += 360.0;
    }
    return flipped;
}

ChainNode MakeBlockAPayloadNode() {
    ChainNode node;
    node.trigger_period = 300;
    node.trigger_tick = 0;
    node.spawn_command.bullet_type = kBlockAPayloadModeId;  // 原版节点 +0x28 [实测]
    node.spawn_command.count = 1;                            // 原版节点 +0x24 [实测]
    node.spawn_command.spawn_x = kSpawnFollowEmitter;        // 偏移含 random/fcsel，未静态化
    node.spawn_command.spawn_y = kSpawnFollowEmitter;
    // base_angle_deg / spread / 速度：#11 §4.2 未解码 → 未转写（默认 0）。
    node.motion_override.bullet_func_no = kBlockAPayloadFuncNo;  // w2 [推断]
    node.motion_override.bullet_kind = 0;  // 0 = 用 cmd.bullet_type(50)（w3=182 赋值未验证）
    node.offset_expr = "s8/s9 = (A*0.2>enemyX ? ±30 : ±cos(random(1,360))*30) / "
                       "(A*0.1>enemyY ? ±30 : ±sin(random(1,360))*30)";
    node.provenance = "boss_pattern_tables.md §4.2 触发块 A 载荷节点 [实测]（#11 解码样板）；"
                      "#21 自身未逐块解码，周期/门控/id 均取自 #11";
    return node;
}

// 触发块 B 的 4 个触发项（200 窗内 frame%16==0 的落点）。节点字段未能确定 →
// count=0（不发射），仅登记触发时刻与瞄准语义。
ChainNode MakeBlockBNode(std::uint32_t offset) {
    ChainNode node;
    node.trigger_period = kBlockBPeriod;
    node.trigger_tick = offset;
    // 未转写：bullet_type / count / 偏移 / 速度（#11 §4.3 未给出节点字段）→ 全默认值。
    node.spawn_command.count = 0;  // 未能确定 → 占位不发射
    node.aim = AimMode::AtShip;    // getAngleToMyShip [实测：#11 §4.3]
    node.angle_expr = "getAngleToMyShip(enemyX, enemyY)";
    node.offset_expr = "常量 72.0(@0xace230) / double -0.1(@0xace3a0)；节点载荷未能确定";
    node.provenance = "boss_pattern_tables.md §4.3 触发块 B [实测：触发条件与瞄准；"
                      "未能确定：节点 id/count/偏移]（#11 解码样板）";
    return node;
}



// 实测节奏节点（tna_boss21.hpp）：一次触发展开 nodes 个节点、每节点
// count_per_node 发，角度按 angle_step 递减。
ChainNode MakeCadenceNode(const TnaCadenceEntry& entry, std::uint16_t index) {
    ChainNode node;
    node.trigger_period = entry.period;
    node.trigger_tick = entry.phase;
    node.spawn_command.count = static_cast<std::int16_t>(entry.count_per_node);
    // ★ 实测角度来自原版（cocos2d，y 向上）：270° 起每步 −12°，即弹朝屏幕
    //   下方（朝自机）。本项目用屏幕系（y 向下），所以这里换一次坐标。
    node.spawn_command.base_angle_deg = static_cast<float>(
        FromOriginalAngleDeg(TnaBoss21NodeAngleDeg(entry, index)));
    // 弹种与初速来自同一份 .rodata 模板（+0x00 / +0x20）；不填这两项，弹幕会全部
    // 堆在 Boss 点、半径为 0（实测踩过）。
    node.spawn_command.bullet_type = entry.bullet_type;
    node.motion_override.speed = static_cast<double>(entry.speed);
    // 运动函数：原版 `Enemy::createBullets1` @0x37bacc 读 command+0x00（= 本表
    // bullet_type，即节点模式 id），经双跳转表映射出 (w2, w3)，并把这一对作为
    // 参数 2/3 传给每颗子弹的虚初始化（vtable+0x218）[实测：0x37bef0 起]。
    // w2 的取值域（24..59）与 bulletFunc 编号域相容 → 取作运动函数号 [推断]。
    // bullet_type == 0 表示该波模板的弹种字段未定位（4 条 (400,1) 波），
    // 此时不派生（保持 0 = 直线），避免用一个不属于它的函数号改掉运动。
    if (entry.bullet_type > 0) {
        node.motion_override.bullet_func_no = BulletModeForId(entry.bullet_type).w2;
    }
    // 半径：模板里对应 `pf2`（碰撞半径系数）的字段尚未定位，先用引擎直线弹的
    // 默认 4.0px（= 4000 milli，与合成弹幕同口径），保证可判定、可渲染。
    node.motion_override.pf2 = 4.0;
    node.spawn_command.spawn_x = -1.0f;  // 跟随发射者（原版哨兵）
    node.spawn_command.spawn_y = -1.0f;
    node.provenance = "tna_boss21.hpp：节奏点实测（subs+msub）+ 9 次循环 × count=1；"
                      "bullet_func_no 由 BulletModeForId(bullet_type).w2 派生";
    return node;
}

// ---------------------------------------------------------------------------
// state 14 实测波次（rev-nodes 2026-11-08 程序化提取）
//
// 来源：`docs/apk-reverse-notes/patterns/boss21_nodes_full.csv` +
//       `boss21_state14_waves.csv`（192 行展开清单）。
// 4 个门 / 36+36+48+72 个波次，每波 count=1、bullet_type=28
// （bullet_modeid_map.json ranges 27..35 -> func 45 / kind 180）。
//
// ★ 2026-11-08 二次修正（用户实机反馈「样式和内容完全和原版不同」）：
//   旧版把 offset_angle 烘成发射角、f40 取区间中值、spawn 跟随发射者，
//   结果是从 Boss 中心朝 192 个方向齐射 —— 形状根本不是原版的环上扩散。
//   逐指令复核后改回原版语义，静态表只存**确定量**：
//     base_angle = 270 + random(-10,10)          [逐节点独立，运行期]
//     f40       = 0.1*random(0,n) + base        [逐节点独立，运行期]
//     spawn     = boss + (cos(offset)*(20c+80), sin(offset)*(20c+100))
//   其中 offset_angle 是**确定**的环上偏移方向（本表第二列），
//   随机部分交给 ChainRuntimeSlot / ExpandChainNode 的确定性 RNG。
// ---------------------------------------------------------------------------
struct Tna21State14Wave {
    std::uint32_t period;
    double offset_angle_deg;  // 原版 spawn 偏移方向（确定值，实测）
    double speed_base;        // f40 分布下界
    int speed_rand_max;       // f40 = 0.1*U(0,n) + speed_base 的上界
};

// 192 个波次：P=30:36 / P=20:36 / P=10:120
inline constexpr Tna21State14Wave kTna21State14Waves[] = {
    {30, 6.0, 0.5, 6},
    {30, -6.0, 0.5, 6},
    {30, -18.0, 0.5, 6},
    {30, -30.0, 0.5, 6},
    {30, -42.0, 0.5, 6},
    {30, -54.0, 0.5, 6},
    {30, -66.0, 0.5, 6},
    {30, -78.0, 0.5, 6},
    {30, -90.0, 0.5, 6},
    {30, -6.0, 0.5, 6},
    {30, 6.0, 0.5, 6},
    {30, 18.0, 0.5, 6},
    {30, 30.0, 0.5, 6},
    {30, 42.0, 0.5, 6},
    {30, 54.0, 0.5, 6},
    {30, 66.0, 0.5, 6},
    {30, 78.0, 0.5, 6},
    {30, 90.0, 0.5, 6},
    {30, 174.0, 0.5, 6},
    {30, 186.0, 0.5, 6},
    {30, 198.0, 0.5, 6},
    {30, 210.0, 0.5, 6},
    {30, 222.0, 0.5, 6},
    {30, 234.0, 0.5, 6},
    {30, 246.0, 0.5, 6},
    {30, 258.0, 0.5, 6},
    {30, 270.0, 0.5, 6},
    {30, 186.0, 0.5, 6},
    {30, 174.0, 0.5, 6},
    {30, 162.0, 0.5, 6},
    {30, 150.0, 0.5, 6},
    {30, 138.0, 0.5, 6},
    {30, 126.0, 0.5, 6},
    {30, 114.0, 0.5, 6},
    {30, 102.0, 0.5, 6},
    {30, 90.0, 0.5, 6},
    {20, 6.0, 0.8, 6},
    {20, -6.0, 0.8, 6},
    {20, -18.0, 0.8, 6},
    {20, -30.0, 0.8, 6},
    {20, -42.0, 0.8, 6},
    {20, -54.0, 0.8, 6},
    {20, -66.0, 0.8, 6},
    {20, -78.0, 0.8, 6},
    {20, -90.0, 0.8, 6},
    {20, -6.0, 0.8, 6},
    {20, 6.0, 0.8, 6},
    {20, 18.0, 0.8, 6},
    {20, 30.0, 0.8, 6},
    {20, 42.0, 0.8, 6},
    {20, 54.0, 0.8, 6},
    {20, 66.0, 0.8, 6},
    {20, 78.0, 0.8, 6},
    {20, 90.0, 0.8, 6},
    {20, 174.0, 0.8, 6},
    {20, 186.0, 0.8, 6},
    {20, 198.0, 0.8, 6},
    {20, 210.0, 0.8, 6},
    {20, 222.0, 0.8, 6},
    {20, 234.0, 0.8, 6},
    {20, 246.0, 0.8, 6},
    {20, 258.0, 0.8, 6},
    {20, 270.0, 0.8, 6},
    {20, 186.0, 0.8, 6},
    {20, 174.0, 0.8, 6},
    {20, 162.0, 0.8, 6},
    {20, 150.0, 0.8, 6},
    {20, 138.0, 0.8, 6},
    {20, 126.0, 0.8, 6},
    {20, 114.0, 0.8, 6},
    {20, 102.0, 0.8, 6},
    {20, 90.0, 0.8, 6},
    {10, 4.5, 1.2, 8},
    {10, -4.5, 1.2, 8},
    {10, -13.5, 1.2, 8},
    {10, -22.5, 1.2, 8},
    {10, -31.5, 1.2, 8},
    {10, -40.5, 1.2, 8},
    {10, -49.5, 1.2, 8},
    {10, -58.5, 1.2, 8},
    {10, -67.5, 1.2, 8},
    {10, -76.5, 1.2, 8},
    {10, -85.5, 1.2, 8},
    {10, -94.5, 1.2, 8},
    {10, 4.5, 1.2, 8},
    {10, 13.5, 1.2, 8},
    {10, 22.5, 1.2, 8},
    {10, 31.5, 1.2, 8},
    {10, 40.5, 1.2, 8},
    {10, 49.5, 1.2, 8},
    {10, 58.5, 1.2, 8},
    {10, 67.5, 1.2, 8},
    {10, 76.5, 1.2, 8},
    {10, 85.5, 1.2, 8},
    {10, 94.5, 1.2, 8},
    {10, 103.5, 1.2, 8},
    {10, 184.5, 1.2, 8},
    {10, 193.5, 1.2, 8},
    {10, 202.5, 1.2, 8},
    {10, 211.5, 1.2, 8},
    {10, 220.5, 1.2, 8},
    {10, 229.5, 1.2, 8},
    {10, 238.5, 1.2, 8},
    {10, 247.5, 1.2, 8},
    {10, 256.5, 1.2, 8},
    {10, 265.5, 1.2, 8},
    {10, 274.5, 1.2, 8},
    {10, 283.5, 1.2, 8},
    {10, 184.5, 1.2, 8},
    {10, 175.5, 1.2, 8},
    {10, 166.5, 1.2, 8},
    {10, 157.5, 1.2, 8},
    {10, 148.5, 1.2, 8},
    {10, 139.5, 1.2, 8},
    {10, 130.5, 1.2, 8},
    {10, 121.5, 1.2, 8},
    {10, 112.5, 1.2, 8},
    {10, 103.5, 1.2, 8},
    {10, 94.5, 1.2, 8},
    {10, 85.5, 1.2, 8},
    {10, 3.0, 1.5, 10},
    {10, -3.0, 1.5, 10},
    {10, -9.0, 1.5, 10},
    {10, -15.0, 1.5, 10},
    {10, -21.0, 1.5, 10},
    {10, -27.0, 1.5, 10},
    {10, -33.0, 1.5, 10},
    {10, -39.0, 1.5, 10},
    {10, -45.0, 1.5, 10},
    {10, -51.0, 1.5, 10},
    {10, -57.0, 1.5, 10},
    {10, -63.0, 1.5, 10},
    {10, -69.0, 1.5, 10},
    {10, -75.0, 1.5, 10},
    {10, -81.0, 1.5, 10},
    {10, -87.0, 1.5, 10},
    {10, -93.0, 1.5, 10},
    {10, -99.0, 1.5, 10},
    {10, -3.0, 1.5, 10},
    {10, 3.0, 1.5, 10},
    {10, 9.0, 1.5, 10},
    {10, 15.0, 1.5, 10},
    {10, 21.0, 1.5, 10},
    {10, 27.0, 1.5, 10},
    {10, 33.0, 1.5, 10},
    {10, 39.0, 1.5, 10},
    {10, 45.0, 1.5, 10},
    {10, 51.0, 1.5, 10},
    {10, 57.0, 1.5, 10},
    {10, 63.0, 1.5, 10},
    {10, 69.0, 1.5, 10},
    {10, 75.0, 1.5, 10},
    {10, 81.0, 1.5, 10},
    {10, 87.0, 1.5, 10},
    {10, 93.0, 1.5, 10},
    {10, 99.0, 1.5, 10},
    {10, 177.0, 1.5, 10},
    {10, 183.0, 1.5, 10},
    {10, 189.0, 1.5, 10},
    {10, 195.0, 1.5, 10},
    {10, 201.0, 1.5, 10},
    {10, 207.0, 1.5, 10},
    {10, 213.0, 1.5, 10},
    {10, 219.0, 1.5, 10},
    {10, 225.0, 1.5, 10},
    {10, 231.0, 1.5, 10},
    {10, 237.0, 1.5, 10},
    {10, 243.0, 1.5, 10},
    {10, 249.0, 1.5, 10},
    {10, 255.0, 1.5, 10},
    {10, 261.0, 1.5, 10},
    {10, 267.0, 1.5, 10},
    {10, 273.0, 1.5, 10},
    {10, 279.0, 1.5, 10},
    {10, 183.0, 1.5, 10},
    {10, 177.0, 1.5, 10},
    {10, 171.0, 1.5, 10},
    {10, 165.0, 1.5, 10},
    {10, 159.0, 1.5, 10},
    {10, 153.0, 1.5, 10},
    {10, 147.0, 1.5, 10},
    {10, 141.0, 1.5, 10},
    {10, 135.0, 1.5, 10},
    {10, 129.0, 1.5, 10},
    {10, 123.0, 1.5, 10},
    {10, 117.0, 1.5, 10},
    {10, 111.0, 1.5, 10},
    {10, 105.0, 1.5, 10},
    {10, 99.0, 1.5, 10},
    {10, 93.0, 1.5, 10},
    {10, 87.0, 1.5, 10},
    {10, 81.0, 1.5, 10},
};

inline constexpr std::size_t kTna21State14WaveCount =
    sizeof(kTna21State14Waves) / sizeof(kTna21State14Waves[0]);

std::vector<ChainNode> MakeState14Nodes() {
    std::vector<ChainNode> nodes;
    nodes.reserve(kTna21State14WaveCount);
    std::uint32_t salt = 0;
    for (const Tna21State14Wave& wave : kTna21State14Waves) {
        ChainNode node;
        node.trigger_period = wave.period;
        node.trigger_tick = 0;
        node.spawn_command.bullet_type = 28;   // 实测：state 14 全部波次
        node.spawn_command.count = 1;          // 实测
        node.spawn_command.spread_or_speed = 0.0f;
        node.spawn_command.base_angle_deg = 270.0f;  // 名义值，运行期 ±10 覆盖
        node.spawn_command.spawn_x = -1.0f;    // 名义哨兵，运行期环形偏移覆盖
        node.spawn_command.spawn_y = -1.0f;
        node.motion_override.bullet_func_no = 45;   // type 28 -> (45,180)
        node.motion_override.bullet_kind = 180;
        node.motion_override.speed = wave.speed_base;  // 运行期按 f40 分布覆盖
        node.runtime.enabled = true;
        // ★ 原版实测 `270 + random(-10,10)`（cocos2d，y 向上 ⇒ 朝屏幕下方）。
        //   本项目用屏幕系（y 向下），这里换成等价角 90（±10 的抖动对称，
        //   分布不变）。
        node.runtime.angle_nominal_deg = 90.0;
        node.runtime.angle_jitter_deg = 10;
        node.runtime.speed_base = wave.speed_base;
        node.runtime.speed_step = 0.1;
        node.runtime.speed_rand_max = wave.speed_rand_max;
        node.runtime.spawn_offset_angle_deg = wave.offset_angle_deg;
        node.runtime.spawn_radius_x_base = 80.0;   // 20c+80
        node.runtime.spawn_radius_y_base = 100.0;  // 20c+100
        node.runtime.spawn_radius_step = 20.0;
        node.runtime.rng_salt = salt++;
        node.provenance = "boss21_nodes_full.csv state14 [实测]；"
                          "base_angle/f40/spawn 偏移由运行期 RNG 还原";
        nodes.push_back(std::move(node));
    }
    return nodes;
}

std::vector<BossPhaseTable> BuildLibrary() {
    std::vector<BossPhaseTable> library;

    // #11 样板结构（2026-11-08 拆出来）：此前它挂在 boss21 名下，但那些节点
    // 的周期/门控取自 #11 而非 #21，是「布局样板」不是 #21 的真实数据。
    // 现在 boss21 换成 state 14 实测波次，样板单独注册一个 id，
    // `chain_table_tests` 继续钉结构（3 节点块 A + 4 触发项块 B）。
    BossPhaseTable sample;
    sample.boss_id = "boss11_sample";
    sample.phase_id = 0;
    sample.node_size_bytes = 0x40;
    sample.source_node_count = 146;
    sample.nodes.push_back(MakeBlockAHeadNode());
    sample.nodes.push_back(MakeBlockAPayloadNode());
    for (std::uint32_t offset : kBlockBOffsets) {
        sample.nodes.push_back(MakeBlockBNode(offset));
    }
    library.push_back(std::move(sample));

    BossPhaseTable table;
    table.boss_id = "boss21";       // 对应原版 Enemy::setBossBullet21
    table.phase_id = 0;             // 未解码占位（#21 阶段字段逆向表未给出）
    table.node_size_bytes = 0x40;   // 任务书指定假定（#11 实测布局）
    table.source_node_count = 146;  // #21 节点总数 [实测统计]

    // ★ 2026-11-08：改用 state 14 的实测波次（192 个），替换此前基于 #11 样板的
    //   6 个推测节点（那批节点的周期/门控都是 #11 的值，不是 #21 的）。
    //   证据：docs/apk-reverse-notes/patterns/boss21_nodes_full.csv + state14_waves.csv
    table.nodes = MakeState14Nodes();

    library.push_back(std::move(table));

    // 实测节奏表单独一张表（boss_id = "boss21_tna"）：与上面的 #11 抽样样板
    // 互不干扰——样板钉的是「结构」，这张钉的是「实测节奏 + 9 节点循环」。
    BossPhaseTable cadence_table;
    cadence_table.boss_id = "boss21_tna";
    cadence_table.phase_id = 0;
    cadence_table.node_size_bytes = 0x80;
    cadence_table.source_node_count = 146;
    for (const TnaCadenceEntry& entry : kTnaBoss21Cadence) {
        if (entry.count_per_node == 0) {
            continue;  // 未解码条目不发射
        }
        for (std::uint16_t index = 0; index < entry.nodes; ++index) {
            cadence_table.nodes.push_back(MakeCadenceNode(entry, index));
        }
    }
    library.push_back(std::move(cadence_table));

    // state 20 的独立节奏表：与 state 14 互斥，单独注册以便分别 trace 比对。
    BossPhaseTable s20_table;
    s20_table.boss_id = "boss21_tna_s20";
    s20_table.phase_id = 0;
    s20_table.node_size_bytes = 0x80;
    s20_table.source_node_count = 146;
    for (const TnaCadenceEntry& entry : kTnaBoss21CadenceState20) {
        if (entry.count_per_node == 0) {
            continue;
        }
        for (std::uint16_t index = 0; index < entry.nodes; ++index) {
            s20_table.nodes.push_back(MakeCadenceNode(entry, index));
        }
    }
    library.push_back(std::move(s20_table));

    return library;
}

}  // namespace

const std::vector<BossPhaseTable>& BossPhaseLibrary() {
    static const std::vector<BossPhaseTable> kLibrary = BuildLibrary();
    return kLibrary;
}

const BossPhaseTable* FindBossPhaseTable(std::string_view boss_id, std::uint32_t phase_id) {
    for (const BossPhaseTable& table : BossPhaseLibrary()) {
        if (table.boss_id == boss_id && table.phase_id == phase_id) {
            return &table;
        }
    }
    return nullptr;
}

std::vector<const BossPhaseTable*> FindBossPhaseTables(std::string_view boss_id) {
    std::vector<const BossPhaseTable*> out;
    for (const BossPhaseTable& table : BossPhaseLibrary()) {
        if (table.boss_id == boss_id) {
            out.push_back(&table);
        }
    }
    return out;
}

bool HasBossPhaseTable(std::string_view boss_id) {
    return !FindBossPhaseTables(boss_id).empty();
}

namespace {

// 符卡阶段 → 原版 state 段。**整张映射集中在这一处**（校准只改这里）。
//
// 依据（docs/apk-reverse-notes/patterns 与 TNA21_REWRITE_PROPOSAL.md）：
// setBossBullet21 内 state 14（[0x4d6fb8,0x4d89e4)）与 state 20
// （[0x4d9a88,0x4df70c)）是顺序执行的两段。服务端没有原版的 Boss 阶段字段
// （Enemy+0x534，未解码），故用 3 段符卡阶段近似这个进度：
//   phase 0/1（HP 100%~33%）→ state 14 表；phase 2（HP < 33%）→ state 20 表。
// 这是**假设**，不是实测；实测进度推进方式不同的话只改这张表。
struct BossStateSegmentGroup {
    std::string_view base_boss_id;
    std::vector<BossStateSegment> segments;
};

const std::vector<BossStateSegmentGroup>& BossStateSegmentLibrary() {
    static const std::vector<BossStateSegmentGroup> kLibrary = {
        {"boss21_tna", {{"boss21_tna", 0}, {"boss21_tna_s20", 2}}},
        // 默认 boss（boss_id 为空 → EffectiveChainBossId 归一为 "boss21"）此前
        // **没有段表**，于是所有符卡阶段共用同一张 state14 表 —— 实机上每关
        // 弹幕完全一样。这里用引擎既有的阶段机制把三段拆开：
        //   phase 0 → state14 实测 192 波次（环上生成 / 270°±10 / f40 分布）
        //   phase 1 → 9 条实测 cadence（不同节奏、不同弹型）
        //   phase 2 → state20 表（270° 静态角 / type114 / speed2.0）
        // 段表本身是**假设**（原版阶段字段 Enemy+0x534 未解码），集中在此处，
        // 日后校准只改这一行。
        {"boss21", {{"boss21", 0}, {"boss21_tna", 1}, {"boss21_tna_s20", 2}}},
    };
    return kLibrary;
}

}  // namespace

const std::vector<BossStateSegment>& BossStateSegments(std::string_view base_boss_id) {
    static const std::vector<BossStateSegment> kNoSegments;
    for (const BossStateSegmentGroup& group : BossStateSegmentLibrary()) {
        if (group.base_boss_id == base_boss_id) {
            return group.segments;
        }
    }
    return kNoSegments;
}

std::string_view BossStateBossIdForPhase(std::string_view base_boss_id, std::uint32_t spell_phase) {
    const std::vector<BossStateSegment>& segments = BossStateSegments(base_boss_id);
    // 默认 = 原样返回：无段表 / 所有段的 from_phase 都大于 spell_phase 时走这里。
    std::string_view resolved = base_boss_id;
    std::uint32_t best_from_phase = 0;
    bool matched = false;
    for (const BossStateSegment& segment : segments) {
        if (segment.from_phase > spell_phase) {
            continue;
        }
        if (!matched || segment.from_phase >= best_from_phase) {
            best_from_phase = segment.from_phase;
            resolved = segment.boss_id;
            matched = true;
        }
    }
    return resolved;
}

std::string_view EffectiveChainBossId(std::string_view boss_id) {
    return boss_id.empty() ? kDefaultChainBossId : boss_id;
}

bool ChainTablesActive(std::string_view ruleset_version, std::string_view boss_id) {
    if (ruleset_version != kRulesetDanmakuMechanics) {
        return false;
    }
    // 空 boss_id 现在是「默认节点链」，不再直接 false。显式 tian/hui 因库内
    // 没有对应的 ChainNode 表，仍会返回 false → 保留目录里的 10 模式轮转。
    return HasBossPhaseTable(EffectiveChainBossId(boss_id));
}

namespace {

// 单节点展开：触发判定（周期先按 interval_mul 缩放）+ 瞄准覆盖 + EmitRadial。
// triggered=false = 本 tick 未触发（或周期缩放后为 0）；bullets 为空但
// triggered=true = 已触发但 count==0（未转写/头节点，展开 0 颗）。
struct ChainNodeExpansion {
    bool triggered = false;
    std::vector<BulletMotion> bullets;
};

ChainNodeExpansion ExpandChainNode(
    const ChainNode& node,
    std::uint64_t tick,
    const EmitContext& ctx,
    const MotionTargets& targets,
    const DifficultyScale& difficulty
) {
    ChainNodeExpansion out;
    if (node.trigger_period == 0) {
        return out;
    }
    const std::uint32_t period = ScalePeriod(node.trigger_period, difficulty.interval_mul);
    if (period == 0 || tick % period != node.trigger_tick) {
        return out;
    }
    out.triggered = true;
    SpawnCommand cmd = node.spawn_command;
    BulletTemplate tmpl = node.motion_override;
    if (node.aim == AimMode::AtShip && targets.has_my_ship) {
        cmd.base_angle_deg = static_cast<float>(
            AngleToPoint(ctx.emitter_x, ctx.emitter_y, targets.my_ship_x, targets.my_ship_y));
    } else if (node.aim == AimMode::AtEnemy && targets.has_enemy) {
        cmd.base_angle_deg = static_cast<float>(
            AngleToPoint(ctx.emitter_x, ctx.emitter_y, targets.enemy_x, targets.enemy_y));
    }
    // ---- 原版运行期语义 -----------------------------------------------------
    // state14 的每个节点在**每次触发时**重算角度、速度与 spawn 偏移，静态表里
    // 写不出确定值（rev-nodes 2026-11-08 逐指令确认）：
    //   base_angle = 270 + random(-10,10)
    //   f40        = 0.1 * random(0,6|8|10) + {0.5|0.8|1.2|1.5}
    //   spawn      = boss + (cos(offset)*(20c+80), sin(offset)*(20c+100))
    // 这里用 (match_seed, tick, rng_salt) 的确定性 RNG 取样，既还原分布、又让
    // 回放与客户端重建器可复现。旧节点 runtime.enabled=false → 逐字节不变。
    if (node.runtime.enabled) {
        const ChainRuntimeSlot& r = node.runtime;
        DeterministicRng rng(ctx.match_seed, tick, r.rng_salt, 0);
        if (r.angle_jitter_deg != 0) {
            // ★ 原版 `270 + random(-10,10)` 是 cocos2d（y 向上）语义的角度，
            //   必须换成本项目（y 向下）的角度，否则弹全部朝屏幕上方飞、
            //   永远碰不到下方的自机。
            cmd.base_angle_deg = static_cast<float>(
                r.angle_nominal_deg +
                rng.RandomInt(-r.angle_jitter_deg, r.angle_jitter_deg));
        }
        if (r.speed_rand_max > 0) {
            tmpl.speed = r.speed_base + r.speed_step * rng.RandomInt(0, r.speed_rand_max);
        }
        if (r.spawn_radius_x_base != 0.0 || r.spawn_radius_y_base != 0.0) {
            // 原版 c=[x19,#0x68c] 是「波次计数」，但取值域未在逆向表里解开。
            // 若直接取 tick/period，7200 tick 时半径会涨到 14000px（弹全出屏）。
            // 按 state14 的四段子循环把 c 钳到 [0,3]：半径 80~140 / 100~160 px，
            // 与原版的环形扩散形状一致。这里标为**推断**，待逆向确认后修正。
            const double wave = static_cast<double>((tick / period) % 4);
            // ★ 环形 spawn 的 offset_angle 同样取自原版静态表（y 向上语义），
            //   换成 y 向下后再算 sin/cos，环才落在原版的那个方向上。
            const double radians =
                FromOriginalAngleDeg(r.spawn_offset_angle_deg) * 3.14159265358979323846 / 180.0;
            cmd.spawn_x = static_cast<float>(ctx.emitter_x +
                std::cos(radians) * (r.spawn_radius_x_base + r.spawn_radius_step * wave));
            cmd.spawn_y = static_cast<float>(ctx.emitter_y +
                std::sin(radians) * (r.spawn_radius_y_base + r.spawn_radius_step * wave));
        }
    }
    out.bullets = EmitRadial(cmd, tmpl, ctx, difficulty);
    return out;
}

}  // namespace

std::vector<BulletMotion> EmitChainTableTick(
    const BossPhaseTable& table,
    std::uint64_t tick,
    const EmitContext& ctx,
    const MotionTargets& targets,
    const DifficultyScale& difficulty
) {
    std::vector<BulletMotion> out;
    for (const ChainNode& node : table.nodes) {
        const ChainNodeExpansion expanded = ExpandChainNode(node, tick, ctx, targets, difficulty);
        if (!expanded.triggered) {
            continue;
        }
        out.insert(out.end(), expanded.bullets.begin(), expanded.bullets.end());
    }
    return out;
}

std::vector<BulletMotion> EmitChainBossTick(
    std::string_view boss_id,
    std::uint64_t tick,
    const EmitContext& ctx,
    const MotionTargets& targets,
    const DifficultyScale& difficulty
) {
    std::vector<BulletMotion> out;
    for (const ChainNodeEmission& emission :
             EmitChainBossTickDetailed(boss_id, tick, ctx, targets, difficulty)) {
        out.insert(out.end(), emission.bullets.begin(), emission.bullets.end());
    }
    return out;
}

std::vector<ChainNodeEmission> EmitChainBossTickDetailed(
    std::string_view boss_id,
    std::uint64_t tick,
    const EmitContext& ctx,
    const MotionTargets& targets,
    const DifficultyScale& difficulty
) {
    std::vector<ChainNodeEmission> out;
    const std::vector<const BossPhaseTable*> tables = FindBossPhaseTables(boss_id);
    // chain_id = 节点在合并时间线内的平坦序号（库序 + 表内序），跨表连续计数。
    std::uint32_t node_ordinal = 0;
    for (std::size_t table_index = 0; table_index < tables.size(); ++table_index) {
        const BossPhaseTable& table = *tables[table_index];
        for (std::size_t node_id = 0; node_id < table.nodes.size(); ++node_id) {
            const std::uint32_t chain_id = node_ordinal++;
            const ChainNodeExpansion expanded =
                ExpandChainNode(table.nodes[node_id], tick, ctx, targets, difficulty);
            if (!expanded.triggered) {
                continue;
            }
            ChainNodeEmission emission;
            emission.table_index = static_cast<std::uint32_t>(table_index);
            emission.node_id = static_cast<std::uint32_t>(node_id);
            emission.chain_id = chain_id;
            emission.bullets = std::move(expanded.bullets);
            out.push_back(std::move(emission));
        }
    }
    return out;
}

std::size_t ChainTriggerCountAt(const BossPhaseTable& table, std::uint64_t tick) {
    std::size_t count = 0;
    for (const ChainNode& node : table.nodes) {
        if (node.trigger_period == 0) {
            continue;
        }
        if (tick % node.trigger_period == node.trigger_tick) {
            ++count;
        }
    }
    return count;
}

std::vector<const ChainNode*> FlattenChainNodes(const BossPhaseTable& table) {
    std::vector<const ChainNode*> out;
    // 先序递归（children 展开）；表规模小（抽样 ~10 个），递归深度 = 数据深度。
    struct Walk {
        static void Visit(const ChainNode& node, std::vector<const ChainNode*>* out) {
            out->push_back(&node);
            for (const ChainNode& child : node.children) {
                Visit(child, out);
            }
        }
    };
    for (const ChainNode& node : table.nodes) {
        Walk::Visit(node, &out);
    }
    return out;
}

}  // namespace phk::battle::danmaku
