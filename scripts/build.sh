#!/usr/bin/env bash
#
# Build the PhK-BattleServer C++ battle server on Linux (e.g. the gateway host).
#
# Usage:
#   scripts/build.sh [--no-test] [--debug] [--release] [-- <extra cmake args>]
#
# Environment overrides:
#   BUILD_DIR    output directory   (default: <repo>/build-linux)
#   BUILD_TYPE   CMake build type   (default: RelWithDebInfo)
#   GENERATOR    CMake generator    (default: Ninja)
#   BUILD_TESTS  build unit tests   (default: ON)
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT}/build-linux}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
GENERATOR="${GENERATOR:-Ninja}"
BUILD_TESTS="${BUILD_TESTS:-ON}"
RUN_TESTS=1
EXTRA_ARGS=()

usage() {
  cat <<'EOF'
Build the PhK-BattleServer C++ battle server.

Usage:
  scripts/build.sh [--no-test] [--debug] [--release] [-- <extra cmake args>]

Options:
  --no-test    configure and build, but skip ctest
  --debug      set CMAKE_BUILD_TYPE=Debug
  --release    set CMAKE_BUILD_TYPE=Release
  -h, --help   show this help

Environment:
  BUILD_DIR    output directory   (default: <repo>/build-linux)
  BUILD_TYPE   CMake build type   (default: RelWithDebInfo)
  GENERATOR    CMake generator    (default: Ninja)
  BUILD_TESTS  build unit tests   (default: ON)
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --no-test) RUN_TESTS=0 ;;
    --debug) BUILD_TYPE="Debug" ;;
    --release) BUILD_TYPE="Release" ;;
    --help|-h) usage; exit 0 ;;
    --) shift; EXTRA_ARGS=("$@"); break ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

command -v cmake >/dev/null 2>&1 || { echo "missing required tool: cmake" >&2; exit 1; }
if [ "${GENERATOR}" = "Ninja" ]; then
  command -v ninja >/dev/null 2>&1 || { echo "missing required tool: ninja" >&2; exit 1; }
fi

echo "==> PhK-BattleServer build"
echo "    root      : ${ROOT}"
echo "    build dir : ${BUILD_DIR}"
echo "    type      : ${BUILD_TYPE}"
echo "    generator : ${GENERATOR}"
echo "    tests     : ${BUILD_TESTS}"

cmake -S "${ROOT}" -B "${BUILD_DIR}" -G "${GENERATOR}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DPHK_BATTLE_BUILD_TESTS="${BUILD_TESTS}" \
  ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}

cmake --build "${BUILD_DIR}" --parallel

if [ "${RUN_TESTS}" -eq 1 ] && [ "${BUILD_TESTS}" = "ON" ]; then
  ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

echo "==> done"
echo "    server binary : ${BUILD_DIR}/phk_battle_server"
echo "    core library  : ${BUILD_DIR}/libphk_battle_core.a"
