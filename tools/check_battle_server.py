#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent


REQUIRED = [
    "CMakeLists.txt",
    "README.md",
    "docs/architecture.md",
    "dev/progress.md",
    "include/phk/battle/version.hpp",
    "include/phk/battle/ticket.hpp",
    "include/phk/battle/handshake.hpp",
    "include/phk/battle/kcp_endpoint.hpp",
    "include/phk/battle/protocol.hpp",
    "include/phk/battle/result.hpp",
    "include/phk/battle/server.hpp",
    "include/phk/battle/simulation.hpp",
    "src/ticket.cpp",
    "src/handshake.cpp",
    "src/kcp_endpoint.cpp",
    "src/protocol.cpp",
    "src/result.cpp",
    "src/server.cpp",
    "src/simulation.cpp",
    "apps/phk_battle_server/main.cpp",
    "tests/battle_server_tests.cpp",
]


REQUIRED_CPP_MANIFEST_FIELDS = {
    "BattleTicket": ["match_id", "player_id", "ruleset_version", "expires_at_ms"],
    "BattlePacketHeader": ["match_id", "player_id", "tick", "seq", "ack", "payload_type", "nonce"],
    "BattleInput": ["match_id", "player_id", "tick", "seq", "direction_bits", "slow", "shoot", "bomb", "card_slot"],
    "BattleResult": ["match_id", "mode_id", "result_hash", "replay_id"],
}


def run(command: list[str], cwd: pathlib.Path) -> None:
    print("+", " ".join(command))
    subprocess.run(command, cwd=str(cwd), check=True)


def is_multi_config_generator(build_dir: pathlib.Path) -> bool:
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return False

    for line in cache.read_text(encoding="utf-8", errors="ignore").splitlines():
        if line.startswith("CMAKE_CONFIGURATION_TYPES:"):
            return True
    return False


def descriptor_messages(descriptor: dict) -> dict[str, dict]:
    messages: dict[str, dict] = {}
    for proto_file in descriptor.get("files", []):
        for message in proto_file.get("messages", []):
            messages[str(message.get("name", ""))] = message
    return messages


def field_names(message: dict) -> set[str]:
    return {str(field.get("name", "")) for field in message.get("fields", [])}


def check_protocol_descriptor() -> bool:
    descriptor_path = WORKSPACE / "PhK-Protocol" / "descriptors" / "phk_v1_descriptor.json"
    if not descriptor_path.exists():
        print("PhK-Protocol descriptor is missing; run tools/export_descriptor.py", file=sys.stderr)
        return False
    descriptor = json.loads(descriptor_path.read_text(encoding="utf-8"))
    if descriptor.get("descriptor_version") != "0.1.0-draft":
        print("unsupported PhK-Protocol descriptor version", file=sys.stderr)
        return False
    messages = descriptor_messages(descriptor)
    for message_name in [
        "BattleTicket",
        "SignedBattleTicket",
        "BattleHandshakeHello",
        "BattleHandshakeAccept",
        "BattlePacketHeader",
        "BattleInput",
        "BattleResult",
        "SignedBattleResult",
    ]:
        if message_name not in messages:
            print(f"descriptor missing {message_name}", file=sys.stderr)
            return False
    ticket_fields = field_names(messages["BattleTicket"])
    for field in ["match_id", "player_id", "mode_id", "battle_server_id", "endpoint", "deck_snapshot_hash", "ruleset_version", "expires_at_ms"]:
        if field not in ticket_fields:
            print(f"descriptor BattleTicket missing {field}", file=sys.stderr)
            return False
    header_fields = field_names(messages["BattlePacketHeader"])
    for field in ["match_id", "player_id", "tick", "seq", "ack", "payload_type", "key_id", "nonce"]:
        if field not in header_fields:
            print(f"descriptor BattlePacketHeader missing {field}", file=sys.stderr)
            return False
    return True


