cmake_minimum_required(VERSION 3.22)

project(PhKBattleServer
    VERSION 0.1.0
    DESCRIPTION "Open-source C++ battle server core for Phantasm Klash"
    LANGUAGES C CXX
)

option(PHK_BATTLE_BUILD_TESTS "Build PhK BattleServer tests" ON)

# Resolve the generated PhK-Protocol C++ manifest include root.
#
# Priority:
#   1. Explicit override (-DPHK_PROTOCOL_CPP_INCLUDE_OVERRIDE=<path>).
#   2. Monorepo sibling ../PhK-Protocol/gen/cpp (canonical, always fresh).
#   3. Vendored copy third_party/phk-protocol/gen/cpp (self-contained; used by
#      GitHub Actions and any consumer without a PhK-Protocol checkout).
set(PHK_PROTOCOL_CPP_INCLUDE_OVERRIDE "" CACHE PATH
    "Optional explicit path to the generated PhK-Protocol C++ manifest include root"
)

set(_phk_protocol_candidates "")
if(PHK_PROTOCOL_CPP_INCLUDE_OVERRIDE)
    list(APPEND _phk_protocol_candidates "${PHK_PROTOCOL_CPP_INCLUDE_OVERRIDE}")
endif()
list(APPEND _phk_protocol_candidates
    "${CMAKE_CURRENT_SOURCE_DIR}/../PhK-Protocol/gen/cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/phk-protocol/gen/cpp"
)

set(PHK_PROTOCOL_CPP_INCLUDE_DIR "")
foreach(_phk_candidate IN LISTS _phk_protocol_candidates)
    if(EXISTS "${_phk_candidate}/phk/v1/manifest.hpp")
        set(PHK_PROTOCOL_CPP_INCLUDE_DIR "${_phk_candidate}")
        break()
    endif()
endforeach()

if(NOT PHK_PROTOCOL_CPP_INCLUDE_DIR)
    message(FATAL_ERROR
        "Missing PhK-Protocol C++ manifest (phk/v1/manifest.hpp). "
        "Checked: ${_phk_protocol_candidates}. "
        "Run PhK-Protocol/tools/export_cpp_manifest.py, or re-vendor "
        "third_party/phk-protocol/gen/cpp.")
endif()
message(STATUS "PhK-Protocol C++ manifest: ${PHK_PROTOCOL_CPP_INCLUDE_DIR}")

# .dem v1 container uses zlib for the body (COORDINATION §0.2).
find_package(ZLIB REQUIRED)

# Battle input AEAD (docs/battle-wire-scheme.md §P2). The cipher comes from
# OpenSSL rather than a hand-rolled ChaCha20-Poly1305: writing your own AEAD is
# where this kind of code goes wrong, and the gateway has OpenSSL 3.0 with
# headers and pkg-config. Only libcrypto is needed.
#
# NOTE: `PhK-BattleServer/.github/workflows/protocol-audit.yml` compiles a few
# sources with a hand-written g++ line; that line needs -lcrypto added whenever
# `src/aead.cpp` or `src/protocol.cpp` is in it.
find_package(OpenSSL REQUIRED)

add_library(phk_battle_core
    src/aead.cpp
    src/card_effects.cpp
    src/card_events_report.cpp
    src/card_hand.cpp
    src/coop_boss.cpp
    src/chain_log.cpp
    src/dem.cpp
    src/boss_race.cpp
    src/danmaku/boss_catalog.cpp
    src/danmaku/bullet_mode_map.cpp
    src/danmaku/bullet_motion.cpp
    src/danmaku/chain_node.cpp
    src/danmaku/chain_split.cpp
    src/danmaku/emitter.cpp
    src/danmaku/judgment.cpp
    src/danmaku/pattern_timeline.cpp
    src/danmaku/player_loadout.cpp
    src/handshake.cpp
    src/kcp_endpoint.cpp
    src/kcp_server_endpoint.cpp
    src/lobby_rng.cpp
    src/match_lifecycle.cpp
    src/protocol.cpp
    src/result.cpp
    src/server.cpp
    src/simulation.cpp
    src/ticket.cpp
    third_party/ikcp/ikcp.c
)

target_include_directories(phk_battle_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ikcp
        ${PHK_PROTOCOL_CPP_INCLUDE_DIR}
)

target_link_libraries(phk_battle_core PUBLIC ZLIB::ZLIB OpenSSL::Crypto)

# Vendored third-party C code: keep it warning-free from our own -Wall flags.
set_source_files_properties(third_party/ikcp/ikcp.c PROPERTIES COMPILE_OPTIONS "-w")

