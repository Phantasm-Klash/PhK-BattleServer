cmake_minimum_required(VERSION 3.22)

project(PhKBattleServer
    VERSION 0.1.0
    DESCRIPTION "Open-source C++ battle server core for Phantasm Klash"
    LANGUAGES CXX
)

option(PHK_BATTLE_BUILD_TESTS "Build PhK BattleServer tests" ON)
set(PHK_PROTOCOL_CPP_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../PhK-Protocol/gen/cpp"
    CACHE PATH "Path to generated PhK-Protocol C++ manifest include root"
)

if(NOT EXISTS "${PHK_PROTOCOL_CPP_INCLUDE_DIR}/phk/v1/manifest.hpp")
    message(FATAL_ERROR "Missing PhK-Protocol C++ manifest. Run PhK-Protocol/tools/export_cpp_manifest.py")
endif()

add_library(phk_battle_core
    src/handshake.cpp
    src/kcp_endpoint.cpp
    src/protocol.cpp
    src/result.cpp
    src/server.cpp
    src/simulation.cpp
    src/ticket.cpp
)

target_include_directories(phk_battle_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PHK_PROTOCOL_CPP_INCLUDE_DIR}
)

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

include(CTest)
if(PHK_BATTLE_BUILD_TESTS)
    add_executable(phk_battle_tests
        tests/battle_server_tests.cpp
    )
    target_link_libraries(phk_battle_tests PRIVATE phk_battle_core)
    add_test(NAME phk_battle_tests COMMAND phk_battle_tests)
endif()
