# PhK-BattleServer Development Progress

Status date: 2026-06-29

| Area | Status | Notes |
| --- | --- | --- |
| Repository skeleton | Started | CMake C++17 project, README, architecture note, source/include layout, CLI entrypoint, tests, and checker are present. `python tools\check_battle_server.py --build` passes locally with CMake/MSVC and CTest. |
| Protocol boundary | Started | Local structs mirror the v0.1 `PhK-Protocol` battle ticket/header/encrypted-packet/input/result/replay concepts until generated protobuf C++ bindings are wired. The repository now includes `../PhK-Protocol/gen/cpp` in CMake, consumes `phk/v1/manifest.hpp` for protocol/business/battle/ruleset constants, validates generated message-field gates in CTest, and the checker verifies both the shared descriptor and C++ manifest remain in sync. Manifest gates now cover signed tickets, handshake hello/accept, encrypted packets, mode actions, snapshots, battle events, signed results, and replay summaries including replay id, owner user id, event-stream hash, and final tick fields in addition to the basic ticket/header/input/result fields. |
| Ticket verification | Started | Development verifier checks ticket binding, expiry, nonce/hash shape, key id, Ed25519 public-key/signature shape, and rejects raw bearer-like business sessions. Real Ed25519 verification is pending. |
| Battle result submission | Started | Development `SignedBattleResult` boundary and verifier check registered match/mode/ruleset binding, player-id set, local replay-derived result hash, replay id, replay event cursor, final tick, input/fallback/mode-action counters, replay trace counts, projection-only reward shape, settled time, key id, Ed25519 signature shape, and idempotent result hash replay through the server facade after structural verification. The server facade can now build replay-bound development signed-result callback material from the local authoritative replay summary and immediately verify/submit it through the same boundary. The battle server still emits only verifiable result material and rejects obvious inventory, wallet, grant, balance, database, or Steam inventory mutation fields; it does not write inventory, rewards, wallet, or database state. Real result signing, protobuf serialization, and service-to-service submission are pending. |
| Battle handshake | Started | Placeholder handshake re-verifies registered ticket structure/expiry, rejects missing client key/random material or unsupported AEAD lists, selects ChaCha20-Poly1305/XChaCha20-Poly1305 labels, creates deterministic dev session metadata, derives development client-to-server/server-to-client key refs, and emits Ed25519-shaped transcript signature material. Real X25519/ECDHE, HKDF, transcript signing, and server signature verification are pending. |
| Transport | Started | KCP/UDP endpoint boundary exists with an echo placeholder. Real KCP event loop, retransmission tuning, encryption, and reconnect path are pending. |
| Packet dispatch | Started | Dispatcher validates protocol version, payload type, encrypted-path key id/nonce/ciphertext/auth-tag shape, encrypted adapter nonce reuse, seq replay, tick sanity, and rejects client-authored battle results. Generated protobuf dispatcher, real protobuf decoding, and AEAD verification are pending. |
| Authoritative simulation | Started | Added a dependency-light C++ simulation slice behind the server facade: fixed `kBattleTickRateHz = 60`, capacity-checked rooms, match-bound `mode_id`/`ruleset_version`, authoritative `BattleInput` validation, per-player seq replay and max-jump rejection, registered-player facade guards, input tick window checks, per-player duplicate input/tick rejection before replay hash state advances, player disconnect/reconnect state with disconnected-input rejection, connected-player snapshot counts, and cursor-aware reconnect snapshots, milli-unit movement, simplified deterministic radial bullet spawning/movement, `BattleSnapshot` mirror structs, canonical FNV-1a state hash, replay summary input/event/final hashes, deterministic input/event trace material, manifest-compatible replay input summary record material, and development replay fixture material that binds replay id, owner user id, ordered players, event cursor, final snapshot, trace material, tick rate, and result hash. Full hit/graze/card/Boss scoring is still pending. |
| Tests | Started | CTest smoke validates protocol manifest consumption, stricter manifest field gates, ticket verification, ticket replay rejection, mode/ruleset binding, room capacity, registered-ticket handshake acceptance/rejection, dev transcript key/signature material, battle result submission/idempotency plus hash/replay/cursor/final-tick/replay-count/projection-only guards, simulation determinism, a 1v1 60-tick authoritative replay/hash fixture, replay fixture trace and manifest-compatible replay-summary record material, invalid input/mode-action branches, duplicate player/tick input rejection, seq window rejection, server-side authoritative input/snapshot/replay summary calls, disconnect/reconnect snapshot/cursor and input guards, KCP placeholder echo, encrypted packet key/nonce/ciphertext/auth-tag shape guards, and dispatcher result rejection. The repository checker now handles Visual Studio multi-config builds with `--config Debug` and asserts result/simulation/server/handshake/protocol authority boundaries exist. |