target_compile_features(phk_battle_core PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(phk_battle_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(phk_battle_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(phk_battle_server
    apps/phk_battle_server/main.cpp
)
target_link_libraries(phk_battle_server PRIVATE phk_battle_core)

# 单关 APK 对齐验收（计划 §4.1）的 trace 导出工具：纯模拟、无网络，逐 tick
# 写快照 JSONL。刻意不注册 ctest——它是产出验收数据的工具，不是测试。
add_executable(phk_danmaku_trace
    apps/phk_danmaku_trace/main.cpp
)
target_link_libraries(phk_danmaku_trace PRIVATE phk_battle_core)

# 数理差分测试台 C++ 侧：读共享场景跑生产运动学，输出逐 tick 状态。
# 与 SpellKard/laya/dev/motion_diff.mjs（TS 镜像）交 compare.py 比对。
add_executable(phk_motion_diff
    apps/phk_motion_diff/main.cpp
)
target_link_libraries(phk_motion_diff PRIVATE phk_battle_core)

include(CTest)
if(PHK_BATTLE_BUILD_TESTS)
    add_executable(phk_battle_tests
        tests/battle_server_tests.cpp
    )
    target_link_libraries(phk_battle_tests PRIVATE phk_battle_core)
    add_test(NAME phk_battle_tests COMMAND phk_battle_tests)

    add_executable(phk_boss_race_tests
        tests/boss_race_tests.cpp
    )
    target_link_libraries(phk_boss_race_tests PRIVATE phk_battle_core)
    add_test(NAME phk_boss_race_tests COMMAND phk_boss_race_tests)

    add_executable(phk_danmaku_tests
        tests/danmaku_tests.cpp
    )
    target_link_libraries(phk_danmaku_tests PRIVATE phk_battle_core)
    add_test(NAME phk_danmaku_tests COMMAND phk_danmaku_tests)

    add_executable(phk_danmaku_chain_tests
        tests/danmaku_chain_tests.cpp
    )
    target_link_libraries(phk_danmaku_chain_tests PRIVATE phk_battle_core)
    add_test(NAME phk_danmaku_chain_tests COMMAND phk_danmaku_chain_tests)

    add_executable(phk_battle_lifecycle_tests
        tests/battle_server_lifecycle_tests.cpp
    )
    target_link_libraries(phk_battle_lifecycle_tests PRIVATE phk_battle_core)
    add_test(NAME phk_battle_lifecycle_tests COMMAND phk_battle_lifecycle_tests)

    add_executable(phk_lobby_rng_tests
        tests/lobby_rng_tests.cpp
    )
    target_link_libraries(phk_lobby_rng_tests PRIVATE phk_battle_core)
    add_test(NAME phk_lobby_rng_tests COMMAND phk_lobby_rng_tests)

    add_executable(phk_card_effects_tests
        tests/card_effects_tests.cpp
    )
    target_link_libraries(phk_card_effects_tests PRIVATE phk_battle_core)
    add_test(NAME phk_card_effects_tests COMMAND phk_card_effects_tests)

    add_executable(phk_coop_boss_tests
        tests/coop_boss_tests.cpp
    )
    target_link_libraries(phk_coop_boss_tests PRIVATE phk_battle_core)
    add_test(NAME phk_coop_boss_tests COMMAND phk_coop_boss_tests)

    add_executable(phk_card_hand_tests
        tests/card_hand_tests.cpp
    )
    target_link_libraries(phk_card_hand_tests PRIVATE phk_battle_core)
    add_test(NAME phk_card_hand_tests COMMAND phk_card_hand_tests)

    add_executable(phk_card_cast_execution_tests
        tests/card_cast_execution_tests.cpp
    )
    target_link_libraries(phk_card_cast_execution_tests PRIVATE phk_battle_core)
    add_test(NAME phk_card_cast_execution_tests COMMAND phk_card_cast_execution_tests)

    add_executable(phk_boss_race_card_tests
        tests/boss_race_card_tests.cpp
    )
    target_link_libraries(phk_boss_race_card_tests PRIVATE phk_battle_core)
    add_test(NAME phk_boss_race_card_tests COMMAND phk_boss_race_card_tests)

    add_executable(phk_chain_log_tests
        tests/chain_log_tests.cpp
    )
    target_link_libraries(phk_chain_log_tests PRIVATE phk_battle_core)
    add_test(NAME phk_chain_log_tests COMMAND phk_chain_log_tests)

    add_executable(phk_dem_tests
        tests/dem_tests.cpp
    )
    target_link_libraries(phk_dem_tests PRIVATE phk_battle_core)
    add_test(NAME phk_dem_tests COMMAND phk_dem_tests)

    # ChainNode 表路径的发射数量/时刻钉帧（P3 验收）。
    add_executable(phk_chain_table_tests
        tests/chain_table_tests.cpp
    )
    target_link_libraries(phk_chain_table_tests PRIVATE phk_battle_core)
    add_test(NAME phk_chain_table_tests COMMAND phk_chain_table_tests)

    # 难度缩放钉帧（P4 验收）：默认 ruleset 恒等 + 密度/间隔通道。
    add_executable(phk_difficulty_scale_tests
        tests/difficulty_scale_tests.cpp
    )
    target_link_libraries(phk_difficulty_scale_tests PRIVATE phk_battle_core)
    add_test(NAME phk_difficulty_scale_tests COMMAND phk_difficulty_scale_tests)

    # 局内计分（TNA GameData 形状）：擦弹/符卡/击破/结算 + score 不进 state hash。
    add_executable(phk_score_tests
        tests/score_tests.cpp
    )
    target_link_libraries(phk_score_tests PRIVATE phk_battle_core)
    add_test(NAME phk_score_tests COMMAND phk_score_tests)

    add_executable(phk_tna_cadence_tests
        tests/tna_cadence_tests.cpp
    )
    target_link_libraries(phk_tna_cadence_tests PRIVATE phk_battle_core)
    add_test(NAME phk_tna_cadence_tests COMMAND phk_tna_cadence_tests)

    add_executable(phk_motion_sheet apps/phk_motion_sheet/main.cpp)
    target_link_libraries(phk_motion_sheet PRIVATE phk_battle_core)

    # 计分系统独立验证（V1）：不变量/无双计/受击作废/默认 ruleset/hash/
    # 结算一次性/时间边界/wire 四字段/自机乘区。
    # 输入面 AEAD（P2）：RFC 8439 官方向量 + 信封严格解析 + 每条校验分支。
    add_executable(phk_aead_tests
        tests/aead_tests.cpp
    )
    target_link_libraries(phk_aead_tests PRIVATE phk_battle_core)
    add_test(NAME phk_aead_tests COMMAND phk_aead_tests)
endif()
