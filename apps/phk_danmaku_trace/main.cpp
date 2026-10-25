// 单关 APK 对齐验收（计划 §4.1）的 trace 导出工具。
//
// 无网络、无大厅、无对局服——只跑 BossRaceSimulation 本身：固定 seed + 固定
// 输入脚本，逐 tick 调用 Tick()，用**生产快照编码器** `EncodeBossRaceSnapshotPayload`
// 把每 tick 的快照 JSON 写成一行（JSONL）。这就是客户端解析的同一种 wire 形状，
// 因此 `SpellKard/laya/dev/stage_parity.mjs` 可以直接消费本工具的产出。
//
// 用法：
//   phk_danmaku_trace --out trace.jsonl [--seed 12345] [--ticks 300]
//                      [--ruleset tna-1.40-danmaku] [--boss-hp 6000]
//                      [--boss-id tian] [--difficulty 1]
//
// --difficulty：0/1/2/3 = easy/normal/hard/lunatic（默认 1），透传
// BossRaceConfig.difficulty；只在 tna-1.40-danmaku 下由服务端权威缩放，
// 默认 ruleset 只回显不缩放。
//
// 输入脚本（确定性，无随机）：两名玩家全程 shoot；p-alpha 走方形走位（每 32 tick
// 换向）并在 tick 100 放一次 bomb；p-beta 每 64 tick 换向。移动/射击都服务端计算，
// 与真实客户端行为一致。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "phk/battle/boss_race.hpp"
#include "phk/battle/match_lifecycle.hpp"

namespace {

bool ParseArg(int argc, char** argv, const char* name, std::string* out) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            *out = argv[i + 1];
            return true;
        }
    }
    return false;
}

std::uint64_t ParseU64(const std::string& text, std::uint64_t fallback) {
    if (text.empty()) {
        return fallback;
    }
    try {
        return static_cast<std::uint64_t>(std::stoull(text));
    } catch (...) {
        return fallback;
    }
}

std::int32_t ParseI32(const std::string& text, std::int32_t fallback) {
    if (text.empty()) {
        return fallback;
    }
    try {
        return static_cast<std::int32_t>(std::stoll(text));
    } catch (...) {
        return fallback;
    }
}

