# PhK-BattleServer Development Progress

Status date: 2026-06-28

| Area | Status | Notes |
| --- | --- | --- |
| Repository skeleton | Started | CMake C++17 project, README, architecture note, source/include layout, CLI entrypoint, tests, and checker are present. `python tools\check_battle_server.py --build` passes locally with CMake/MSVC and CTest. |
| Protocol boundary | Started | Local structs mirror the v0.1 `PhK-Protocol` battle ticket/header/input/result concepts until generated protobuf C++ bindings are wired. The repository now includes `../PhK-Protocol/gen/cpp` in CMake, consumes `phk/v1/manifest.hpp` for protocol/business/battle/ruleset constants, validates generated message-field gates in CTest, and the checker verifies both the shared descriptor and C++ manifest remain in sync. |
| Ticket verification | Started | Development verifier checks ticket binding, expiry, nonce/hash shape, key id, Ed25519 public-key/signature shape, and rejects raw bearer-like business sessions. Real Ed25519 verification is pending. |
| Battle result submission | Started | Development `SignedBattleResult` boundary and verifier check registered match/mode/ruleset binding, player-id set, result hash, replay id, settled time, key id, Ed25519 signature shape, and idempotent result hash replay through the server facade after structural verification. Real result signing, protobuf serialization, and service-to-service submission are pending. |
| Battle handshake | Started | Placeholder handshake re-verifies registered ticket structure/expiry, selects ChaCha20-Poly1305 labels, and creates deterministic dev session metadata. Real X25519/ECDHE, HKDF, transcript hash, and server signatures are pending. |
| Transport | Started | KCP/UDP endpoint boundary exists with an echo placeholder. Real KCP event loop, retransmission tuning, encryption, and reconnect path are pending. |
| Packet dispatch | Started | Dispatcher validates protocol version, seq replay, tick sanity, and rejects client-authored battle results. Generated protobuf dispatcher and battle simulation are pending. |
| Authoritative simulation | Started | Added a dependency-light C++ simulation slice behind the server facade: fixed `kBattleTickRateHz = 60`, capacity-checked rooms, match-bound `mode_id`/`ruleset_version`, authoritative `BattleInput` validation, per-player seq replay rejection, registered-player facade guards, input tick window checks, player disconnect/reconnect state with disconnected-input rejection and connected-player snapshot counts, milli-unit movement, simplified deterministic radial bullet spawning/movement, `BattleSnapshot` mirror structs, canonical FNV-1a state hash, and replay summary input/event/final hashes. Full hit/graze/card/Boss scoring is still pending. |
| Tests | Started | CTest smoke validates protocol manifest consumption, ticket verification, ticket replay rejection, mode/ruleset binding, room capacity, registered-ticket handshake acceptance/rejection, battle result submission/idempotency guards, simulation determinism, invalid input/mode-action branches, server-side authoritative input/snapshot/replay summary calls, disconnect/reconnect snapshot and input guards, KCP placeholder echo, and dispatcher guards. The repository checker now handles Visual Studio multi-config builds with `--config Debug` and asserts result/simulation/server authority boundaries exist. |

## 2026-06-28 Verification

- `python3 PhK-BattleServer/tools/check_battle_server.py` passes.
- Direct `g++ -std=c++17 ... /tmp/phk_battle_tests` build and `/tmp/phk_battle_tests` pass in the current Linux container.
- CMake/CTest could not be run in the current container because `cmake` is not installed; the source remains wired in `CMakeLists.txt` for the normal repository build path.
- The current simulation smoke includes mode/ruleset snapshot and replay projection, room capacity rejection, registered-ticket handshake rejection, disconnected player snapshot projection, disconnected-input rejection, reconnect restoration, invalid direction bits, invalid card slot, missing mode-action fields, far-future mode-action rejection, registered-player guards, and battle-result mode/ruleset mismatch rejection.