def check_cpp_manifest() -> bool:
    manifest_path = WORKSPACE / "PhK-Protocol" / "gen" / "cpp" / "phk" / "v1" / "manifest.hpp"
    if not manifest_path.exists():
        print("PhK-Protocol C++ manifest is missing; run tools/export_cpp_manifest.py", file=sys.stderr)
        return False
    manifest = manifest_path.read_text(encoding="utf-8")
    descriptor_path = WORKSPACE / "PhK-Protocol" / "descriptors" / "phk_v1_descriptor.json"
    descriptor = json.loads(descriptor_path.read_text(encoding="utf-8"))
    for constant, key in [
        ("kDescriptorVersion", "descriptor_version"),
        ("kBattleApiVersion", "battle_api_version"),
        ("kRulesetVersion", "ruleset_version"),
        ("kRulesetHash", "ruleset_hash"),
    ]:
        value = str(descriptor.get(key, ""))
        if f'{constant} = "{value}"' not in manifest:
            print(f"C++ manifest {constant} is out of sync", file=sys.stderr)
            return False
    if f"kProtocolVersion = {int(descriptor.get('protocol_version', 0))}" not in manifest:
        print("C++ manifest protocol version is out of sync", file=sys.stderr)
        return False
    for message_name, fields in REQUIRED_CPP_MANIFEST_FIELDS.items():
        for field in fields:
            if f'{{"{message_name}", "{field}"}}' not in manifest:
                print(f"C++ manifest missing {message_name}.{field}", file=sys.stderr)
                return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", action="store_true", help="Configure, build, and run CTest.")
    parser.add_argument("--config", default="Debug", help="CTest/build configuration for multi-config generators.")
    args = parser.parse_args()

    missing = [path for path in REQUIRED if not (ROOT / path).exists()]
    if missing:
        print("missing files:", ", ".join(missing), file=sys.stderr)
        return 1

    protocol_battle = WORKSPACE / "PhK-Protocol" / "proto" / "phk" / "v1" / "battle.proto"
    protocol_matchmaking = WORKSPACE / "PhK-Protocol" / "proto" / "phk" / "v1" / "matchmaking.proto"
    if not protocol_battle.exists() or not protocol_matchmaking.exists():
        print("PhK-Protocol battle/matchmaking proto files are missing", file=sys.stderr)
        return 1
    if not check_protocol_descriptor():
        return 1
    if not check_cpp_manifest():
        return 1

    ticket_text = (ROOT / "include" / "phk" / "battle" / "ticket.hpp").read_text(encoding="utf-8")
    if "SignedBattleTicket" not in ticket_text or "TicketVerifier" not in ticket_text:
        print("ticket boundary missing SignedBattleTicket/TicketVerifier", file=sys.stderr)
        return 1

    handshake_text = (ROOT / "include" / "phk" / "battle" / "handshake.hpp").read_text(encoding="utf-8")
    if "BattleHandshakeHello" not in handshake_text or "BattleHandshakeAccept" not in handshake_text:
        print("handshake boundary missing hello/accept", file=sys.stderr)
        return 1

    result_text = (ROOT / "include" / "phk" / "battle" / "result.hpp").read_text(encoding="utf-8")
    if "SignedBattleResult" not in result_text or "BattleResultVerifier" not in result_text:
        print("result boundary missing SignedBattleResult/BattleResultVerifier", file=sys.stderr)
        return 1

    version_text = (ROOT / "include" / "phk" / "battle" / "version.hpp").read_text(encoding="utf-8")
    if "phk/v1/manifest.hpp" not in version_text or "phk::v1::kRulesetVersion" not in version_text:
        print("version boundary is not wired to PhK-Protocol C++ manifest", file=sys.stderr)
        return 1

    simulation_text = (ROOT / "include" / "phk" / "battle" / "simulation.hpp").read_text(encoding="utf-8")
    if "kBattleTickRateHz = 60" not in simulation_text or "BattleSimulation" not in simulation_text or "ReplaySummary" not in simulation_text:
        print("simulation boundary missing fixed tick, simulation, or replay summary", file=sys.stderr)
        return 1

    simulation_impl = (ROOT / "src" / "simulation.cpp").read_text(encoding="utf-8")
    if "CanonicalStateHash" not in simulation_impl or "input_tick_too_far_ahead" not in simulation_impl:
        print("simulation implementation missing canonical hash or authoritative input validation", file=sys.stderr)
        return 1

    if args.build:
        build_dir = ROOT / "build"
        run(["cmake", "-S", str(ROOT), "-B", str(build_dir)], ROOT)
        run(["cmake", "--build", str(build_dir), "--config", args.config], ROOT)
        ctest_command = ["ctest", "--test-dir", str(build_dir), "--output-on-failure"]
        if is_multi_config_generator(build_dir):
            ctest_command.extend(["-C", args.config])
        run(ctest_command, ROOT)

    print("check_battle_server ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
