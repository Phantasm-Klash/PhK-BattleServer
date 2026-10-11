// 千夜追忆 1.40 玩家自机射击/炸弹参数表（数据驱动）。
// 由 player_shots_reimpl.hpp 转写；仅用于自研玩法。
#include "phk/battle/danmaku/player_loadout.hpp"

namespace phk::battle::danmaku {
namespace {

const PlayerShotBullet kReimuA1[] = {
    { 0, 90.0, 10.0, -10.0, 5.0 },
    { 0, 90.0, 10.0, 10.0, 5.0 },
};

const PlayerShotBullet kReimuA2[] = {
    { 1, 140.0, 5.0, 0.0, 0.0 },
    { 1, 100.0, 5.0, 0.0, 0.0 },
    { 1, 80.0, 5.0, 0.0, 0.0 },
    { 1, 40.0, 5.0, 0.0, 0.0 },
};

const PlayerShotBullet kReimuB1[] = {
    { 0, 90.0, 10.0, -10.0, 5.0 },
    { 0, 90.0, 10.0, 10.0, 5.0 },
};

const PlayerShotBullet kReimuB2[] = {
    { 2, 90.0, 18.0, -20.0, 5.0 },
    { 2, 90.0, 18.0, 0.0, 5.0 },
    { 2, 90.0, 18.0, 20.0, 5.0 },
    { 2, 90.0, 18.0, -30.0, -5.0 },
    { 2, 90.0, 18.0, -10.0, -5.0 },
    { 2, 90.0, 18.0, 10.0, -5.0 },
    { 2, 90.0, 18.0, 30.0, -5.0 },
};

const PlayerShotBullet kMarisaA1[] = {
    { 3, 90.0, 10.0, -10.0, 5.0 },
    { 3, 90.0, 10.0, 10.0, 5.0 },
};

const PlayerShotBullet kMarisaA2[] = {
    { 4, 90.0, 8.0, 0.0, 0.0 },
    { 6, 90.0, 8.0, 0.0, 0.0 },
    { 7, 90.0, 8.0, 0.0, 0.0 },
    { 5, 90.0, 8.0, 0.0, 0.0 },
};

const PlayerShotBullet kMarisaB1[] = {
    { 8, 90.0, 10.0, -10.0, 5.0 },
    { 8, 90.0, 10.0, 10.0, 5.0 },
};

const PlayerShotBullet kMarisaB2[] = {
    { 9, 90.0, 3.0, 0.0, 0.0 },
    { 9, 90.0, 3.0, 0.0, 0.0 },
    { 9, 90.0, 3.0, 0.0, 0.0 },
    { 9, 90.0, 3.0, 0.0, 0.0 },
};

const PlayerShotBullet kSanaeA1[] = {
    { 10, 90.0, 10.0, 0.0, 0.0 },
    { 10, 90.0, 10.0, 0.0, 0.0 },
    { 14, 90.0, 10.0, 0.0, 0.0 },
    { 14, 90.0, 10.0, 0.0, 0.0 },
    { 12, 90.0, 10.0, 0.0, 0.0 },
    { 12, 90.0, 10.0, 0.0, 0.0 },
    { 13, 90.0, 10.0, 0.0, 0.0 },
    { 13, 90.0, 10.0, 0.0, 0.0 },
    { 11, 90.0, 10.0, 0.0, 0.0 },
    { 11, 90.0, 10.0, 0.0, 0.0 },
    { 15, 90.0, 10.0, 0.0, 0.0 },
    { 15, 90.0, 10.0, 0.0, 0.0 },
};

const PlayerShotBullet kSanaeA2[] = {
    { 16, 90.0, 8.0, -38.0, -25.0 },
    { 16, 90.0, 8.0, -28.0, -25.0 },
    { 16, 90.0, 8.0, -15.0, 5.0 },
    { 16, 90.0, 8.0, -5.0, 5.0 },
    { 16, 90.0, 8.0, 5.0, 5.0 },
    { 16, 90.0, 8.0, 15.0, 5.0 },
    { 16, 90.0, 8.0, 28.0, -25.0 },
    { 16, 90.0, 8.0, 38.0, -25.0 },
};

const PlayerShotBullet kSanaeB1[] = {
    { 10, 90.0, 10.0, 0.0, 0.0 },
    { 10, 90.0, 10.0, 0.0, 0.0 },
    { 14, 90.0, 10.0, 0.0, 0.0 },
    { 14, 90.0, 10.0, 0.0, 0.0 },
    { 12, 90.0, 10.0, 0.0, 0.0 },
    { 12, 90.0, 10.0, 0.0, 0.0 },
    { 13, 90.0, 10.0, 0.0, 0.0 },
    { 13, 90.0, 10.0, 0.0, 0.0 },
    { 11, 90.0, 10.0, 0.0, 0.0 },
    { 11, 90.0, 10.0, 0.0, 0.0 },
    { 15, 90.0, 10.0, 0.0, 0.0 },
    { 15, 90.0, 10.0, 0.0, 0.0 },
};

const PlayerShotBullet kSanaeB2[] = {
    { 17, 111.0, 4.5, -32.0, -5.0 },
    { 17, 100.0, 4.5, -32.0, 5.0 },
    { 17, 90.0, 4.5, -28.0, 0.0 },
    { 17, 90.0, 4.5, -11.0, -20.0 },
    { 17, 90.0, 4.5, 11.0, -20.0 },
    { 17, 90.0, 4.5, 28.0, 0.0 },
    { 17, 80.0, 4.5, 32.0, 5.0 },
    { 17, 69.0, 4.5, 32.0, -5.0 },
};

const PlayerShotBullet kSakuyaA1[] = {
    { 18, 90.0, 10.0, -10.0, 5.0 },
    { 18, 90.0, 10.0, 10.0, 5.0 },
};

const PlayerShotBullet kSakuyaA2[] = {
    { 19, 85.0, 18.0, -20.0, -5.0 },
    { 19, 95.0, 18.0, 20.0, -5.0 },
    { 19, 85.0, 18.0, -30.0, -15.0 },
    { 19, 95.0, 18.0, 30.0, -15.0 },
    { 19, 85.0, 18.0, -40.0, -25.0 },
    { 19, 95.0, 18.0, 40.0, -25.0 },
};

const PlayerShotBullet kSakuyaB1[] = {
    { 18, 90.0, 10.0, -10.0, 5.0 },
    { 18, 90.0, 10.0, 10.0, 5.0 },
};

const PlayerShotBullet kSakuyaB2[] = {
    { 20, 0.0, 18.0, -7.0, 0.0 },
    { 20, 0.0, 18.0, -27.0, 0.0 },
    { 20, 0.0, 18.0, -47.0, -20.0 },
    { 20, 0.0, 18.0, -57.0, -20.0 },
    { 20, 0.0, 18.0, 7.0, 0.0 },
    { 20, 0.0, 18.0, 27.0, 0.0 },
    { 20, 0.0, 18.0, 47.0, -20.0 },
    { 20, 0.0, 18.0, 57.0, -20.0 },
};

const PlayerShotBullet kReisenA1[] = {
    { 21, 90.0, 10.0, -10.0, 5.0 },
    { 21, 90.0, 10.0, 10.0, 5.0 },
};

const PlayerShotBullet kReisenA2[] = {
    { 22, 0.0, 11.0, 0.0, 40.0 },
    { 22, 0.0, 11.0, 0.0, -40.0 },
    { 22, 0.0, 11.0, -40.0, 0.0 },
    { 22, 0.0, 11.0, 40.0, 0.0 },
};

const PlayerShotBullet kReisenB1[] = {
    { 21, 90.0, 10.0, -10.0, 5.0 },
    { 21, 90.0, 10.0, 10.0, 5.0 },
};

const PlayerShotBullet kReisenB2[] = {
    { 23, 90.0, 8.0, -30.0, 0.0 },
    { 23, 90.0, 8.0, -14.0, 30.0 },
    { 23, 90.0, 8.0, 14.0, 30.0 },
    { 23, 90.0, 8.0, 30.0, 0.0 },
};

const PlayerShotBullet kYoumuA1[] = {
    { 24, 90.0, 10.0, -10.0, 5.0 },
    { 24, 90.0, 10.0, 10.0, 5.0 },
};

const PlayerShotBullet kYoumuA2[] = {
    { 25, 90.0, 0.0, 0.0, 0.0 },
    { 26, 90.0, 0.0, 0.0, 0.0 },
    { 27, 90.0, 0.0, 0.0, 0.0 },
    { 28, 90.0, 0.0, 0.0, 0.0 },
    { 29, 90.0, 0.0, 0.0, 0.0 },
    { 30, 90.0, 0.0, 0.0, 0.0 },
    { 31, 90.0, 0.0, 0.0, 0.0 },
    { 32, 90.0, 0.0, 0.0, 0.0 },
};

const PlayerShotBullet kYoumuB1[] = {
    { 33, 90.0, 10.0, -10.0, 5.0 },
    { 33, 90.0, 10.0, 10.0, 5.0 },
};

const PlayerShotBullet kYoumuB2[] = {
    { 34, 90.0, 10.0, 0.0, 0.0 },
    { 34, 90.0, 10.0, 0.0, 0.0 },
    { 34, 90.0, 10.0, 0.0, 0.0 },
    { 34, 75.0, 10.0, 0.0, 0.0 },
    { 34, 105.0, 10.0, 0.0, 0.0 },
};

const PlayerShotBullet kAyaA1[] = {
    { 35, 90.0, 10.0, 10.0, 20.0 },
    { 35, 270.0, 10.0, 10.0, -20.0 },
    { 35, 90.0, 10.0, -10.0, 20.0 },
    { 35, 270.0, 10.0, -10.0, -20.0 },
};

const PlayerShotBullet kAyaA2[] = {
    { 36, 0.0, 6.0, -5.0, 20.0 },
    { 36, 0.0, 6.0, 5.0, 20.0 },
    { 36, 0.0, 6.0, 5.0, 20.0 },
    { 36, 0.0, 6.0, -5.0, -20.0 },
    { 36, 0.0, 6.0, 5.0, -20.0 },
    { 36, 0.0, 6.0, 5.0, 20.0 },
};

const PlayerShotBullet kAyaB1[] = {
    { 37, 90.0, 10.0, 10.0, 20.0 },
    { 37, 90.0, 10.0, -10.0, 20.0 },
};

const PlayerShotBullet kAyaB2[] = {
    { 38, 90.0, 1.0, 0.0, 0.0 },
};

const PlayerShotBullet kSeijaA1[] = {
    { 39, 93.0, 10.0, -10.0, 15.0 },
    { 39, 90.0, 10.0, -6.0, 15.0 },
    { 39, 90.0, 10.0, 6.0, 15.0 },
    { 39, 87.0, 10.0, 10.0, 15.0 },
};

const PlayerShotBullet kSeijaA2[] = {
    { 39, 102.0, 10.0, -15.0, 15.0 },
    { 39, 100.0, 10.0, -12.0, 15.0 },
    { 39, 80.0, 10.0, 12.0, 15.0 },
    { 39, 78.0, 10.0, 15.0, 15.0 },
};

const PlayerShotBullet kSeijaB1[] = {
    { 40, 93.0, 10.0, -10.0, 15.0 },
    { 40, 90.0, 10.0, -6.0, 15.0 },
    { 40, 90.0, 10.0, 6.0, 15.0 },
    { 40, 87.0, 10.0, 10.0, 15.0 },
};

const PlayerShotBullet kSeijaB2[] = {
    { 40, 102.0, 10.0, -15.0, 15.0 },
    { 40, 100.0, 10.0, -12.0, 15.0 },
    { 40, 80.0, 10.0, 12.0, 15.0 },
    { 40, 78.0, 10.0, 15.0, 15.0 },
};

const PlayerShotBullet kCirnoA1[] = {
    { 41, 94.0, 10.0, -10.0, -10.0 },
    { 41, 91.0, 10.0, -5.0, -10.0 },
    { 41, 89.0, 10.0, 5.0, -10.0 },
    { 41, 86.0, 10.0, 10.0, -10.0 },
};

const PlayerShotBullet kCirnoA2[] = {
    { 41, 97.0, 10.0, 0.0, 0.0 },
    { 41, 104.0, 10.0, 0.0, 0.0 },
    { 41, 83.0, 10.0, 0.0, 0.0 },
    { 41, 76.0, 10.0, 0.0, 0.0 },
};

const PlayerShotBullet kCirnoB1[] = {
    { 42, 90.0, 11.0, 10.0, 0.0 },
    { 42, 90.0, 11.0, -10.0, 0.0 },
};

const PlayerShotBullet kCirnoB2[] = {
    { 43, 124.0, 8.0, -15.0, -15.0 },
    { 43, 114.0, 8.0, -10.0, -15.0 },
    { 43, 104.0, 8.0, -5.0, -15.0 },
    { 43, 94.0, 8.0, 0.0, -15.0 },
    { 43, 56.0, 8.0, 15.0, -15.0 },
    { 43, 66.0, 8.0, 10.0, -15.0 },
    { 43, 76.0, 8.0, 5.0, -15.0 },
    { 43, 86.0, 8.0, 0.0, -15.0 },
};

const PlayerShotBullet kYukariA1[] = {
    { 44, 90.0, 0.0, 0.0, 0.0 },
};

const PlayerShotBullet kYukariA2[] = {
    { 45, 86.0, 10.0, 0.0, 0.0 },
    { 45, 90.0, 10.0, 0.0, 0.0 },
    { 45, 94.0, 10.0, 0.0, 0.0 },
    { 46, 90.0, 10.0, 0.0, 0.0 },
    { 46, 93.0, 10.0, 0.0, 0.0 },
    { 46, 87.0, 10.0, 0.0, 0.0 },
    { 46, 90.0, 10.0, 0.0, 0.0 },
};

const PlayerShotBullet kYukariB1[] = {
    { 47, 90.0, 10.0, 0.0, 0.0 },
    { 47, 90.0, 10.0, 0.0, 0.0 },
    { 47, 90.0, 10.0, 0.0, 0.0 },
    { 47, 90.0, 10.0, 0.0, 0.0 },
    { 47, 90.0, 10.0, 0.0, 0.0 },
    { 47, 90.0, 10.0, 0.0, 0.0 },
    { 47, 90.0, 10.0, 0.0, 0.0 },
    { 47, 90.0, 10.0, 0.0, 0.0 },
};

const PlayerShotBullet kAliceA1[] = {
    { 48, 90.0, 8.0, 0.0, 0.0 },
    { 50, 90.0, 8.0, 0.0, 0.0 },
    { 51, 90.0, 8.0, 0.0, 0.0 },
    { 49, 90.0, 8.0, 0.0, 0.0 },
};

const PlayerShotBullet kAliceB1[] = {
    { 3, 90.0, 10.0, -10.0, 5.0 },
    { 3, 90.0, 10.0, 10.0, 5.0 },
};

const PlayerShotBullet kAliceB2[] = {
    { 53, 90.0, 6.0, 0.0, 0.0 },
    { 53, 90.0, 6.0, 0.0, 0.0 },
    { 53, 112.0, 6.0, 0.0, 0.0 },
    { 53, 68.0, 6.0, 0.0, 0.0 },
    { 53, 100.0, 6.0, 0.0, 0.0 },
    { 53, 80.0, 6.0, 0.0, 0.0 },
    { 53, 94.0, 6.0, 0.0, 0.0 },
    { 53, 86.0, 6.0, 0.0, 0.0 },
};

const PlayerShotBullet kRemilliaA1[] = {
    { 54, 90.0, 6.0, 0.0, 0.0 },
    { 54, 90.0, 6.0, 0.0, 0.0 },
};

const PlayerShotBullet kRemilliaA2[] = {
    { 55, 90.0, 10.0, 0.0, 0.0 },
    { 55, 90.0, 10.0, 0.0, 0.0 },
    { 55, 90.0, 10.0, 0.0, 0.0 },
    { 55, 90.0, 10.0, 0.0, 0.0 },
};

const PlayerShotBullet kRemilliaB1[] = {
    { 56, 90.0, 6.0, 0.0, 0.0 },
    { 56, 90.0, 6.0, 0.0, 0.0 },
};

const PlayerShotBullet kRemilliaB2[] = {
    { 57, 92.0, 5.0, 0.0, 0.0 },
    { 57, 88.0, 5.0, 0.0, 0.0 },
    { 57, 84.0, 5.0, 0.0, 0.0 },
    { 57, 96.0, 5.0, 0.0, 0.0 },
};

const PlayerShotBullet kYuyukoA1[] = {
    { 58, 90.0, 4.0, 0.0, 20.0 },
    { 58, 90.0, 4.0, 0.0, -5.0 },
    { 58, 87.0, 4.0, 10.0, -10.0 },
    { 58, 93.0, 4.0, -10.0, -10.0 },
};

const PlayerShotBullet kYuyukoA2[] = {
    { 59, 72.0, 4.0, 0.0, 0.0 },
    { 59, 81.0, 4.0, 0.0, 0.0 },
    { 59, 99.0, 4.0, 0.0, 0.0 },
    { 59, 108.0, 4.0, 0.0, 0.0 },
};

const PlayerShotBullet kYuyukoB1[] = {
    { 60, 0.0, 4.0, 20.0, 0.0 },
    { 60, 102.0, 4.0, -20.0, 0.0 },
    { 60, 72.0, 4.0, 30.0, -20.0 },
    { 60, 108.0, 4.0, -30.0, -20.0 },
};

const PlayerShotBullet kYuyukoB2[] = {
    { 61, 0.0, 4.0, 40.0, 30.0 },
    { 61, 0.0, 4.0, 20.0, -10.0 },
    { 61, 0.0, 4.0, -20.0, -10.0 },
    { 61, 0.0, 4.0, -40.0, 30.0 },
};

const PlayerBombBullet kBombReimu[] = {
    { 62, 90.0, 45 },
    { 62, 0.0, 0 },
};

const PlayerBombBullet kBombMarisa[] = {
    { 64, 0.0, 24 },
    { 64, 0.0, 24 },
    { 64, 0.0, 24 },
    { 64, 0.0, 24 },
    { 64, 0.0, 24 },
    { 64, 0.0, 24 },
    { 63, 88.0, 0 },
    { 63, 92.0, 0 },
    { 64, 90.0, 0 },
};

const PlayerBombBullet kBombSanae[] = {
    { 65, 90.0, 95 },
    { 65, 210.0, 0 },
    { 65, 330.0, 0 },
};

const PlayerBombBullet kBombSakuya[] = {
    { 66, 0.0, 0 },
    { 67, 0.0, 0 },
    { 66, 90.0, 81 },
    { 66, 90.0, 63 },
};

const PlayerBombBullet kBombReisen[] = {
    { 68, 0.0, 0 },
    { 69, 0.0, 0 },
    { 70, 0.0, 33 },
};

const PlayerBombBullet kBombYoumu[] = {
    { 71, 90.0, 36 },
};

const PlayerBombBullet kBombAya[] = {
    { 72, 0.0, 12 },
};

const PlayerBombBullet kBombSeija[] = {
    { 73, 0.0, 67 },
};

const PlayerBombBullet kBombCirno[] = {
    { 74, 90.0, 48 },
    { 75, 90.0, 0 },
};

const PlayerBombBullet kBombYukari[] = {
    { 76, 90.0, 0 },
    { 77, 0.0, 0 },
    { 76, 90.0, 0 },
    { 76, 90.0, 0 },
    { 76, 90.0, 60 },
};

const PlayerBombBullet kBombAlice[] = {
    { 78, 90.0, 24 },
    { 79, 90.0, 24 },
};

const PlayerBombBullet kBombRemillia[] = {
    { 80, 90.0, 0 },
    { 80, 90.0, 0 },
    { 80, 90.0, -1 },
};

const PlayerBombBullet kBombYuyuko[] = {
    { 81, 180.0, 0 },
    { 82, 0.0, 0 },
    { 81, 90.0, 57 },
};

}  // namespace

const std::array<PlayerShotSpec, 52>& PlayerShotTable() {
    static const std::array<PlayerShotSpec, 52> kTable = {{
        { "ReimuA1", "Reimu", 'A', 1, 2, 10.0, 90.0, 90.0, 50, 8, 0, true, kReimuA1, 2 },
        { "ReimuA2", "Reimu", 'A', 2, 4, 5.0, 40.0, 140.0, 65, 12, 1, false, kReimuA2, 4 },
        { "ReimuB1", "Reimu", 'B', 1, 2, 10.0, 90.0, 90.0, 50, 8, 0, true, kReimuB1, 2 },
        { "ReimuB2", "Reimu", 'B', 2, 7, 18.0, 90.0, 90.0, 100, 12, 2, true, kReimuB2, 7 },
        { "MarisaA1", "Marisa", 'A', 1, 2, 10.0, 90.0, 90.0, 13, 8, 3, true, kMarisaA1, 2 },
        { "MarisaA2", "Marisa", 'A', 2, 4, 8.0, 90.0, 90.0, 13, 12, 4, false, kMarisaA2, 4 },
        { "MarisaB1", "Marisa", 'B', 1, 2, 10.0, 90.0, 90.0, 40, 8, 8, true, kMarisaB1, 2 },
        { "MarisaB2", "Marisa", 'B', 2, 4, 3.0, 90.0, 90.0, 40, 6, 9, false, kMarisaB2, 4 },
        { "SanaeA1", "Sanae", 'A', 1, 12, 10.0, 90.0, 90.0, 100, 8, 10, false, kSanaeA1, 12 },
        { "SanaeA2", "Sanae", 'A', 2, 8, 8.0, 90.0, 90.0, 40, 12, 16, true, kSanaeA2, 8 },
        { "SanaeB1", "Sanae", 'B', 1, 12, 10.0, 90.0, 90.0, 100, 8, 10, false, kSanaeB1, 12 },
        { "SanaeB2", "Sanae", 'B', 2, 8, 4.5, 69.0, 111.0, 80, 12, 17, true, kSanaeB2, 8 },
        { "SakuyaA1", "Sakuya", 'A', 1, 2, 10.0, 90.0, 90.0, 100, 8, 18, true, kSakuyaA1, 2 },
        { "SakuyaA2", "Sakuya", 'A', 2, 6, 18.0, 85.0, 95.0, 270, 12, 19, true, kSakuyaA2, 6 },
        { "SakuyaB1", "Sakuya", 'B', 1, 2, 10.0, 90.0, 90.0, 100, 8, 18, true, kSakuyaB1, 2 },
        { "SakuyaB2", "Sakuya", 'B', 2, 8, 18.0, 90.0, 90.0, 68, 12, 20, false, kSakuyaB2, 8 },
        { "ReisenA1", "Reisen", 'A', 1, 2, 10.0, 90.0, 90.0, 20, 8, 21, true, kReisenA1, 2 },
        { "ReisenA2", "Reisen", 'A', 2, 4, 11.0, 90.0, 90.0, 20, 12, 22, false, kReisenA2, 4 },
        { "ReisenB1", "Reisen", 'B', 1, 2, 10.0, 90.0, 90.0, 20, 8, 21, true, kReisenB1, 2 },
        { "ReisenB2", "Reisen", 'B', 2, 4, 8.0, 90.0, 90.0, 20, 12, 23, true, kReisenB2, 4 },
        { "YoumuA1", "Youmu", 'A', 1, 2, 10.0, 90.0, 90.0, 80, 8, 24, true, kYoumuA1, 2 },
        { "YoumuA2", "Youmu", 'A', 2, 8, 0.0, 90.0, 90.0, -1, 12, 25, true, kYoumuA2, 8 },
        { "YoumuB1", "Youmu", 'B', 1, 2, 10.0, 90.0, 90.0, 65, 8, 33, true, kYoumuB1, 2 },
        { "YoumuB2", "Youmu", 'B', 2, 5, 10.0, 75.0, 105.0, 105, 8, 34, false, kYoumuB2, 5 },
        { "AyaA1", "Aya", 'A', 1, 4, 10.0, 0.0, 180.0, 100, 8, 35, true, kAyaA1, 4 },
        { "AyaA2", "Aya", 'A', 2, 6, 6.0, 90.0, 90.0, 140, 12, 36, false, kAyaA2, 6 },
        { "AyaB1", "Aya", 'B', 1, 2, 10.0, 90.0, 90.0, 105, 8, 37, true, kAyaB1, 2 },
        { "AyaB2", "Aya", 'B', 2, 1, 1.0, 90.0, 90.0, -1, 6, 38, false, kAyaB2, 1 },
        { "SeijaA1", "Seija", 'A', 1, 4, 10.0, 87.0, 93.0, -1, 8, 39, true, kSeijaA1, 4 },
        { "SeijaA2", "Seija", 'A', 2, 4, 10.0, 78.0, 102.0, -1, 12, 39, true, kSeijaA2, 4 },
        { "SeijaB1", "Seija", 'B', 1, 4, 10.0, 87.0, 93.0, 65, 8, 40, true, kSeijaB1, 4 },
        { "SeijaB2", "Seija", 'B', 2, 4, 10.0, 78.0, 102.0, 65, 12, 40, true, kSeijaB2, 4 },
        { "CirnoA1", "Cirno", 'A', 1, 4, 10.0, 86.0, 94.0, 105, 8, 41, true, kCirnoA1, 4 },
        { "CirnoA2", "Cirno", 'A', 2, 4, 10.0, 76.0, 104.0, 105, 12, 41, false, kCirnoA2, 4 },
        { "CirnoB1", "Cirno", 'B', 1, 2, 11.0, 90.0, 90.0, 50, 8, 42, true, kCirnoB1, 2 },
        { "CirnoB2", "Cirno", 'B', 2, 8, 8.0, 56.0, 124.0, 160, 12, 43, true, kCirnoB2, 8 },
        { "YukariA1", "Yukari", 'A', 1, 1, 0.0, 90.0, 90.0, -1, 8, 44, true, kYukariA1, 1 },
        { "YukariA2", "Yukari", 'A', 2, 7, 10.0, 86.0, 94.0, 27, 6, 45, false, kYukariA2, 7 },
        { "YukariB1", "Yukari", 'B', 1, 8, 10.0, 90.0, 90.0, 100, 8, 47, false, kYukariB1, 8 },
        { "YukariB2", "Yukari", 'B', 2, 0, 0.0, 90.0, 90.0, -1, 0, -1, true, nullptr, 0 },
        { "AliceA1", "Alice", 'A', 1, 4, 8.0, 90.0, 90.0, 68, 8, 48, false, kAliceA1, 4 },
        { "AliceA2", "Alice", 'A', 2, 0, 0.0, 90.0, 90.0, -1, 0, -1, true, nullptr, 0 },
        { "AliceB1", "Alice", 'B', 1, 2, 10.0, 90.0, 90.0, 13, 8, 3, true, kAliceB1, 2 },
        { "AliceB2", "Alice", 'B', 2, 8, 6.0, 67.0, 113.0, 70, 12, 53, false, kAliceB2, 8 },
        { "RemilliaA1", "Remillia", 'A', 1, 2, 6.0, 90.0, 90.0, 68, 8, 54, true, kRemilliaA1, 2 },
        { "RemilliaA2", "Remillia", 'A', 2, 4, 10.0, 90.0, 90.0, 170, 12, 55, false, kRemilliaA2, 4 },
        { "RemilliaB1", "Remillia", 'B', 1, 2, 6.0, 90.0, 90.0, 140, 8, 56, true, kRemilliaB1, 2 },
        { "RemilliaB2", "Remillia", 'B', 2, 4, 5.0, 86.0, 94.0, 170, 12, 57, false, kRemilliaB2, 4 },
        { "YuyukoA1", "Yuyuko", 'A', 1, 4, 4.0, 87.0, 93.0, 110, 8, 58, true, kYuyukoA1, 4 },
        { "YuyukoA2", "Yuyuko", 'A', 2, 4, 4.0, 72.0, 108.0, 163, 12, 59, false, kYuyukoA2, 4 },
        { "YuyukoB1", "Yuyuko", 'B', 1, 4, 4.0, 72.0, 108.0, 163, 8, 60, false, kYuyukoB1, 4 },
        { "YuyukoB2", "Yuyuko", 'B', 2, 4, 4.0, 90.0, 90.0, 250, 12, 61, false, kYuyukoB2, 4 },
    }};
    return kTable;
}

const std::array<PlayerBombSpec, 13>& PlayerBombTable() {
    static const std::array<PlayerBombSpec, 13> kTable = {{
        { "Reimu", 320, 440, 45, 62, "mybomb_reimu_01", kBombReimu, 2 },
        { "Marisa", 200, 320, 24, 64, "mybomb_marisa_02", kBombMarisa, 9 },
        { "Sanae", 250, 370, 95, 65, "mybomb_sanae_01", kBombSanae, 3 },
        { "Sakuya", 200, 320, 81, 66, "mybomb_sakuya_01", kBombSakuya, 4 },
        { "Reisen", 400, 520, 33, 70, "mybomb_reisen_03", kBombReisen, 3 },
        { "Youmu", 260, 380, 36, 71, "mybomb_youmu_01", kBombYoumu, 1 },
        { "Aya", 400, 520, 12, 72, "mybomb_aya_01", kBombAya, 1 },
        { "Seija", 200, 320, 67, 73, "mybomb_seija_01", kBombSeija, 1 },
        { "Cirno", 300, 420, 48, 74, "mybomb_cirno_01", kBombCirno, 2 },
        { "Yukari", 200, 440, 60, 76, "mybomb_yukari_01", kBombYukari, 5 },
        { "Alice", 200, 320, 24, 78, "mybomb_alice_01", kBombAlice, 2 },
        { "Remillia", 200, 320, -1, 80, "mybomb_remillia_01", kBombRemillia, 3 },
        { "Yuyuko", 300, 420, 57, 81, "mybomb_yuyuko_01", kBombYuyuko, 3 },
    }};
    return kTable;
}

int PlayerBulletDamage(int type) {
    switch (type) {
        case 0: return 50;
        case 1: return 65;
        case 2: return 100;
        case 3: return 13;
        case 4: return 13;
        case 5: return 100;
        case 6: return 13;
        case 7: return 13;
        case 8: return 40;
        case 9: return 40;
        case 10: return 100;
        case 11: return 100;
        case 12: return 100;
        case 13: return 100;
        case 14: return 40;
        case 15: return 40;
        case 16: return 40;
        case 17: return 80;
        case 18: return 100;
        case 19: return 270;
        case 20: return 68;
        case 21: return 20;
        case 22: return 20;
        case 23: return 20;
        case 24: return 80;
        case 25: return -1;
        case 26: return 50;
        case 27: return 50;
        case 28: return 65;
        case 29: return 65;
        case 30: return 105;
        case 31: return 65;
        case 32: return 110;
        case 33: return 65;
        case 34: return 105;
        case 35: return 100;
        case 36: return 140;
        case 37: return 105;
        case 38: return -1;
        case 39: return -1;
        case 40: return 65;
        case 41: return 105;
        case 42: return 50;
        case 43: return 160;
        case 44: return -1;
        case 45: return 27;
        case 46: return -1;
        case 47: return 100;
        case 48: return 68;
        case 49: return 84;
        case 50: return 84;
        case 51: return 84;
        case 52: return 50;
        case 53: return 70;
        case 54: return 68;
        case 55: return 170;
        case 56: return 140;
        case 57: return 170;
        case 58: return 110;
        case 59: return 163;
        case 60: return 163;
        case 61: return 250;
        case 83: return 110;
        default: return -1;
    }
}

const char* PlayerBulletTexture(int type) {
    switch (type) {
        case 0: return "myshot_reimuA_11";
        case 1: return "myshot_reimuA_21";
        case 2: return "myshot_reimuB_11";
        case 3: return "myshot_marisaA_11";
        case 4: return "myshot_marisaA_21";
        case 5: return "myshot_marisaA_22";
        case 6: return "myshot_marisaA_23";
        case 7: return "myshot_marisaA_24";
        case 8: return "myshot_marisaB_11";
        case 9: return "myshot_marisaB_21";
        case 10: return "myshot_sanaeA_11";
        case 11: return "myshot_sanaeA_12";
        case 12: return "myshot_sanaeA_13";
        case 13: return "myshot_sanaeA_14";
        case 14: return "myshot_sanaeA_15";
        case 15: return "myshot_sanaeA_16";
        case 16: return "myshot_sanaeA_21";
        case 17: return "myshot_sanaeB_11";
        case 18: return "myshot_sakuyaA_11";
        case 19: return "myshot_sakuyaA_21";
        case 20: return "myshot_sakuyaB_11";
        case 21: return "myshot_reisenA_11";
        case 22: return "myshot_reisenA_21";
        case 23: return "myshot_reisenB_11";
        case 24: return "myshot_youmuA_11";
        case 25: return "myshot_youmuA_21";
        case 26: return "myshot_youmuA_22";
        case 27: return "myshot_youmuA_23";
        case 28: return "myshot_youmuA_24";
        case 29: return "myshot_youmuA_25";
        case 30: return "myshot_youmuA_26";
        case 31: return "myshot_youmuA_27";
        case 32: return "myshot_youmuA_28";
        case 33: return "myshot_youmuA_11";
        case 34: return "myshot_youmuB_11";
        case 35: return "myshot_ayaA_11";
        case 36: return "myshot_ayaA_21";
        case 37: return "myshot_ayaB_11";
        case 38: return "myshot_ayaB_21";
        case 39: return "myshot_seijaA_11";
        case 40: return "myshot_seijaB_11";
        case 41: return "myshot_cirnoA_11";
        case 42: return "myshot_cirnoB_11";
        case 43: return "myshot_cirnoB_21";
        case 44: return "myshot_yukariA_11";
        case 45: return "myshot_yukariA_12";
        case 46: return "myshot_yukariA_21";
        case 47: return "myshot_yukariB_11";
        case 48: return "myshot_aliceA_11";
        case 49: return "myshot_aliceA_12";
        case 50: return "myshot_aliceA_13";
        case 51: return "myshot_aliceA_14";
        case 52: return "myshot_aliceB_11";
        case 53: return "myshot_aliceB_21";
        case 54: return "myshot_remiliaA_11";
        case 55: return "myshot_remiliaA_21";
        case 56: return "myshot_remiliaB_11";
        case 57: return "myshot_remiliaB_21";
        case 58: return "myshot_yuyukoA_11";
        case 59: return "myshot_yuyukoA_21";
        case 60: return "myshot_yuyukoB_11";
        case 61: return "myshot_yuyukoB_21";
        case 83: return "player_graze";
        default: return "";
    }
}

const char* PlayerBombTexture(int type) {
    switch (type) {
        case 62: return "mybomb_reimu_01";
        case 63: return "mybomb_marisa_01";
        case 64: return "mybomb_marisa_02";
        case 65: return "mybomb_sanae_01";
        case 66: return "mybomb_sakuya_01";
        case 67: return "mybomb_sakuya_02";
        case 68: return "mybomb_reisen_01";
        case 69: return "mybomb_reisen_02";
        case 70: return "mybomb_reisen_03";
        case 71: return "mybomb_youmu_01";
        case 72: return "mybomb_aya_01";
        case 73: return "mybomb_seija_01";
        case 74: return "mybomb_cirno_01";
        case 75: return "mybomb_cirno_02";
        case 76: return "mybomb_yukari_01";
        case 77: return "mybomb_yukari_02";
        case 78: return "mybomb_alice_01";
        case 79: return "mybomb_alice_01";
        case 80: return "mybomb_remillia_01";
        case 81: return "mybomb_yuyuko_01";
        case 82: return "mybomb_yuyuko_02";
        default: return "";
    }
}

}  // namespace phk::battle::danmaku
