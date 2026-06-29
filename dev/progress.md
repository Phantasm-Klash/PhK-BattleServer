# PhK-BattleServer Development Progress

Status date: 2026-06-29

| Area | Status | Notes |
| --- | --- | --- |
| Repository skeleton | Started | CMake C++17 project, README, architecture note, source/include layout, CLI entrypoint, tests, and checker are present. `python tools\check_battle_server.py --build` passes locally with CMake/MSVC and CTest. |
| Protocol boundary | Started | Local structs mirror the v0.1 `PhK-Protocol` battle ticket/header/input/result concepts until generated protobuf C++ bindings are wired. The repository now includes `../PhK-Protocol/gen/cpp` in CMake, consumes `phk/v1/manifest.hpp` for protocol/business/battle/ruleset constants, validates generated message-field gates in CTest, and the checker verifies both the shared descriptor and C++ manifest remain in sync. Manifest gates now cover mode actions, snapshots, battle events, signed results, and replay summaries in addition to the basic ticket/header/input/result fields. |
| Ticket verification | Started | Development verifier checks ticket binding, expiry, nonce/hash shape, key id, Ed25519 public-key/signature shape, and rejects raw bearer-like business sessions. Real Ed25519 verification is pending. |
| Battle result submission | Started | Development `SignedBattleResult` boundary and verifier check registered match/mode/ruleset binding, player-id set, local replay-derived result hash, replay id, replay event cursor, projection-only reward shape, settled time, key id, Ed25519 signature shape, and idempotent result hash replay through the server facade after structural verification. The battle server still emits only verifiable result material and rejects obvious inventory, wallet, grant, balance, database, or Steam inventory mutation fields; it does not write inventory, rewards, wallet, or database state. Real result signing, protobuf serialization, and service-to-service submission are pending. |
| Battle handshake | Started | Placeholder handshake re-verifies registered ticket structure/expiry, rejects missing client key/random material or unsupported AEAD lists, selects ChaCha20-Poly1305 labels, and creates deterministic dev session metadata. Real X25519/ECDHE, HKDF, transcript hash, and server signatures are pending. |
| Transport | Started | KCP/UDP endpoint boundary exists with an echo placeholder. Real KCP event loop, retransmission tuning, encryption, and reconnect path are pending. |
| Packet dispatch | Started | Dispatcher validates protocol version, payload type, encrypted-path key id/nonce shape, seq replay, tick sanity, and rejects client-authored battle results. Generated protobuf dispatcher, real protobuf decoding, and AEAD verification are pending. |
| Authoritative simulation | Started | Added a dependency-light C++ simulation slice behind the server facade: fixed `kBattleTickRateHz = 60`, capacity-checked rooms, match-bound `mode_id`/`ruleset_version`, authoritative `BattleInput` validation, per-player seq replay and max-jump rejection, registered-player facade guards, input tick window checks, player disconnect/reconnect state with disconnected-input rejection, connected-player snapshot counts, and cursor-aware reconnect snapshots, milli-unit movement, simplified deterministic radial bullet spawning/movement, `BattleSnapshot` mirror structs, canonical FNV-1a state hash, and replay summary input/event/final hashes. Full hit/graze/card/Boss scoring is still pending. |
| Tests | Started | CTest smoke validates protocol manifest consumption, stricter manifest field gates, ticket verification, ticket replay rejection, mode/ruleset binding, room capacity, registered-ticket handshake acceptance/rejection and shape checks, battle result submission/idempotency plus hash/replay/cursor/projection-only guards, simulation determinism, a 1v1 60-tick authoritative replay/hash fixture, invalid input/mode-action branches, seq window rejection, server-side authoritative input/snapshot/replay summary calls, disconnect/reconnect snapshot/cursor and input guards, KCP placeholder echo, encrypted packet key/nonce shape guards, and dispatcher result rejection. The repository checker now handles Visual Studio multi-config builds with `--config Debug` and asserts result/simulation/server/handshake/protocol authority boundaries exist. |

## 2026-06-29 Implementation Sync

- Tightened the dispatcher scaffold so input, mode-action, ping, and reconnect packets require a key id and hex nonce shape before seq replay state is accepted. This is still a development shape gate, not production AEAD verification.
- Tightened the signed-result boundary so reward projection JSON cannot contain obvious inventory, wallet, currency, grant, item, balance, database, or Steam inventory mutation fields. The result remains replay/hash/cursor material for Nakama/Go to verify and settle idempotently.
- Expanded the PhK-Protocol C++ manifest checker to require mode-action, snapshot, battle-event, signed-result, and replay-summary fields while full generated protobuf C++ bindings are still pending.
- Added a deterministic 1v1 60-tick authoritative replay/hash fixture with two players, 120 accepted inputs, stable replay hashes, 60Hz snapshot projection, and deterministic bullet-event counts.

## 2026-06-29 Verification

- `python3 tools/check_battle_server.py` passes.
- Direct host `python3 tools/check_battle_server.py --build` is blocked because this container does not have `cmake` installed.
- Direct host `g++ -std=c++17 -Iinclude -I../PhK-Protocol/gen/cpp tests/battle_server_tests.cpp src/ticket.cpp src/handshake.cpp src/kcp_endpoint.cpp src/protocol.cpp src/result.cpp src/server.cpp src/simulation.cpp -o /tmp/phk_battle_tests && /tmp/phk_battle_tests` passes.
- Docker regression passes with legacy Compose: `docker-compose run --rm test`.
- Direct `/root/gotouhou/docs/ops/protocol_audit_check.py` is not executable in this workspace and returns permission denied; `env HOME=/root GOCACHE=/tmp/go-build-cache python3 /root/gotouhou/docs/ops/protocol_audit_check.py` passes across PhK-Protocol, Gensoulkyo, and PhK-BattleServer.

## 2026-06-28 Verification

- `python3 PhK-BattleServer/tools/check_battle_server.py` passes.
- Direct `g++ -std=c++17 ... /tmp/phk_battle_tests` build and `/tmp/phk_battle_tests` pass in the current Linux container.
- CMake/CTest could not be run in the current container because `cmake` is not installed; the source remains wired in `CMakeLists.txt` for the normal repository build path.
- The current simulation smoke includes mode/ruleset snapshot and replay projection, room capacity rejection, registered-ticket handshake rejection, handshake shape rejection, disconnected player snapshot projection, disconnected-input rejection, reconnect restoration and cursor bounds, invalid direction bits, invalid card slot, seq jump rejection, missing mode-action fields, far-future mode-action rejection, registered-player guards, and battle-result mode/ruleset/hash/replay/cursor mismatch rejection.