## 2026-06-29 Replay Summary Manifest Boundary

- Added `ReplayInputStreamSummaryRecord` and `BattleSimulation::BuildReplayInputStreamSummary`, a dependency-light record shaped to the generated `PhK-Protocol` `ReplayInputStreamSummary` fields: version, replay id, owner user id, match id, input/event counts, input/event stream hashes, final state hash, and final tick.
- `ReplayFixture` now embeds that record alongside the final authoritative snapshot and ordered trace material, so protobuf replacement has a concrete record boundary instead of only aggregate C++ structs.
- CTest now checks the record against the generated manifest field gates and canonical serialization/tamper behavior. `tools/check_battle_server.py` gates replay id, owner user id, event-stream hash, and the record builder while full protobuf C++ bindings remain pending.

## 2026-06-29 Signed Result Replay Counter Boundary

- Signed-result callback material now includes accepted-input count, fallback-input counters, mode-action count, final tick, and input/event trace counts in `mode_result_json`.
- `BattleResultVerifier` now binds those callback counters to the authoritative `ReplaySummary` before accepting a result, so a C++ result callback cannot preserve the result hash/replay id while advertising stale replay counters to Nakama/Go.
- CTest coverage now rejects missing replay counter fields plus mismatched final tick, mode-action count, and event-trace count. `tools/check_battle_server.py` gates the new result verifier strings while real protobuf result bindings are still pending.

## 2026-06-29 Replay Trace Boundary

- `ReplaySummary` and `ReplayFixture` now carry deterministic input and event trace vectors in addition to aggregate hashes/counters. Accepted inputs, neutral/held fallback inputs, bullet-spawn events, mode actions, and connection events are recorded in the same stable order the authoritative simulation processes them.
- Development result-hash derivation now folds the trace lines into `DevResultHashFromReplaySummary`, so a result callback or replay fixture cannot preserve aggregate counters while changing ordered replay material unnoticed.
- CTest coverage now checks deterministic trace parity across two 60Hz simulations, fallback trace classification, fixture trace export, connection/mode-action trace entries, and result-hash sensitivity to trace tampering. The repository checker also gates trace fields and hash wiring while full protobuf replay bindings are pending.

## 2026-06-29 Encrypted Reconnect Cursor Boundary

- Server-facade encrypted reconnect packets now bind header `ack` to the authoritative replay event cursor and reject impossible future cursors with `encrypted_event_cursor_ahead` before dispatcher seq/nonce state advances.
- CTest coverage now checks both the rejection path and an accepted reconnect packet at the current event cursor, keeping the encrypted KCP/AEAD scaffold aligned with `ReconnectSnapshot(..., last_seen_event_cursor)` until generated protobuf decoding carries the cursor explicitly.
- `tools/check_battle_server.py` and `docs/architecture.md` now gate and document the reconnect cursor boundary alongside the existing encrypted session and tick-window checks.

## 2026-06-29 Handshake Transcript Boundary

- `BattleHandshakeAccept` now exposes development client-to-server and server-to-client key references plus Ed25519-shaped server transcript signature material derived from the accepted ticket transcript.
- Handshake tests now assert ChaCha20 and XChaCha20 selection, transcript hash shape, direction-specific key refs, and deterministic server signature shape/material while keeping the implementation clearly marked as non-production crypto.
- `tools/check_battle_server.py` and `docs/architecture.md` now gate and document this X25519/HKDF/transcript-signing replacement boundary before generated protobuf and real crypto dependencies land.

## 2026-06-29 Tick-Bound Mode Action Sync

- Mode actions now stay inside the 60Hz authoritative tick boundary: `AcceptModeAction` validates identity/version/seq/tick/type/result-forbidden shape and reserves the player seq, but queues the action by target tick instead of immediately mutating replay/event/hash state.
- `BattleSimulation::Tick` applies queued mode actions for the tick being advanced in stable seq/player/action order before bullet events and snapshots are built, so result hashes and event cursors only include mode intent that the server tick has actually processed.
- CTest coverage now asserts queued mode actions are invisible in replay summaries and snapshots before their target tick, then become visible in event hash, mode-action count, last-action projection, and battle-result verification after the tick advances.
- `tools/check_battle_server.py` now gates the pending mode-action queue and tick application path while the repository still uses manifest-backed local structs before generated protobuf C++ bindings land.

## 2026-06-29 Replay Fixture Boundary