phk::battle::BattleInput MakeScriptedInput(
    const std::string& player_id,
    std::uint64_t tick,
    bool shoot,
    bool bomb
) {
    phk::battle::BattleInput input;
    input.match_id = "match_trace_000001";
    input.player_id = player_id;
    input.tick = tick;
    input.seq = tick;
    input.shoot = shoot;
    input.bomb = bomb;
    // 确定性走位：每 32 tick 换一个方向（右→下→左→上循环）。
    const std::uint32_t phase = static_cast<std::uint32_t>((tick / 32) % 4);
    switch (phase) {
        case 0: input.direction_bits = 0x2; break;  // 右
        case 1: input.direction_bits = 0x4; break;  // 下
        case 2: input.direction_bits = 0x8; break;  // 左
        default: input.direction_bits = 0x1; break;  // 上
    }
    return input;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_path;
    if (!ParseArg(argc, argv, "--out", &out_path) || out_path.empty()) {
        std::fprintf(stderr, "usage: phk_danmaku_trace --out <path> [--seed N] [--ticks N] "
                             "[--ruleset v] [--boss-hp N] [--boss-id id] [--difficulty N]\n");
        return 2;
    }
    std::string seed_text;
    std::string ticks_text;
    std::string ruleset;
    std::string boss_hp_text;
    std::string boss_id;
    std::string difficulty_text;
    ParseArg(argc, argv, "--seed", &seed_text);
    ParseArg(argc, argv, "--ticks", &ticks_text);
    ParseArg(argc, argv, "--ruleset", &ruleset);
    ParseArg(argc, argv, "--boss-hp", &boss_hp_text);
    ParseArg(argc, argv, "--boss-id", &boss_id);
    ParseArg(argc, argv, "--difficulty", &difficulty_text);

    const std::uint64_t seed = ParseU64(seed_text, 12345);
    const std::uint64_t ticks = ParseU64(ticks_text, 300);
    const std::uint64_t boss_hp = ParseU64(boss_hp_text, 6000);
    // 难度：默认 1（normal）。解析失败/越界交给服务端 DifficultyScaleFor 回落 normal。
    const std::int32_t difficulty = ParseI32(difficulty_text, 1);

    phk::battle::BossRaceConfig config;
    config.match_id = "match_trace_000001";
    config.match_seed = seed;
    config.boss_max_hp = boss_hp;
    config.ruleset_version = ruleset.empty() ? "tna-1.40-danmaku" : ruleset;
    config.boss_id = boss_id;
    config.difficulty = difficulty;

    phk::battle::BossRaceSimulation simulation(config);
    simulation.AddPlayer("p-alpha", 0, 240000);
    simulation.AddPlayer("p-beta", 0, -240000);
    simulation.SetPlayerConnected("p-alpha", true);
    simulation.SetPlayerConnected("p-beta", true);

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "cannot open %s\n", out_path.c_str());
        return 1;
    }

    // chains[] 统计（验收用）：tna-1.40-danmaku + 库内 boss_id 时应出现
    // 真实 node_id / rng_cursor（非 P1 占位 node_id=0 且 rng_cursor=0）。
    std::uint64_t ticks_with_chains = 0;
    std::uint64_t max_chains_per_tick = 0;
    std::uint64_t first_chain_tick = 0;
    std::uint64_t real_chain_records = 0;
    std::uint64_t total_chain_records = 0;

    for (std::uint64_t tick = 1; tick <= ticks; ++tick) {
        simulation.SubmitInput(MakeScriptedInput("p-alpha", tick, true, tick == 100));
        simulation.SubmitInput(MakeScriptedInput("p-beta", tick, true, false));
        const phk::battle::BossRaceSnapshot snapshot = simulation.Tick();
        if (!snapshot.chains.empty()) {
            ++ticks_with_chains;
            if (first_chain_tick == 0) {
                first_chain_tick = tick;
            }
            if (snapshot.chains.size() > max_chains_per_tick) {
                max_chains_per_tick = snapshot.chains.size();
            }
            for (const phk::battle::BossChainWire& chain : snapshot.chains) {
                ++total_chain_records;
                if (chain.node_id != 0 || chain.rng_cursor != 0) {
                    ++real_chain_records;
                }
            }
        }
        const std::vector<std::uint8_t> payload =
            phk::battle::EncodeBossRaceSnapshotPayload(snapshot);
        if (payload.empty()) {
            std::fprintf(stderr, "empty payload at tick %llu\n",
                         static_cast<unsigned long long>(tick));
            return 1;
        }
        // payload[0] = BattlePayloadType；其余是快照 JSON。
        out.write(reinterpret_cast<const char*>(payload.data() + 1),
                  static_cast<std::streamsize>(payload.size() - 1));
        out.put('\n');
        if (simulation.State() == phk::battle::BossRaceState::Finished) {
            std::fprintf(stdout, "match finished at tick %llu (winner %s)\n",
                         static_cast<unsigned long long>(tick),
                         simulation.WinnerPlayerId().c_str());
            break;
        }
    }
    out.flush();
    if (!out) {
        std::fprintf(stderr, "write failed\n");
        return 1;
    }

    const phk::battle::BossRaceSnapshot last = simulation.Snapshot();
    std::fprintf(stdout, "trace written: %s (ticks=%llu, bullets=%zu, state_hash=%s)\n",
                 out_path.c_str(),
                 static_cast<unsigned long long>(simulation.CurrentTick()),
                 last.bullets.size(),
                 last.state_hash.c_str());
    std::fprintf(stdout, "chains: ticks_with_chains=%llu max_per_tick=%llu first_tick=%llu "
                         "records=%llu real=%llu\n",
                 static_cast<unsigned long long>(ticks_with_chains),
                 static_cast<unsigned long long>(max_chains_per_tick),
                 static_cast<unsigned long long>(first_chain_tick),
                 static_cast<unsigned long long>(total_chain_records),
                 static_cast<unsigned long long>(real_chain_records));
    return 0;
}
