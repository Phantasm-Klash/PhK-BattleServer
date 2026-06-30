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


def descriptor_enums(descriptor: dict) -> dict[str, dict]:
    enums: dict[str, dict] = {}
    for proto_file in descriptor.get("files", []):
        for enum in proto_file.get("enums", []):
            enums[str(enum.get("name", ""))] = enum
    return enums


def field_names(message: dict) -> set[str]:
    return {str(field.get("name", "")) for field in message.get("fields", [])}


def field_by_name(message: dict, name: str) -> dict:
    for field in message.get("fields", []):
        if field.get("name") == name:
            return field
    return {}


def check_descriptor_field_shape(
    messages: dict[str, dict],
    message_name: str,
    field_name: str,
    field_type: str,
    field_number: int,
    repeated: bool = False,
) -> bool:
    message = messages.get(message_name, {})
    field = field_by_name(message, field_name)
    if not field:
        print(f"descriptor missing {message_name}.{field_name}", file=sys.stderr)
        return False
    if str(field.get("type", "")) != field_type:
        print(
            f"descriptor {message_name}.{field_name} expected type {field_type}, "
            f"got {field.get('type')}",
            file=sys.stderr,
        )
        return False
    if int(field.get("number", 0)) != field_number:
        print(
            f"descriptor {message_name}.{field_name} expected number {field_number}, "
            f"got {field.get('number')}",
            file=sys.stderr,
        )
        return False
    if bool(field.get("repeated", False)) != repeated:
        print(
            f"descriptor {message_name}.{field_name} repeated flag mismatch",
            file=sys.stderr,
        )
        return False
    return True


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
    enums = descriptor_enums(descriptor)
    payload_type_enum = enums.get("BattlePayloadType")
    if payload_type_enum is None:
        print("descriptor missing BattlePayloadType enum", file=sys.stderr)
        return False
    expected_payload_values = {
        "BATTLE_PAYLOAD_TYPE_UNSPECIFIED": 0,
        "BATTLE_PAYLOAD_TYPE_HANDSHAKE_HELLO": 1,
        "BATTLE_PAYLOAD_TYPE_HANDSHAKE_ACCEPT": 2,
        "BATTLE_PAYLOAD_TYPE_INPUT": 3,
        "BATTLE_PAYLOAD_TYPE_SNAPSHOT": 4,
        "BATTLE_PAYLOAD_TYPE_EVENT": 5,
        "BATTLE_PAYLOAD_TYPE_PING": 6,
        "BATTLE_PAYLOAD_TYPE_RECONNECT": 7,
        "BATTLE_PAYLOAD_TYPE_RESULT": 8,
        "BATTLE_PAYLOAD_TYPE_MODE_ACTION": 9,
    }
    actual_payload_values = {
        str(value.get("name", "")): int(value.get("number", -1))
        for value in payload_type_enum.get("values", [])
    }
    for name, number in expected_payload_values.items():
        if actual_payload_values.get(name) != number:
            print(f"descriptor BattlePayloadType {name} expected {number}", file=sys.stderr)
            return False
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
    required_field_shapes = [
        ("BattlePacketHeader", "payload_type", "BattlePayloadType", 7, False),
        ("BattlePacketHeader", "nonce", "bytes", 9, False),
        ("BattleEncryptedPacket", "ciphertext", "bytes", 2, False),
        ("BattleEncryptedPacket", "auth_tag", "bytes", 3, False),
        ("BattleHandshakeHello", "client_x25519_pub", "bytes", 3, False),
        ("BattleHandshakeHello", "client_random", "bytes", 4, False),
        ("BattleHandshakeHello", "supported_aead", "string", 5, True),
        ("BattleHandshakeAccept", "server_x25519_pub", "bytes", 4, False),
        ("BattleHandshakeAccept", "server_random", "bytes", 5, False),
        ("BattleHandshakeAccept", "transcript_hash", "bytes", 9, False),
        ("BattleHandshakeAccept", "server_signature", "SignedBlob", 10, False),
        ("BattleModeAction", "payload_json", "bytes", 8, False),
        ("BattleModeAction", "client_result_authoritative", "bool", 9, False),
        ("BattleResult", "reward_projection_json", "bytes", 7, False),
        ("BattleResult", "mode_result_json", "bytes", 8, False),
        ("SignedBattleResult", "signature", "bytes", 4, False),
    ]
    for message_name, field_name, field_type, field_number, repeated in required_field_shapes:
        if not check_descriptor_field_shape(
            messages,
            message_name,
            field_name,
            field_type,
            field_number,
            repeated,
        ):
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
    if (
        "required_input_stream_hash" not in result_text
        or "required_event_stream_hash" not in result_text
        or "required_final_state_hash" not in result_text
        or "required_replay_fixture_hash" not in result_text
        or "required_tick_rate_hz" not in result_text
    ):
        print("result boundary missing replay stream/state hash requirements", file=sys.stderr)
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
        or "DevReplayInputStreamSummaryHash" not in simulation_text
        or "CanonicalReplayFixturePayload" not in simulation_text
        or "DevReplayFixtureHash" not in simulation_text
        or "DevModeResultJsonFromReplayFixture" not in simulation_text
        or "DevResultHashFromReplaySummary" not in simulation_text
        or "AcceptModeAction" not in simulation_text
        or "pending_mode_actions_by_tick_" not in simulation_text
        or "ApplyModeActionsForTick" not in simulation_text
        or "mode_id" not in simulation_text
        or "ruleset_version" not in simulation_text
        or "SetPlayerConnected" not in simulation_text
        or "IsPlayerConnected" not in simulation_text
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
        or "IsReconnectModeAction" not in simulation_impl
        or "BuildReplayInputStreamSummary" not in simulation_impl
        or "BuildReplayFixture" not in simulation_impl
        or "CanonicalReplayInputStreamSummaryRecord" not in simulation_impl
        or "DevReplayInputStreamSummaryHash" not in simulation_impl
        or "CanonicalReplayFixturePayload" not in simulation_impl
        or "DevReplayFixtureHash" not in simulation_impl
        or "DevResultHashFromReplaySummary" not in simulation_impl
        or "DevReplayIdFromReplaySummary" not in simulation_impl
        or 'Snapshot("replay_final")' not in simulation_impl
        or "input_tick_duplicate" not in simulation_impl
        or "input_tick_too_far_ahead" not in simulation_impl
        or "mode_action_client_result_forbidden" not in simulation_impl
        or "mode_action_type_unsupported" not in simulation_impl
        or "player_disconnected" not in simulation_impl
        or "player_it->second.connected = true" not in simulation_impl
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
        or "CanonicalReplayFixturePayload(fixture)" not in simulation_impl
        or "CanonicalSnapshotPayload" not in simulation_impl
        or "DevModeResultJsonFromReplayFixture" not in simulation_impl
        or '"tick_rate_hz"' not in simulation_impl
        or "DevReplayInputStreamSummaryHash(fixture.replay_summary_record)" not in simulation_impl
        or "DevReplayFixtureHash(fixture)" not in simulation_impl
        or "player.x_milli" not in simulation_impl
        or "bullet.pattern_id" not in simulation_impl
        or "snapshot.mode_state" not in simulation_impl
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
        or "options.required_tick_rate_hz = replay_fixture.tick_rate_hz" not in server_impl
        or "ReconnectSnapshot" not in server_impl
        or "DispatchEncrypted" not in server_impl
        or "AcceptDecodedInput" not in server_impl
        or "AcceptDecodedModeAction" not in server_impl
        or "SameVersionStamp" not in server_impl
        or "decoded_input_header_mismatch" not in server_impl
        or "decoded_mode_action_header_mismatch" not in server_impl
        or "handshake_required" not in server_impl
        or "handshake_accepted" not in server_impl
        or "client_to_server_key_ref" not in server_impl
        or "session_key_mismatch" not in server_impl
        or "IsSnapshotAckBoundPayload" not in server_impl
        or "encrypted_ack_ahead" not in server_impl
        or "encrypted_event_cursor_ahead" not in server_impl
        or "encrypted_player_disconnected" not in server_impl
        or "encrypted_tick_too_old" not in server_impl
        or "encrypted_tick_too_far_ahead" not in server_impl
        or "match_full" not in server_impl
        or "ticket_not_registered" not in server_impl
        or "IsClientToServerEncryptedPayload" not in server_impl
        or "encrypted_payload_type_invalid" not in server_impl
        or "BuildSignedBattleResult" not in server_impl
        or "CanonicalBattleResultPayload" not in server_impl
        or "DevModeResultJsonFromReplayFixture(replay_fixture)" not in server_impl
        or "DevResultHashFromReplaySummary" not in server_impl
        or "DevReplayIdFromReplaySummary" not in server_impl
        or "projection_only" not in server_impl
        or "fallback_input_count" not in server_impl
        or "mode_action_count" not in server_impl
        or "options.required_input_stream_hash = summary.input_stream_hash" not in server_impl
        or "options.required_event_stream_hash = summary.event_stream_hash" not in server_impl
        or "options.required_final_state_hash = summary.final_state_hash" not in server_impl
        or "options.required_replay_summary_hash = DevReplayInputStreamSummaryHash(replay_fixture.replay_summary_record)" not in server_impl
        or "options.required_replay_fixture_hash = DevReplayFixtureHash(replay_fixture)" not in server_impl
        or "input_stream_hash" not in server_impl
        or "event_stream_hash" not in server_impl
        or "final_state_hash" not in server_impl
        or "replay_summary_hash" not in server_impl
        or "replay_fixture_hash" not in server_impl
    ):
        print("server implementation missing mode/ruleset, capacity, handshake, encrypted session, decoded header/payload binding, client-to-server encrypted payload, encrypted tick/event-cursor window, fallback/mode-action-bound signed-result callback, or registered-player authority guards", file=sys.stderr)
        return 1

    result_impl = (ROOT / "src" / "result.cpp").read_text(encoding="utf-8")
    if (
        "ruleset_version_mismatch" not in result_impl
        or "reward_projection_json" not in result_impl
        or "result_hash_mismatch" not in result_impl
        or "replay_id_mismatch" not in result_impl
        or "event_cursor_mismatch" not in result_impl
        or "final_tick_mismatch" not in result_impl
        or "tick_rate_hz_mismatch" not in result_impl
        or "input_count_mismatch" not in result_impl
        or "mode_action_count_mismatch" not in result_impl
        or "input_trace_count_mismatch" not in result_impl
        or "event_trace_count_mismatch" not in result_impl
        or "input_stream_hash_mismatch" not in result_impl
        or "event_stream_hash_mismatch" not in result_impl
        or "final_state_hash_mismatch" not in result_impl
        or "replay_summary_hash_mismatch" not in result_impl
        or "replay_fixture_hash_mismatch" not in result_impl
        or "reward_projection_mutation_forbidden" not in result_impl
    ):
        print("result boundary missing ruleset/hash/replay/cursor/tick/count/trace/digest verification or projection-only result shape", file=sys.stderr)
        return 1

    protocol_text = (ROOT / "include" / "phk" / "battle" / "protocol.hpp").read_text(encoding="utf-8")
    if (
        "BattleEncryptedPacket" not in protocol_text
        or "DispatchEncrypted" not in protocol_text
        or "DevAeadNonceHex" not in protocol_text
    ):
        print("protocol boundary missing encrypted packet adapter shape", file=sys.stderr)
        return 1

    protocol_impl = (ROOT / "src" / "protocol.cpp").read_text(encoding="utf-8")
    tests_text = (ROOT / "tests" / "battle_server_tests.cpp").read_text(encoding="utf-8")
    if (
        "key_id_missing" not in protocol_impl
        or "nonce_invalid" not in protocol_impl
        or "nonce_mismatch" not in protocol_impl
        or "DevAeadNonceHex" not in protocol_impl
        or "payload_type_missing" not in protocol_impl
        or "ciphertext_missing" not in protocol_impl
        or "auth_tag_invalid" not in protocol_impl
        or "HasExpectedAeadNonceShape" not in protocol_impl
        or "nonce_replay" not in protocol_impl
        or "encrypted_payload_type_invalid" not in protocol_impl
    ):
        print("protocol dispatcher missing encrypted packet key/nonce/ciphertext/tag/payload/replay shape guards", file=sys.stderr)
        return 1
    if "nonce_mismatch_result" not in tests_text or "RefreshDevAeadNonce" not in tests_text:
        print("protocol tests missing header-bound development nonce mismatch coverage", file=sys.stderr)
        return 1

    kcp_text = (ROOT / "include" / "phk" / "battle" / "kcp_endpoint.hpp").read_text(encoding="utf-8")
    kcp_impl = (ROOT / "src" / "kcp_endpoint.cpp").read_text(encoding="utf-8")
    if (
        "KcpAeadPacketAdapter" not in kcp_text
        or "KcpAeadAdapterResult" not in kcp_text
        or "KcpAeadAdapterStats" not in kcp_text
        or "accepted_datagrams" not in kcp_text
        or "rejected_datagrams" not in kcp_text
        or "remote_endpoint_mismatches" not in kcp_text
        or "remote_endpoint_rebinds" not in kcp_text
        or "bound_sessions" not in kcp_text
        or "BattleEncryptedPacket" not in kcp_text
        or "remote_endpoint_by_session_" not in kcp_text
        or "ProcessEncryptedDatagram" not in kcp_impl
        or "KcpAeadPacketAdapter::Stats" not in kcp_impl
        or "server_.DispatchEncrypted(packet)" not in kcp_impl
        or "stats_.accepted_datagrams" not in kcp_impl
        or "stats_.rejected_datagrams" not in kcp_impl
        or "stats_.remote_endpoint_mismatches" not in kcp_impl
        or "stats_.remote_endpoint_rebinds" not in kcp_impl
        or "remote_endpoint_mismatch" not in kcp_impl
        or "remote_rebind_allowed = packet.header.payload_type == BattlePayloadType::Reconnect" not in kcp_impl
        or "endpoint_.ProcessDatagram(datagram)" not in kcp_impl
        or "if (!result.dispatch.ok)" not in kcp_impl
    ):
        print("KCP endpoint missing encrypted AEAD packet adapter boundary, remote rebinding guard, or dispatch-before-forward guard", file=sys.stderr)
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
        or "BattlePayloadType::HandshakeAccept" not in tests_text
        or "BattlePayloadType::ModeAction" not in tests_text
        or "DriveAuthoritativeReplay60Ticks" not in tests_text
        or "fnv64:183370bd6f8c18e7" not in tests_text
        or "fnv64:7c13fa803ae1b2dd" not in tests_text
        or "sha256:dev-fnv64-eb5d3d3884abf76a" not in tests_text
        or "fnv64:a0b383d4a7be0bf7" not in tests_text
        or "fnv64:8049946f03724f36" not in tests_text
        or "sha256:dev-fnv64-a7519545ad65902e" not in tests_text
        or "CanonicalReplayInputStreamSummaryRecord(summary_record) ==" not in tests_text
        or "DevReplayInputStreamSummaryHash(summary_record)" not in tests_text
        or "sha256:dev-fnv64-2a7544832ca5ff92" not in tests_text
        or "CanonicalReplayFixturePayload(fixture)" not in tests_text
        or "sha256:dev-fnv64-f2df27561abbe64e" not in tests_text
        or "tampered_fixture_snapshot" not in tests_text
        or "tampered_fixture_player" not in tests_text
        or "tampered_fixture_bullet" not in tests_text
        or "tampered_fixture_mode_state" not in tests_text
        or "tampered_fixture_authority" not in tests_text
        or "CanonicalBattleResultPayload(built.signed_result.result)" not in tests_text
        or "BuildSignedBattleResult(\"match-001\")" not in tests_text
        or "const auto valid_result = built_result.signed_result" not in tests_text
        or "ReplaceJsonStringField" not in tests_text
        or "ModeResultJsonForSummary" in tests_text
        or "MakeBattleResultForSummary" in tests_text
        or "sha256:dev-fnv64-7cd25aafda3bc356" not in tests_text
        or "sha256:dev-fnv64-4e23b1e341f35e87" not in tests_text
        or "sha256:dev-fnv64-f286e5b4976a50da" not in tests_text
        or "replay_summary_hash_mismatch" not in tests_text
        or "replay_fixture_hash_mismatch" not in tests_text
        or "input_stream_hash_mismatch" not in tests_text
        or "event_stream_hash_mismatch" not in tests_text
        or "final_state_hash_mismatch" not in tests_text
        or "handshake_required" not in tests_text
        or "client_to_server_key_ref" not in tests_text
        or "DecodedPayloadHeaderBinding" not in tests_text
        or "decoded_input_header_mismatch" not in tests_text
        or "decoded_input_payload_type_mismatch" not in tests_text
        or "decoded_mode_action_header_mismatch" not in tests_text
        or "decoded_mode_action_payload_type_mismatch" not in tests_text
        or "KcpAeadPacketAdapterBoundary" not in tests_text
        or "remote_endpoint_mismatch" not in tests_text
        or "reconnect_result" not in tests_text
        or "disconnected_card_action" not in tests_text
        or "action-reconnect-p2" not in tests_text
        or "last_mode_action_type" not in tests_text
        or "adapter.Stats().accepted_datagrams" not in tests_text
        or "adapter.Stats().rejected_datagrams" not in tests_text
        or "adapter.Stats().remote_endpoint_mismatches" not in tests_text
        or "adapter.Stats().remote_endpoint_rebinds" not in tests_text
        or "adapter.Stats().bound_sessions" not in tests_text
        or "session_key_mismatch" not in tests_text
        or "ping_ack_ahead" not in tests_text
        or "endpoint.Stats().datagrams_in, static_cast<std::uint64_t>(0)" not in tests_text
    ):
        print("battle server tests missing payload enum pinning, pinned 60Hz replay/result fingerprints, handshake-bound encrypted session coverage, decoded header/payload binding coverage, or KCP/AEAD remote rebinding coverage", file=sys.stderr)
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
