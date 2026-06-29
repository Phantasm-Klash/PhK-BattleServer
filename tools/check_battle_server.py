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
    "SignedBattleTicket": ["ticket", "signature_alg", "signature", "key_id"],
    "BattleHandshakeHello": ["battle_ticket", "client_x25519_pub", "client_random", "supported_aead"],
    "BattleHandshakeAccept": ["match_id", "player_id", "server_x25519_pub", "server_random", "selected_aead", "kcp_conv", "key_id", "transcript_hash", "server_signature"],
    "BattlePacketHeader": ["match_id", "player_id", "tick", "seq", "ack", "payload_type", "nonce"],
    "BattleEncryptedPacket": ["header", "ciphertext", "auth_tag"],
    "BattleInput": ["match_id", "player_id", "tick", "seq", "direction_bits", "slow", "shoot", "bomb", "card_slot"],
    "BattleModeAction": ["match_id", "player_id", "tick", "seq", "action_id", "action_type", "client_result_authoritative"],
    "BattleSnapshot": ["match_id", "snapshot_tick", "state_hash", "event_cursor", "players", "bullets_delta", "mode_state"],
    "BattleEvent": ["match_id", "tick", "cursor", "type", "server_authoritative"],
    "BattleResult": ["match_id", "mode_id", "result_hash", "replay_id", "reward_projection_json", "mode_result_json"],
    "SignedBattleResult": ["result", "signature_alg", "signature", "key_id"],
    "ReplayInputStreamSummary": [
        "replay_id",
        "owner_user_id",
        "match_id",
        "input_count",
        "event_count",
        "input_stream_hash",
        "event_stream_hash",
        "final_state_hash",
        "final_tick",
    ],
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
    if "require_projection_only_reward" not in result_text:
        print("result boundary missing projection-only reward guard option", file=sys.stderr)
        return 1

    version_text = (ROOT / "include" / "phk" / "battle" / "version.hpp").read_text(encoding="utf-8")
    if "phk/v1/manifest.hpp" not in version_text or "phk::v1::kRulesetVersion" not in version_text:
        print("version boundary is not wired to PhK-Protocol C++ manifest", file=sys.stderr)
        return 1

    simulation_text = (ROOT / "include" / "phk" / "battle" / "simulation.hpp").read_text(encoding="utf-8")
    if (
        "kBattleTickRateHz = 60" not in simulation_text
        or "BattleSimulation" not in simulation_text
        or "ReplaySummary" not in simulation_text
        or "ReplayFixture" not in simulation_text
        or "ReplayInputStreamSummaryRecord" not in simulation_text
        or "BuildReplayInputStreamSummary" not in simulation_text
        or "BuildReplayFixture" not in simulation_text
        or "CanonicalReplayInputStreamSummaryRecord" not in simulation_text
        or "DevResultHashFromReplaySummary" not in simulation_text
        or "AcceptModeAction" not in simulation_text
        or "pending_mode_actions_by_tick_" not in simulation_text
        or "ApplyModeActionsForTick" not in simulation_text
        or "mode_id" not in simulation_text
        or "ruleset_version" not in simulation_text
        or "SetPlayerConnected" not in simulation_text
        or "ReconnectSnapshot" not in simulation_text
        or "max_seq_ahead" not in simulation_text
        or "fallback_input_count" not in simulation_text
        or "input_trace" not in simulation_text
        or "event_trace" not in simulation_text
    ):
        print("simulation boundary missing fixed tick, simulation, replay fixture/summary, mode/ruleset, reconnect snapshot, seq window, fallback audit, replay trace, or mode action acceptance", file=sys.stderr)
        return 1

    simulation_impl = (ROOT / "src" / "simulation.cpp").read_text(encoding="utf-8")
    if (
        "CanonicalStateHash" not in simulation_impl
        or "BuildReplayInputStreamSummary" not in simulation_impl
        or "BuildReplayFixture" not in simulation_impl
        or "CanonicalReplayInputStreamSummaryRecord" not in simulation_impl
        or "DevResultHashFromReplaySummary" not in simulation_impl
        or "DevReplayIdFromReplaySummary" not in simulation_impl
        or 'Snapshot("replay_final")' not in simulation_impl
        or "input_tick_duplicate" not in simulation_impl
        or "input_tick_too_far_ahead" not in simulation_impl
        or "mode_action_client_result_forbidden" not in simulation_impl
        or "mode_action_type_unsupported" not in simulation_impl
        or "player_disconnected" not in simulation_impl
        or "seq_too_far_ahead" not in simulation_impl
        or "event_cursor_ahead" not in simulation_impl
        or 'snapshot.mode_state["missed_event_count"]' not in simulation_impl
        or 'snapshot.mode_state["fallback_input_count"]' not in simulation_impl
        or 'snapshot.mode_state["mode_action_count"]' not in simulation_impl
        or "AccumulateFallbackInput" not in simulation_impl
        or "mode_action_count_" not in simulation_impl
        or "pending_mode_actions_by_tick_[action.tick].push_back(action)" not in simulation_impl
        or "ApplyModeActionsForTick(tick_to_apply)" not in simulation_impl
        or 'snapshot.mode_state["mode_id"]' not in simulation_impl
        or 'snapshot.mode_state["ruleset_version"]' not in simulation_impl
        or "input_trace_.push_back" not in simulation_impl
        or "event_trace_.push_back" not in simulation_impl
        or "summary.input_trace = input_trace_" not in simulation_impl
        or "fixture.input_trace = fixture.summary.input_trace" not in simulation_impl
        or "fixture.replay_summary_record = BuildReplayInputStreamSummary" not in simulation_impl
        or "record.owner_user_id" not in simulation_impl
        or "HashAppend(hash, item)" not in simulation_impl
    ):
        print("simulation implementation missing canonical hash, replay fixture material, mode/ruleset projection, reconnect, fallback/mode-action replay audit, replay trace hashing, or authoritative input/mode-action validation", file=sys.stderr)
        return 1

    server_impl = (ROOT / "src" / "server.cpp").read_text(encoding="utf-8")
    if (
        "match_mode_ruleset_mismatch" not in server_impl
        or "SessionExistsForPlayer" not in server_impl
        or "options.required_ruleset_version" not in server_impl
        or "options.required_result_hash" not in server_impl
        or "options.required_event_cursor" not in server_impl
        or "ReconnectSnapshot" not in server_impl
        or "DispatchEncrypted" not in server_impl
        or "session_key_mismatch" not in server_impl
        or "encrypted_ack_ahead" not in server_impl
        or "encrypted_event_cursor_ahead" not in server_impl
        or "encrypted_tick_too_old" not in server_impl
        or "encrypted_tick_too_far_ahead" not in server_impl
        or "match_full" not in server_impl
        or "ticket_not_registered" not in server_impl
        or "BuildSignedBattleResult" not in server_impl
        or "CanonicalBattleResultPayload" not in server_impl
        or "DevResultHashFromReplaySummary" not in server_impl
        or "DevReplayIdFromReplaySummary" not in server_impl
        or "projection_only" not in server_impl
        or "fallback_input_count" not in server_impl
        or "mode_action_count" not in server_impl
    ):
        print("server implementation missing mode/ruleset, capacity, handshake, encrypted session, encrypted tick/event-cursor window, fallback/mode-action-bound signed-result callback, or registered-player authority guards", file=sys.stderr)
        return 1

    result_impl = (ROOT / "src" / "result.cpp").read_text(encoding="utf-8")
    if (
        "ruleset_version_mismatch" not in result_impl
        or "reward_projection_json" not in result_impl
        or "result_hash_mismatch" not in result_impl
        or "replay_id_mismatch" not in result_impl
        or "event_cursor_mismatch" not in result_impl
        or "final_tick_mismatch" not in result_impl
        or "input_count_mismatch" not in result_impl
        or "mode_action_count_mismatch" not in result_impl
        or "input_trace_count_mismatch" not in result_impl
        or "event_trace_count_mismatch" not in result_impl
        or "reward_projection_mutation_forbidden" not in result_impl
    ):
        print("result boundary missing ruleset/hash/replay/cursor/tick/count/trace verification or projection-only result shape", file=sys.stderr)
        return 1

    protocol_text = (ROOT / "include" / "phk" / "battle" / "protocol.hpp").read_text(encoding="utf-8")
    if "BattleEncryptedPacket" not in protocol_text or "DispatchEncrypted" not in protocol_text:
        print("protocol boundary missing encrypted packet adapter shape", file=sys.stderr)
        return 1

    protocol_impl = (ROOT / "src" / "protocol.cpp").read_text(encoding="utf-8")
    if (
        "key_id_missing" not in protocol_impl
        or "nonce_invalid" not in protocol_impl
        or "payload_type_missing" not in protocol_impl
        or "ciphertext_missing" not in protocol_impl
        or "auth_tag_invalid" not in protocol_impl
        or "HasExpectedAeadNonceShape" not in protocol_impl
        or "nonce_replay" not in protocol_impl
        or "encrypted_payload_type_invalid" not in protocol_impl
    ):
        print("protocol dispatcher missing encrypted packet key/nonce/ciphertext/tag/payload/replay shape guards", file=sys.stderr)
        return 1

    handshake_text = (ROOT / "include" / "phk" / "battle" / "handshake.hpp").read_text(encoding="utf-8")
    handshake_impl = (ROOT / "src" / "handshake.cpp").read_text(encoding="utf-8")
    if (
        "client_key_missing" not in handshake_impl
        or "client_random_missing" not in handshake_impl
        or "aead_unsupported" not in handshake_impl
        or "client_to_server_key_ref" not in handshake_text
        or "server_to_client_key_ref" not in handshake_text
        or "server_signature_hex" not in handshake_text
        or "DevHandshakeKeyRef" not in handshake_impl
        or "DevHandshakeServerSignature" not in handshake_impl
    ):
        print("handshake boundary missing client key/random/aead checks, dev key refs, or transcript signature material", file=sys.stderr)
        return 1

    tests_text = (ROOT / "tests" / "battle_server_tests.cpp").read_text(encoding="utf-8")
    if (
        "MakeAuthoritativeReplay60Config" not in tests_text
        or "DriveAuthoritativeReplay60Ticks" not in tests_text
        or "fnv64:183370bd6f8c18e7" not in tests_text
        or "fnv64:7c13fa803ae1b2dd" not in tests_text
        or "sha256:dev-fnv64-eb5d3d3884abf76a" not in tests_text
        or "fnv64:a0b383d4a7be0bf7" not in tests_text
        or "fnv64:8049946f03724f36" not in tests_text
        or "sha256:dev-fnv64-a7519545ad65902e" not in tests_text
        or "CanonicalReplayInputStreamSummaryRecord(summary_record) ==" not in tests_text
        or "CanonicalBattleResultPayload(built.signed_result.result)" not in tests_text
        or "sha256:dev-fnv64-7cd25aafda3bc356" not in tests_text
    ):
        print("battle server tests missing pinned 60Hz replay/result boundary fingerprints", file=sys.stderr)
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