- Added `ReplayFixture` and `BattleSimulation::BuildReplayFixture`, a development-only envelope that packages the authoritative replay summary, final snapshot, ordered player ids, owner user id, 60Hz tick-rate metadata, event cursor, replay id, and replay-derived result hash as one protobuf-replacement boundary.
- Moved the development result-hash and replay-id derivation into the simulation boundary as `DevResultHashFromReplaySummary` and `DevReplayIdFromReplaySummary`, so server callback material and replay fixture tests cannot drift while real protobuf/result signing are pending.
- Added CTest coverage for a 60-tick 1v1 replay fixture and extended `tools/check_battle_server.py` to gate replay fixture material plus shared replay/result hash derivation.

## 2026-06-29 Duplicate Input Tick Boundary

- `BattleSimulation::ValidateInput` now rejects a second input for the same `player_id` and authoritative `tick` with `input_tick_duplicate`, even if the later packet has a higher seq. This prevents a queued input from being replaced after the replay input hash already counted the first packet.
- The simulation boundary now exposes `DuplicateInputForTick` in `InputValidationCodeName`, documents the rule in `docs/architecture.md`, and keeps the checker gating the duplicate-input reason while the protobuf/KCP path remains a scaffold.
- CTest coverage now asserts duplicate same-player/same-tick input rejection in the deterministic simulation path before seq replay and tick-window cases.

## 2026-06-29 Implementation Sync

- Added `BattleServer::BuildSignedBattleResult`, a development-only signed-result callback builder that derives match/mode/ruleset, player ids, result hash, replay id, event cursor, final tick, projection-only reward JSON, and dev Ed25519-shaped signature metadata from the local replay summary. This gives the C++ result boundary a testable outbound material path before real signing and service-to-service submission land.
- Added CTest coverage that generates the dev signed-result callback, verifies result hash/replay id/cursor/key/signature shape, submits it through the existing `SubmitBattleResult` verifier, and checks idempotent duplicate handling.
- Tightened the dispatcher scaffold so input, mode-action, ping, and reconnect packets require a key id and hex nonce shape before seq replay state is accepted. This is still a development shape gate, not production AEAD verification.
- Added a dependency-light `BattleEncryptedPacket` adapter shape guard aligned with the generated manifest: encrypted-path packets must provide non-empty ciphertext, a 16-byte auth tag, key id, nonce, and an allowed encrypted payload type. Client-authored result packets and server event/snapshot packets are still rejected on this client encrypted-submit path.
- Tightened the signed-result boundary so reward projection JSON cannot contain obvious inventory, wallet, currency, grant, item, balance, database, or Steam inventory mutation fields. The result remains replay/hash/cursor material for Nakama/Go to verify and settle idempotently.
- Expanded the PhK-Protocol C++ manifest checker to require mode-action, snapshot, battle-event, signed-result, and replay-summary fields while full generated protobuf C++ bindings are still pending.
- Added a deterministic 1v1 60-tick authoritative replay/hash fixture with two players, 120 accepted inputs, stable replay hashes, 60Hz snapshot projection, and deterministic bullet-event counts.
- Added encrypted adapter nonce replay rejection keyed by `match_id:player_id:key_id:nonce`, so a reused nonce is rejected before real ChaCha20-Poly1305 lands. This is still a development replay guard and does not replace production AEAD verification.
- Tightened the C++ manifest gate to cover signed-ticket and handshake hello/accept fields, including future `server_signature`, so the temporary manifest bridge fails sooner if the generated protobuf shape drifts.
- Added a server-facade encrypted packet session boundary: encrypted packet dispatch now requires a registered match/player session and the session key id before the dispatcher records seq or nonce state. This keeps the current KCP/AEAD placeholder tied to allocated battle tickets while real session keys are pending.
- Added a server-facade encrypted input tick-window boundary: encrypted input and mode-action packet headers are rejected when stale or beyond the match simulation's configured input-ahead window before dispatcher seq/nonce state is recorded. This keeps the KCP/AEAD scaffold aligned with the 60Hz authoritative simulation window while real protobuf decoding and AEAD verification are pending.
- Tightened the encrypted adapter nonce shape from "at least 96-bit hex" to exactly 24 hex characters, matching the migration-time ChaCha20-Poly1305 nonce contract before real AEAD lands.
- Added a server-facade encrypted input ack boundary: encrypted input and mode-action headers cannot acknowledge snapshots beyond the authoritative simulation tick, while current-tick acknowledgements remain accepted.
- Extended the 1v1 60Hz replay fixture to assert reconnect snapshots at the final replay tick, including stable state hash, event cursor, missed-event count, and event-cursor-ahead rejection.
- Added replay-summary fallback input audit counters for missing-input ticks. The simulation now separates accepted inputs, neutral fallback ticks, and held-input fallback ticks, exposes those counters in snapshots/reconnect snapshots, folds them into the canonical state/input hash path, and includes them in development signed-result callback material so result hashes cannot ignore latency or missing-input fallback behavior.
- Added focused CTest coverage for neutral and held fallback audit paths alongside the existing fully supplied 60Hz 1v1 replay fixture, and extended the repository checker to require the fallback audit boundary while the protobuf replay summary remains manifest-backed.
- Added a shared mode-action type gate for the current scaffold: `cast_card`, `select_round_card`, `transfer_card`, `ready`, and `reconnect` are accepted, while unknown action types such as reward-grant attempts are rejected before replay state advances. Accepted mode actions now have an explicit replay-summary/snapshot count that is folded into the canonical state hash and development signed-result material, so result hashes cannot ignore mode-intent traffic.

