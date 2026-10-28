// TNA Boss21 弹幕节奏表单测：验证实测的 (period, phase) 判定与弹数统计。
//
// 表来源见 include/phk/battle/danmaku/tna_boss21.hpp 的头注释（capstone 实测）。
#include <cstdint>
#include <iostream>
#include <string>

#include "phk/battle/danmaku/tna_boss21.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& label) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << label << "\n";
    }
}

using phk::battle::danmaku::kTnaBoss21Cadence;
using phk::battle::danmaku::TnaBoss21BulletsAt;
using phk::battle::danmaku::TnaBoss21BulletsInRange;
using phk::battle::danmaku::TnaBoss21FiringCount;
using phk::battle::danmaku::TnaCadenceEntry;
using phk::battle::danmaku::TnaCadenceFires;

void TestTableShape() {
    Check(kTnaBoss21Cadence.size() == 11, "table: 11 measured cadence points");
    std::uint32_t p300 = 0;
    std::uint32_t p400 = 0;
    for (const TnaCadenceEntry& entry : kTnaBoss21Cadence) {
        if (entry.period == 300 && entry.phase == 1) ++p300;
        if (entry.period == 400 && entry.phase == 1) ++p400;
    }
    Check(p300 == 4, "table: four (300,1) points");
    Check(p400 == 4, "table: four (400,1) points");
}

void TestFiringRule() {
    const TnaCadenceEntry p400{400u, 1u, 180, 0.0f, 2.0f, 0u};
    Check(!TnaCadenceFires(p400, 0), "fire: tick 0 is before the phase");
    Check(TnaCadenceFires(p400, 1), "fire: tick 1 fires");
    Check(!TnaCadenceFires(p400, 2), "fire: tick 2 does not");
    Check(TnaCadenceFires(p400, 401), "fire: tick 401 fires");
    Check(TnaCadenceFires(p400, 801), "fire: tick 801 fires");

    const TnaCadenceEntry p112{112u, 91u, 0, 0.0f, 0.0f, 0u};
    Check(!TnaCadenceFires(p112, 90), "fire: phase 91 not reached at 90");
    Check(TnaCadenceFires(p112, 91), "fire: tick 91 fires");
    Check(TnaCadenceFires(p112, 203), "fire: tick 91+112 fires");
    Check(!TnaCadenceFires(p112, 204), "fire: tick 204 does not");
}

void TestBulletCounts() {
    // tick 1：三个 180 发 + 一个 1 发 = 541
    Check(TnaBoss21BulletsAt(1) == 541u, "count: tick 1 emits 541");
    // tick 2：只可能命中未解码条目 → 0
    Check(TnaBoss21BulletsAt(2) == 0u, "count: tick 2 emits nothing (undecoded entries skipped)");
    // tick 401：同样 541
    Check(TnaBoss21BulletsAt(401) == 541u, "count: tick 401 emits 541");
    // tick 301：300 周期条目 count=0 → 0
    Check(TnaBoss21BulletsAt(301) == 0u, "count: tick 301 emits nothing");

    // 0..1200 累计：tick 1 / 401 / 801 三次 × 541
    Check(TnaBoss21BulletsInRange(0, 1200) == 541u * 3u, "range: 0..1200 emits 1623");
    Check(TnaBoss21FiringCount(1) == 4, "firing: tick 1 fires four entries");
}

void TestDensityAgainstSynthetic() {
    // 合成 10 模式：每玩家每 450 tick 74 发（12/7/4/2/8/6/5/10/4/16）。
    // TNA 节奏表在同样 450 tick 内：tick 1 / 401 → 2 次 × 541 = 1082 发。
    const std::uint64_t tna = TnaBoss21BulletsInRange(0, 450);
    const std::uint64_t synthetic = 74;
    Check(tna == 541u * 2u, "density: 450 ticks emit 1082 with the measured cadence");
    Check(tna > synthetic * 10u, "density: measured cadence is >10x the synthetic rotation");
}

}  // namespace

int main() {
    TestTableShape();
    TestFiringRule();
    TestBulletCounts();
    TestDensityAgainstSynthetic();

    if (g_failures != 0) {
        std::cerr << g_failures << " tna cadence test(s) failed\n";
        return 1;
    }
    std::cout << "tna cadence tests passed\n";
    return 0;
}