## 2026-06-29 Verification

- Current replay summary manifest sample: `python3 tools/check_battle_server.py` passes.
- Current replay summary manifest sample: direct `g++ -std=c++17 -Iinclude -I../PhK-Protocol/gen/cpp tests/battle_server_tests.cpp src/ticket.cpp src/handshake.cpp src/kcp_endpoint.cpp src/protocol.cpp src/result.cpp src/server.cpp src/simulation.cpp -o /tmp/phk_battle_tests && /tmp/phk_battle_tests` passes, including `ReplayFixtureBoundary` record checks.
- Current replay summary manifest sample: `docker-compose run --rm test` passes with a clean container CMake build and CTest run.
- Current replay summary manifest sample: `env HOME=/root GOCACHE=/tmp/go-build-cache python3 /root/gotouhou/docs/ops/protocol_audit_check.py` passes across PhK-Protocol, Gensoulkyo, and PhK-BattleServer.
- Current replay trace boundary sample: `python3 tools/check_battle_server.py` passes.
- Current replay trace boundary sample: direct `g++ -std=c++17 -Iinclude -I../PhK-Protocol/gen/cpp tests/battle_server_tests.cpp src/ticket.cpp src/handshake.cpp src/kcp_endpoint.cpp src/protocol.cpp src/result.cpp src/server.cpp src/simulation.cpp -o /tmp/phk_battle_tests && /tmp/phk_battle_tests` passes, including deterministic input/event traces and result-hash tamper sensitivity.
- Current replay trace boundary sample: `docker-compose run --rm test` passes with a clean container CMake build and CTest run.
- Current replay trace boundary sample: `env HOME=/root GOCACHE=/tmp/go-build-cache python3 /root/gotouhou/docs/ops/protocol_audit_check.py` passes across PhK-Protocol, Gensoulkyo, and PhK-BattleServer.
- Current tick-bound mode-action sample: direct `g++ -std=c++17 ... /tmp/phk_battle_tests` build and `/tmp/phk_battle_tests` pass, including queued mode-action replay invisibility before the target tick and event/hash projection after the tick advances.
- Current tick-bound mode-action sample: `python3 tools/check_battle_server.py` passes.
- Current tick-bound mode-action sample: `docker-compose run --rm test` passes with a clean container CMake build and CTest run.
- Current tick-bound mode-action sample: `env HOME=/root GOCACHE=/tmp/go-build-cache python3 /root/gotouhou/docs/ops/protocol_audit_check.py` passes across PhK-Protocol, Gensoulkyo, and PhK-BattleServer.
- Current duplicate-input boundary sample: `python3 tools/check_battle_server.py` passes.
- Current duplicate-input boundary sample: direct `g++ -std=c++17 -Iinclude -I../PhK-Protocol/gen/cpp tests/battle_server_tests.cpp src/ticket.cpp src/handshake.cpp src/kcp_endpoint.cpp src/protocol.cpp src/result.cpp src/server.cpp src/simulation.cpp -o /tmp/phk_battle_tests && /tmp/phk_battle_tests` passes, including `input_tick_duplicate`.
- Current duplicate-input boundary sample: `docker-compose run --rm test` passes with a clean container CMake build and CTest run.
- Current duplicate-input boundary sample: `env HOME=/root GOCACHE=/tmp/go-build-cache python3 /root/gotouhou/docs/ops/protocol_audit_check.py` passes across PhK-Protocol, Gensoulkyo, and PhK-BattleServer.
- Current fallback replay audit sample: `python3 tools/check_battle_server.py` passes.
- Current fallback replay audit sample: direct `g++ -std=c++17 ... /tmp/phk_battle_tests` build and `/tmp/phk_battle_tests` pass, including `FallbackInputReplayAudit`.
- Current fallback replay audit sample: `docker-compose run --rm test` passes with a clean container CMake build and CTest run.
- Current fallback replay audit sample: `env HOME=/root GOCACHE=/tmp/go-build-cache python3 /root/gotouhou/docs/ops/protocol_audit_check.py` passes across PhK-Protocol, Gensoulkyo, and PhK-BattleServer.
- Current signed-result callback sample: `python3 tools/check_battle_server.py` passes.
- Current signed-result callback sample: direct `g++ -std=c++17 ... /tmp/phk_battle_tests` build and `/tmp/phk_battle_tests` pass, including `BuildSignedBattleResultCallback`.
- Current signed-result callback sample: `python3 tools/check_battle_server.py --build` is still blocked on the host because `cmake` is not installed.
- Current signed-result callback sample: `docker-compose run --rm test` passes with a clean container CMake build and CTest run.
- Current signed-result callback sample: `env HOME=/root GOCACHE=/tmp/go-build-cache python3 /root/gotouhou/docs/ops/protocol_audit_check.py` passes across PhK-Protocol, Gensoulkyo, and PhK-BattleServer.
- `python3 tools/check_battle_server.py` passes.
- `python3 tools/check_battle_server.py --build` is blocked on the host because `cmake` is not installed.
- `docker compose run --rm test` is blocked because this host has legacy `docker-compose` but no Docker Compose v2 plugin.
- `docker-compose run --rm test` passes with a clean container CMake build and CTest run.
- `/root/gotouhou/docs/ops/protocol_audit_check.py` is not executable on this host, but `python3 /root/gotouhou/docs/ops/protocol_audit_check.py` passes across PhK-Protocol, Gensoulkyo, and PhK-BattleServer.
- Direct host `g++ -std=c++17 -Iinclude -I../PhK-Protocol/gen/cpp tests/battle_server_tests.cpp src/ticket.cpp src/handshake.cpp src/kcp_endpoint.cpp src/protocol.cpp src/result.cpp src/server.cpp src/simulation.cpp -o /tmp/phk_battle_tests && /tmp/phk_battle_tests` passes.
- Docker regression passes with legacy Compose: `docker-compose run --rm test`.
- `env HOME=/root GOCACHE=/tmp/go-build-cache python3 /root/gotouhou/docs/ops/protocol_audit_check.py` passes across PhK-Protocol, Gensoulkyo, and PhK-BattleServer.
- Current mode-action boundary sample: `python3 tools/check_battle_server.py` passes.
- Current mode-action boundary sample: direct `g++ -std=c++17 ... /tmp/phk_battle_tests` build and `/tmp/phk_battle_tests` pass, including unsupported mode-action type rejection and mode-action audit projection.
- Current mode-action boundary sample: `docker-compose run --rm test` passes with a clean container CMake build and CTest run.
- Current mode-action boundary sample: `env HOME=/root GOCACHE=/tmp/go-build-cache python3 /root/gotouhou/docs/ops/protocol_audit_check.py` passes across PhK-Protocol, Gensoulkyo, and PhK-BattleServer.
- Current replay fixture sample: direct `g++ -std=c++17 ... /tmp/phk_battle_tests` build and `/tmp/phk_battle_tests` pass, including `ReplayFixtureBoundary`.
- Current replay fixture sample: `python3 tools/check_battle_server.py` passes.
- Current replay fixture sample: `docker-compose run --rm test` passes with a clean container CMake build and CTest run.
- Current replay fixture sample: `env HOME=/root GOCACHE=/tmp/go-build-cache python3 /root/gotouhou/docs/ops/protocol_audit_check.py` passes across PhK-Protocol, Gensoulkyo, and PhK-BattleServer.

## 2026-06-28 Verification

- `python3 PhK-BattleServer/tools/check_battle_server.py` passes.
- Direct `g++ -std=c++17 ... /tmp/phk_battle_tests` build and `/tmp/phk_battle_tests` pass in the current Linux container.
- CMake/CTest could not be run in the current container because `cmake` is not installed; the source remains wired in `CMakeLists.txt` for the normal repository build path.
- The current simulation smoke includes mode/ruleset snapshot and replay projection, room capacity rejection, registered-ticket handshake rejection, handshake shape rejection, disconnected player snapshot projection, disconnected-input rejection, reconnect restoration and cursor bounds, invalid direction bits, invalid card slot, seq jump rejection, missing mode-action fields, far-future mode-action rejection, registered-player guards, and battle-result mode/ruleset/hash/replay/cursor mismatch rejection.
