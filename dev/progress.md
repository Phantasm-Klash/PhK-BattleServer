# PhK-BattleServer Development Progress

Status date: 2026-06-30

## 2026-06-30 Battle Royale Select-Round Payload Boundary

- `select_round_card` mode actions are now battle-royale-only and require an integer `candidate_index` in the server-generated three-choice range 0-2 before they can enter the authoritative tick queue.
- Payloads that omit the candidate, pick an out-of-range candidate, target the wrong mode, or smuggle reward/settlement authority fields are rejected before replay/hash state changes.
- This is intent validation only. Candidate generation, card ownership, rewards, inventory, wallet, Steam state, and business database state remain outside the C++ battle server.

## 2026-06-30 Boss Transfer Mode Scope Boundary

- `transfer_card` mode actions are now Boss-only. Non-Boss matches reject transfer attempts as `transfer_card_mode_unsupported` before transfer-card authority state can be configured or consumed.
- Existing Boss transfer authority checks still cover unknown card instances, owner mismatches, mode-forbidden cards, unpaid costs, cooldown blocks, duplicate transfer attempts, replay trace material, and result projection binding.
- This remains in-memory battle authority only. The C++ battle server still does not persist cards, inventory, rewards, wallet, Steam state, or business database state.

## 2026-06-30 Boss Transfer Aggregate Audit Boundary

- Boss snapshots and development result projection now carry deterministic `transfer_card_edges_material` for every accepted in-match transfer, not only the last transfer.
- The aggregate binds card instance id, from/to player ids, and frozen server authority flags into the final replay snapshot, canonical state hash, signed-result `mode_result_json`, and result verifier options.
- `BattleResultVerifier` rejects tampered aggregate transfer material with `transfer_card_edges_mismatch`, while existing last-transfer fields remain for compatibility. This is replay/result audit material only and does not persist inventory, rewards, wallet, Steam state, or business database state.

## 2026-06-30 Mode Action Authority Payload Boundary

- `cast_card` mode actions now require a server-validated integer `card_slot` intent in the 0-7 range before they can enter the authoritative tick queue.
- All mode-action payloads now reject obvious client-authored authority fields such as position, damage, Boss HP, score, rank, reward, inventory, wallet, database, Steam inventory, result hash, or settlement material.
- This keeps mode actions as player intent only; effects, Boss damage, result hashes, replay material, and settlement remain server-owned.

## 2026-06-30 Boss Layout Count Result Binding

- Boss snapshots and development result projection now carry `boss_layout_player_count`, binding the server-selected Boss ring layout size alongside registered player count and per-player spawn slots.
- `BattleServer::SubmitBattleResult` rejects forged layout-count result material before accepting a signed callback, so Nakama/Go can audit that Boss replay/result layout metadata came from the C++ final snapshot.
- This remains replay/result audit material only. The C++ battle server still does not persist Boss HP, rewards, inventory, wallet, Steam state, or business database state.

## 2026-06-30 Boss Registered Readiness Audit Boundary

- Boss snapshots and development result projection now carry `boss_registered_player_count`, `boss_all_registered_connected`, and `boss_all_registered_ready` alongside the existing connected/ready/start fields.
- `boss_ready_to_start` is now derived from the documented 4-8 connected-player start window plus all registered Boss players being connected and ready, so disconnects or partial ready state cannot be hidden behind a coarse ready count.
- `BattleServer::SubmitBattleResult` binds these fields back to the final replay snapshot, and CTest/checker coverage rejects forged registered-count, all-connected, or all-ready result material before settlement acceptance.
- `ready` mode actions now require an explicit server-validated `{"ready":true}` intent; missing or false ready payloads are rejected before they can mutate ready counts or Boss start audit state.
- This remains replay/result audit material only. The C++ battle server still does not persist Boss HP, rewards, inventory, wallet, Steam state, or business database state.

## 2026-06-30 Replay Seed Result Binding

- Replay summaries, manifest-compatible replay input-stream summary records, canonical replay fixtures, replay records, development mode-result JSON, and signed result hash material now carry the server-owned `match_seed`.
- `BattleServer::SubmitBattleResult` binds the expected seed from the local replay summary, and `BattleResultVerifier` rejects missing or forged seed material with `match_seed_mismatch` before accepting a development signed battle result.
- CTest pins the updated replay/result hashes and covers seed tampering across stream records, fixtures, and submitted result JSON. `tools/check_battle_server.py` now also gates the architecture doc's seed binding notes so replay/result audit docs cannot drift from the implementation.
- This remains deterministic replay/result audit material only. The C++ battle server still does not write inventory, rewards, wallet, Steam state, or business database state.

## 2026-06-30 Decoded Session Boundary

- Direct protobuf-replacement decoded handoff now reuses the negotiated session boundary before simulation state changes.
- `AcceptDecodedInput`, `AcceptDecodedModeAction`, and decoded reconnect handoff require a registered match session that completed handshake, a matching inbound client-to-server key reference, and valid snapshot/event acknowledgement bounds.
- CTest now rejects wrong-session-key decoded input and snapshot ACK claims ahead of the authoritative tick without advancing replay counters. This keeps future protobuf decode entrypoints aligned with the encrypted packet adapter while real protobuf/KCP/AEAD remains pending.

## 2026-06-30 Boss Start Readiness Result Binding

- Boss development result projection now carries the server-owned start boundary fields: `boss_min_players`, `boss_max_players`, `boss_start_ready`, `boss_ready_player_count`, and `boss_ready_to_start`.
- Boss result projection also carries final `connected_player_count` and `disconnected_player_count`, so the business server can audit whether settlement came from the same connected-room state the C++ simulation used.
- Boss result projection now carries each player's server-generated ring spawn slot and fixed fire target, and `SubmitBattleResult` binds that layout audit material back to the final replay snapshot.
- `BattleServer::SubmitBattleResult` binds these fields back to the final replay snapshot, and `BattleResultVerifier` rejects forged Boss readiness/capacity/connection-count/layout result material with dedicated mismatch reasons.
- This is audit/settlement context for Nakama/Go only; the C++ battle server still does not persist Boss HP, rewards, inventory, wallet, or business database state.

## 2026-06-30 Settled Match Freeze Boundary

- Once a signed battle result is accepted, the server facade now freezes that match until `RetireMatch` clears it.
- Post-settlement input, mode actions, reconnect-style decoded actions, connection-state mutation, transferable-card configuration, encrypted dispatch, and tick advancement are rejected or held with `match_settled`, preserving the signed replay/hash material.
- CTest and `tools/check_battle_server.py` now explicitly gate decoded input, decoded mode action, and decoded reconnect entrypoints against the same settled-match freeze.
- Read-only snapshot and replay summary access remain available before retirement for business-server audit; retired matches still reject new tickets as `match_retired`.

## 2026-06-30 Boss Max HP Result Binding

- Development Boss result projection now includes server-owned `boss_max_hp` alongside current HP and damage totals.
- `BattleServer::SubmitBattleResult` binds `boss_max_hp` to the final replay snapshot, and `BattleResultVerifier` rejects forged max-HP result material as `boss_max_hp_mismatch`.
- CTest and `tools/check_battle_server.py` cover the projection and tamper path. This is still settlement audit context only; persistent Boss HP and rewards remain in Nakama/Go.

## 2026-06-30 Boss Friendly-Fire Policy Projection

- Boss simulations now normalize the documented friendly-fire policies (`disabled`, `player_bullets_only`, `all_friendly_fire`) and fall back to `disabled` for unknown values.
- Boss snapshots and development `mode_result_json` carry the server-owned `boss_friendly_fire_policy`, and result verification binds that policy back to the final replay snapshot before accepting a signed callback.
- Tests and `tools/check_battle_server.py` now gate policy projection, invalid-policy fallback, and tampered result rejection. This only exposes configuration/audit material; full teammate damage mechanics remain pending.

## 2026-06-30 Transfer Card Result Verification Boundary

- `BattleServer::SubmitBattleResult` now binds accepted Boss `transfer_card` projection fields back to the server-owned final replay snapshot before accepting a signed result callback.
- `BattleResultVerifier` rejects tampered transfer count, card instance id, from/to player ids, and frozen authority flags with dedicated mismatch reasons.
- The existing Boss result submission test now covers a signed result that includes a server-authorized card transfer, then mutates each transfer projection field to prove the C++ boundary catches forged settlement audit material.
- This remains replay/result audit projection only; the C++ battle server still does not write inventory, rewards, wallets, Steam state, or databases.

## 2026-06-30 Boss Result Projection Verification Boundary

- `BattleServer::SubmitBattleResult` now binds Boss settlement projection fields back to the server-owned final replay snapshot before accepting a signed result callback.
- `BattleResultVerifier` rejects tampered Boss scope, completion policy, current HP, damage total, defeated flag, clear status, and result disposition fields with dedicated mismatch reasons.
- Development `mode_result_json` also carries per-player `boss_damage_<player_id>` contribution fields from the authoritative final snapshot for business-side audit visibility.
- CTest and `tools/check_battle_server.py` now gate these Boss result projection checks. The C++ battle server still emits projection-only settlement material and does not write inventory, rewards, wallets, or database state.

## 2026-06-30 Boss Transfer Card Authority Boundary

- Added an in-memory `TransferableCardState` authority bridge for Boss `transfer_card` mode actions before the full shared card/rules config path lands.
- `transfer_card` now rejects unknown card instances, owner mismatches, mode-forbidden cards, unpaid costs, and blocked cooldowns before the mode action enters the authoritative tick queue or consumes replay/hash state.
- Accepted transfers freeze the server authority snapshot into mode state, replay trace/hash material, and development mode-result projection so Nakama/Go can later audit why the battle server accepted the handoff.
- Boss mode result projection now carries `boss_result_disposition`, distinguishing world Boss damage reports from instance Boss cleared/incomplete outcomes without mutating rewards or persistence.
- The server facade exposes match-bound transferable-card configuration for battle allocation/bootstrap wiring only; it does not persist inventory, rewards, wallets, databases, or Steam-owned state.
- CTest and `tools/check_battle_server.py` now gate the new transfer-card authority reasons while existing deterministic replay/hash fixtures stay unchanged.

## 2026-06-30 Negotiated Encrypted Session Boundary

- `BattleServer::DispatchEncrypted` now routes inbound encrypted packets through a negotiated-session validation helper before dispatcher seq/nonce state can advance.
- The validation keeps ticket registration separate from real session readiness: an accepted handshake must leave transcript material, a supported ChaCha20-Poly1305-compatible AEAD label, non-empty directional key refs, and distinct client-to-server/server-to-client key material.
- CTest and `tools/check_battle_server.py` now gate this scaffold through inbound server-to-client key rejection and implementation fingerprints for transcript, AEAD, and directional-key checks while real X25519/HKDF/AEAD remains pending.

## 2026-06-30 Decoded Reconnect Adapter Boundary

- `BattleServer::AcceptDecodedReconnectModeAction` now models the future protobuf decode handoff for reconnect packets separately from normal mode actions.
- `DecodedBattlePacketAdapter` accepts reconnect packets only after encrypted session/key/nonce/event-cursor dispatch succeeds, then requires a decoded `reconnect` mode action whose version, match id, player id, tick, seq, and `last_seen_event_cursor` payload agree with the encrypted header before recovery is accepted.
- Decoded reconnect packets now restore the server facade connection state immediately while still queuing the reconnect mode action into the authoritative tick/replay stream, so transport recovery can resume packet flow without letting malformed protobuf-replacement payloads consume nonce/seq state.
- CTest and `tools/check_battle_server.py` now gate missing reconnect decoded payloads, reconnect header/action mismatches, rejected non-reconnect action types, missing/mismatched reconnect cursors, future event cursor rejection, immediate connection-state recovery, and tick-bound replay recording while real protobuf/KCP/AEAD remains pending.

## 2026-06-30 Decoded Encrypted-Dispatch Marker

- `DecodedBattlePacketResult` now exposes `encrypted_dispatch_accepted` so tests and future protobuf decode wiring can distinguish encrypted session/nonce/tick acceptance from decoded payload acceptance.
- `DecodedBattlePacketAdapter` sets the marker only after `BattleServer::DispatchEncrypted` succeeds; missing handshake, bad session key, stale tick, or other encrypted-path failures still short-circuit before decoded input or mode-action handoff.
- CTest and `tools/check_battle_server.py` now gate the marker, accepted-dispatch/missing-payload cases, and a no-handshake decoded packet rejection that preserves simulation replay counters.

## 2026-06-30 KCP Datagram Shape Boundary

- The development KCP/AEAD adapter now rejects datagrams with missing remote endpoint identity or empty UDP/KCP payload before calling the battle server encrypted dispatcher.
- Rejected malformed transport frames increment `malformed_datagrams`, do not bind a session endpoint, do not consume dispatcher seq/nonce state, and do not reach the placeholder KCP endpoint.
- CTest and `tools/check_battle_server.py` now gate the malformed datagram counters and rejection reasons while the real KCP event loop and AEAD decrypt path remain pending.

## 2026-06-30 Replay Record Bridge Boundary

- Added `ReplayRecordBridge` and `BattleServer::BuildReplayRecord` as a dependency-light `ReplayRecord` protobuf replacement target before generated C++ bindings land.
- The bridge binds the server-owned replay id, owner user id, match/mode/stage ids, ticket-derived replay loadout refs, manifest-compatible replay stream summary, server-built signed battle result settlement, server-authoritative flags, created-at timestamp, canonical payload, and deterministic development record hash.
- CTest and `tools/check_battle_server.py` now gate the bridge API, canonical payload, pinned record hash, loadout/deck snapshot material, replay owner tamper sensitivity, stream/settlement match/mode/replay/authority tamper sensitivity, and `ReplayRecord.loadout/stream/settlement/server_authoritative` manifest fields. This remains export/scaffold material only and does not persist replay rows, rewards, inventory, wallet, or database state.

## 2026-06-30 Signed Result Tick-Rate Boundary

- Development signed-result `mode_result_json` now carries explicit `tick_rate_hz` from the canonical replay fixture, currently pinned to the 60Hz authoritative simulation contract.
- `BattleServer::SubmitBattleResult` binds result verification to the replay fixture tick rate, and `BattleResultVerifier` rejects missing or forged tick-rate material as `tick_rate_hz_mismatch`.
- CTest and `tools/check_battle_server.py` gate the tick-rate field, verifier option, server binding, callback fingerprint, and negative tamper coverage while full protobuf result bindings remain pending.

## 2026-06-30 Decoded Packet Adapter Boundary

- Added `DecodedBattlePacketAdapter`, a development replacement point for the future AEAD decrypt + generated protobuf decode path.
- The adapter requires encrypted session/nonce/tick dispatch to pass before decoded input or mode-action handoff, then requires the decoded payload kind to match the packet header before simulation state changes.
- CTest and `tools/check_battle_server.py` now gate accepted decoded input/mode-action flow, missing decoded payload rejection, unsupported decoded payload type rejection, encrypted dispatch failure short-circuiting, and replay counter preservation.

## 2026-06-30 Development Result Signature Boundary

- Development signed-result verification now requires the Ed25519-shaped signature field to match deterministic material derived from `CanonicalBattleResultPayload(result)`, the battle server key id, and the current result-signature label.
- `BattleServer::BuildSignedBattleResult` uses the shared `DevBattleResultSignatureHex` helper, and `SubmitBattleResult` rejects stale or forged payload/signature combinations as `dev_result_signature_mismatch`.
- This is still a development adapter seam, not real Ed25519; it narrows the placeholder so canonical result payload tampering cannot pass with only a hex-shaped signature while production signing remains pending.

| Area | Status | Notes |
| --- | --- | --- |
| Repository skeleton | Started | CMake C++17 project, README, architecture note, source/include layout, CLI entrypoint, tests, and checker are present. `python tools\check_battle_server.py --build` passes locally with CMake/MSVC and CTest. |
| Protocol boundary | Started | Local structs mirror the v0.1 `PhK-Protocol` battle ticket/header/encrypted-packet/input/result/replay concepts until generated protobuf C++ bindings are wired. The repository now includes `../PhK-Protocol/gen/cpp` in CMake, consumes `phk/v1/manifest.hpp` for protocol/business/battle/ruleset constants, validates generated message-field gates and local payload enum numeric compatibility in CTest, and the checker verifies both the shared descriptor and C++ manifest remain in sync. Descriptor gates now pin the `BattlePayloadType` enum values plus key field numbers/types for packet nonce, encrypted ciphertext/auth tag, X25519/random/transcript bytes, mode-action/result JSON bytes, and signed-result signature bytes. Manifest gates cover signed tickets, handshake hello/accept, encrypted packets, mode actions, snapshots, battle events, signed results, and replay summaries including replay id, owner user id, event-stream hash, and final tick fields in addition to the basic ticket/header/input/result fields. |
| Ticket verification | Started | Development verifier checks ticket binding, expiry, nonce/hash shape, key id, Ed25519 public-key/signature shape, and rejects raw bearer-like business sessions. Real Ed25519 verification is pending. |
| Battle result submission | Started | Development `SignedBattleResult` boundary and verifier check registered match/mode/ruleset binding, player-id set, local replay-derived result hash, replay id, replay event cursor, final tick, input/fallback/mode-action counters, replay trace counts, input stream hash, event stream hash, final state hash, canonical replay fixture hash, projection-only reward shape, settled time, key id, Ed25519 signature shape, and idempotent result hash replay through the server facade after structural verification. The server facade now builds replay-bound development signed-result callback material from a canonical replay fixture and uses the simulation boundary's single mode-result JSON generator before verifying/submitting through the same boundary; tests tamper server-built callbacks instead of reconstructing callback JSON. The battle server still emits only verifiable result material and rejects obvious inventory, wallet, grant, balance, database, or Steam inventory mutation fields; it does not write inventory, rewards, wallet, or database state. Real result signing, protobuf serialization, and service-to-service submission are pending. |
| Battle handshake | Started | Placeholder handshake re-verifies registered ticket structure/expiry, rejects missing client key/random material or unsupported AEAD lists, selects ChaCha20-Poly1305/XChaCha20-Poly1305 labels, creates deterministic dev session metadata, derives development client-to-server/server-to-client key refs, and emits Ed25519-shaped transcript signature material. Real X25519/ECDHE, HKDF, transcript signing, and server signature verification are pending. |
| Transport | Started | KCP/UDP endpoint boundary exists with an echo placeholder plus a server-backed development KCP/AEAD adapter that dispatches encrypted packet/session/tick/reconnect guards before forwarding accepted datagrams to the placeholder endpoint. Real KCP event loop, retransmission tuning, protobuf decode, AEAD decrypt, and reconnect path are pending. |
| Packet dispatch | Started | Dispatcher validates protocol version, payload type, encrypted-path key id/nonce/ciphertext/auth-tag shape, header-bound development nonce derivation, encrypted adapter nonce reuse, seq replay, tick sanity, and rejects client-authored battle results. Generated protobuf dispatcher, real protobuf decoding, and AEAD verification are pending. |
| Decoded payload handoff | Started | Server facade now exposes development `AcceptDecodedInput`, `AcceptDecodedModeAction`, and reconnect-specific `AcceptDecodedReconnectModeAction` adapter points for the future protobuf decode path. They bind decoded local structs to the already-validated encrypted packet header's version stamp, payload type, match id, player id, tick, and seq before simulation state or replay counters can advance; reconnect packets must decode to a `reconnect` mode action before authoritative recovery is queued. Real protobuf decode and AEAD decrypt remain pending. |
| Authoritative simulation | Started | Added a dependency-light C++ simulation slice behind the server facade: fixed `kBattleTickRateHz = 60`, capacity-checked rooms, match-bound `mode_id`/`ruleset_version`, authoritative `BattleInput` validation, per-player seq replay and max-jump rejection, registered-player facade guards, input tick window checks, per-player duplicate input/tick rejection before replay hash state advances, player disconnect/reconnect state with disconnected-input rejection, tick-bound reconnect intent recovery, connected-player snapshot counts, and cursor-aware reconnect snapshots, milli-unit movement, simplified deterministic radial bullet spawning/movement, `BattleSnapshot` mirror structs, canonical FNV-1a state hash, replay summary input/event/final hashes, deterministic input/event trace material, manifest-compatible replay input summary record material, and development replay fixture material that binds replay id, owner user id, ordered players, event cursor, final snapshot, trace material, tick rate, and result hash. Full hit/graze/card/Boss scoring is still pending. |
| Tests | Started | CTest smoke validates protocol manifest consumption, stricter descriptor/manifest field gates, payload enum numeric compatibility, ticket verification, ticket replay rejection, mode/ruleset binding, room capacity, registered-ticket handshake acceptance/rejection, dev transcript key/signature material, battle result submission/idempotency plus hash/replay/cursor/final-tick/replay-count/projection-only guards, simulation determinism, a 1v1 60-tick authoritative replay/hash fixture, replay fixture trace and manifest-compatible replay-summary record material, invalid input/mode-action branches, duplicate player/tick input rejection, seq window rejection, server-side authoritative input/snapshot/replay summary calls, disconnect/reconnect snapshot/cursor, disconnected-input and disconnected-non-reconnect mode-action guards, tick-bound reconnect action recovery, KCP placeholder echo, KCP/AEAD adapter dispatch-before-forward behavior, encrypted packet key/nonce/ciphertext/auth-tag shape guards, and dispatcher result rejection. The repository checker now handles Visual Studio multi-config builds with `--config Debug` and asserts result/simulation/server/handshake/protocol/KCP adapter authority boundaries exist. |

## 2026-06-29 Header-Bound Development AEAD Nonce Boundary

- Added `DevAeadNonceHex`, a deterministic development nonce derivation helper for the KCP/AEAD adapter scaffold that binds nonce material to direction, key id, match/player identity, payload type, tick, seq, and ack.
- The dispatcher now rejects encrypted-path packet headers whose nonce is only hex-shaped but not derived from the packet header, before seq or nonce replay state advances. Existing duplicate nonce replay rejection remains in place after successful header binding.
- CTest coverage now uses derived nonces for valid encrypted input/mode-action/ping/reconnect paths and explicitly rejects `nonce_mismatch` cases. `tools/check_battle_server.py` gates the helper, mismatch reason, and focused test coverage. This remains development scaffolding until real ChaCha20-Poly1305 nonce construction and AEAD verification replace it.

## 2026-06-29 Decoded Header/Payload Binding Boundary

- Added server-facade `AcceptDecodedInput` and `AcceptDecodedModeAction` handoff methods for the future generated protobuf decode path.
- The handoff binds decoded local payloads to the packet header's version stamp, payload type, `match_id`, `player_id`, `tick`, and `seq` before calling the authoritative simulation; mismatches are rejected without advancing replay input counters or queued mode-action state.
- CTest coverage proves accepted decoded inputs still reach the simulation, mismatched decoded input/mode-action payloads are rejected, and accepted decoded mode actions remain tick-bound. `tools/check_battle_server.py` gates the adapter API, mismatch reasons, and focused test coverage.

## 2026-06-29 Replay-Bound Mode Result JSON Boundary

- Added `DevModeResultJsonFromReplayFixture` so development `mode_result_json` is generated from the same canonical replay fixture that produces `replay_summary_hash` and `replay_fixture_hash`.
- `BattleServer::BuildSignedBattleResult` and `SubmitBattleResult` now share one `ReplayFixture` instance for callback material and verification options, avoiding drift between summary hash, fixture hash, and mode-result JSON construction.
- CTest now uses server-built signed-result callbacks as the positive baseline and only mutates those callbacks for negative result-verifier cases; `tools/check_battle_server.py` rejects resurrecting test-side `ModeResultJsonForSummary`/`MakeBattleResultForSummary` helpers.

Verification sample:
- `python3 tools/check_battle_server.py` passes.
- Direct host `g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -I../PhK-Protocol/gen/cpp src/handshake.cpp src/kcp_endpoint.cpp src/protocol.cpp src/result.cpp src/server.cpp src/simulation.cpp src/ticket.cpp tests/battle_server_tests.cpp -o /tmp/phk_battle_tests && /tmp/phk_battle_tests` passes.
- Docker regression passes with legacy Compose: `docker-compose run --rm test`.
- `env HOME=/root GOCACHE=/tmp/go-build-cache python3 /root/gotouhou/docs/ops/protocol_audit_check.py` passes across PhK-Protocol, Gensoulkyo, and PhK-BattleServer.

## 2026-06-29 Encrypted Ping Ack Boundary

- Server-facade encrypted `ping` packets now share the same snapshot-ack ceiling as encrypted input and mode-action packets: `ack` cannot be greater than the authoritative simulation tick.
- The rejection occurs before dispatcher seq/nonce state advances, so a corrected ping with the same seq/nonce can still be accepted once the ack is valid.
- CTest coverage and `tools/check_battle_server.py` now gate this ping ACK boundary while ping remains a development encrypted-session scaffold before real protobuf/KCP/AEAD decoding lands.

## 2026-06-29 Signed Result Replay Fixture Hash Boundary

- Signed-result callback material now carries `replay_fixture_hash` in `mode_result_json`, derived from the canonical development replay fixture payload rather than only the aggregate replay summary.
- `BattleResultVerifier` binds that fixture hash to the server-owned simulation fixture before accepting a result callback, rejecting forged callbacks with `replay_fixture_hash_mismatch` even when result hash, replay id, counters, and stream hashes are otherwise present.
- CTest pins the build-signed-result callback fixture hash `sha256:dev-fnv64-4e23b1e341f35e87` and the mode-action submission fixture hash `sha256:dev-fnv64-4e12f244398ab1eb`; `tools/check_battle_server.py` gates the verifier option, server callback JSON, mismatch reason, and focused test coverage.

## 2026-06-29 Replay Summary Hash Boundary

- Added `DevReplayInputStreamSummaryHash` for the manifest-compatible `ReplayInputStreamSummary` bridge record, giving the dependency-light protobuf replacement boundary its own compact digest before full generated C++ bindings land.
- Server-built signed-result callback material now includes `replay_summary_hash` in `mode_result_json` and `SubmitBattleResult` requires it to match the current server-owned replay summary record.
- CTest pins the owner-bearing fixture record hash `sha256:dev-fnv64-2a7544832ca5ff92` and the ownerless signed-result callback record hash `sha256:dev-fnv64-f286e5b4976a50da`, and rejects tampered callbacks with `replay_summary_hash_mismatch`.

## 2026-06-29 Protobuf Shape Gate Boundary

- The repository checker now validates the exported `PhK-Protocol` descriptor beyond message/field presence: `BattlePayloadType` enum values are pinned to the C++ payload enum, and key protobuf field numbers/types are checked for packet nonce, encrypted ciphertext/auth tag, handshake X25519/random/transcript bytes, mode-action/result JSON byte fields, and signed-result signature bytes.
- CTest now pins the local `BattlePayloadType` numeric values to the v0.1 protobuf enum order so generated protobuf replacement cannot silently reinterpret packet payload types.
- This remains a dependency-light migration gate; real generated protobuf C++ serialization/decoding is still pending.

Verification sample:
- `python3 tools/check_battle_server.py` passes.
- Direct host `g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -I../PhK-Protocol/gen/cpp src/handshake.cpp src/kcp_endpoint.cpp src/protocol.cpp src/result.cpp src/server.cpp src/simulation.cpp src/ticket.cpp tests/battle_server_tests.cpp -o /tmp/phk_battle_tests && /tmp/phk_battle_tests` passes, including payload enum pinning.
- Docker regression passes with legacy Compose: `docker-compose run --rm test`.
- `env HOME=/root GOCACHE=/tmp/go-build-cache python3 /root/gotouhou/docs/ops/protocol_audit_check.py` reaches and passes PhK-BattleServer checks, but exits nonzero in Gensoulkyo: `TestBattleServerOfflineLifecycleSkipsFutureAllocations` expected offline lifecycle audit status to remain allocation-free, while the sampled status had `AllocationRecords:1` and `LastSuccessOperation:battle_ticket`.

## 2026-06-29 Tick-Bound Reconnect Intent Boundary

- `reconnect` mode actions now have a narrow disconnected-player exception: disconnected players remain blocked from battle input and normal mode actions, but may queue a `reconnect` intent inside the same seq/tick/action validation path.
- The reconnect intent restores connected state only when the authoritative simulation advances the target tick, then records both the connection event and mode-action event in deterministic replay/event trace material.
- CTest coverage proves disconnected card actions are still rejected, reconnect intent is invisible until its tick is processed, snapshots return to connected counts after the tick, and replay event cursors include the recovery path while real protobuf/KCP reconnect payload decoding remains pending.

## 2026-06-29 Signed Result Replay Digest Boundary

- Signed-result callback material now carries `input_stream_hash`, `event_stream_hash`, and `final_state_hash` in `mode_result_json` alongside replay counters and trace counts.
- `BattleResultVerifier` binds those three digest fields to the server-owned `ReplaySummary`, rejecting result callbacks that preserve result hash/replay id while advertising stale or forged replay digest material.
- CTest coverage now rejects mismatched input stream, event stream, and final state hashes; `tools/check_battle_server.py` gates the digest options, server callback JSON, verifier mismatch reasons, and focused test coverage while real protobuf result bindings remain pending.

## 2026-06-29 Encrypted Client Payload Boundary

- Server-facade encrypted packet dispatch now rejects server-to-client or handshake payload types (`snapshot`, `event`, `handshake_hello`, `handshake_accept`) with `encrypted_payload_type_invalid` before dispatcher seq/nonce state advances.
- The facade allowlist is limited to client-to-server encrypted payloads: `input`, `mode_action`, `ping`, and `reconnect`; client-authored `result` packets remain rejected as `client_result_forbidden`.
- CTest coverage proves rejected event/snapshot attempts do not consume the next valid seq/nonce, and `tools/check_battle_server.py` now gates the facade allowlist while full protobuf/KCP decoding remains pending.

## 2026-06-29 Canonical Replay Fixture Payload Boundary

- Added `CanonicalReplayFixturePayload` and `DevReplayFixtureHash` so the development replay fixture has one canonical protobuf-replacement payload that binds replay id, owner, match/mode/ruleset, result hash, tick rate, event cursor, server-authoritative flag, manifest-shaped replay summary record, final snapshot hash/cursor, ordered players, and input/event traces.
- CTest now pins the fixture hash `sha256:dev-fnv64-54919460e75ba83d` for the 60Hz 1v1 replay fixture and proves tampering the summary record, final snapshot, trace material, or server-authoritative flag changes the fixture hash.
- `tools/check_battle_server.py` gates the canonical fixture payload/hash API and focused tamper coverage while full generated protobuf replay bindings remain pending.

## 2026-06-29 Final Snapshot Fixture Body Boundary

- Tightened the canonical replay fixture payload so the development fixture hash binds the final snapshot body, not only the final snapshot hash/cursor. The payload now serializes sorted final player snapshots, bullet deltas, and mode-state key/value material alongside the existing replay summary record and input/event traces.
- The 60Hz 1v1 replay fixture hash is now pinned at `sha256:dev-fnv64-f2df27561abbe64e`; tests prove tampering a final player position, bullet pattern id, or mode id changes the fixture hash even when the aggregate summary fields are otherwise preserved.
- `tools/check_battle_server.py` now gates the canonical snapshot payload path and the added tamper coverage while this remains a development protobuf-replacement boundary.

## 2026-06-29 KCP/AEAD Adapter Boundary

- Added `KcpAeadPacketAdapter`, a development transport adapter that accepts a `BattleEncryptedPacket` plus UDP datagram, calls `BattleServer::DispatchEncrypted` first, and only forwards accepted packets to the current KCP placeholder endpoint.
- Rejected encrypted packets now leave KCP endpoint datagram counters unchanged and produce no replies on this adapter path, keeping failed session key, nonce replay, tick-window, and reconnect-cursor checks outside the transport work queue until real KCP/AEAD decrypt/decode lands.
- CTest coverage now checks session-key rejection, accepted forwarding, nonce replay rejection after acceptance, and endpoint stat behavior. `tools/check_battle_server.py` gates the adapter type and dispatch-before-forward implementation.

## 2026-06-29 Encrypted Disconnect Boundary

- Added a simulation-level connected-state query and wired it into `BattleServer::DispatchEncrypted` for encrypted input and mode-action packets.
- Disconnected players are now rejected with `encrypted_player_disconnected` before dispatcher seq/nonce state advances, keeping the KCP/AEAD scaffold aligned with the authoritative reconnect boundary while protobuf decoding remains pending.
- CTest coverage now checks the disconnected encrypted-input rejection and reconnection recovery path in `ServerEncryptedPacketSessionBoundary`; `tools/check_battle_server.py` gates the new boundary.

## 2026-06-29 Replay Summary Manifest Boundary

- Added `ReplayInputStreamSummaryRecord` and `BattleSimulation::BuildReplayInputStreamSummary`, a dependency-light record shaped to the generated `PhK-Protocol` `ReplayInputStreamSummary` fields: version, replay id, owner user id, match id, input/event counts, input/event stream hashes, final state hash, and final tick.
- `ReplayFixture` now embeds that record alongside the final authoritative snapshot and ordered trace material, so protobuf replacement has a concrete record boundary instead of only aggregate C++ structs.
- CTest now checks the record against the generated manifest field gates and canonical serialization/tamper behavior. `tools/check_battle_server.py` gates replay id, owner user id, event-stream hash, and the record builder while full protobuf C++ bindings remain pending.

## 2026-06-29 Handshake-Bound Encrypted Session Key Boundary

- Server-facade encrypted packet dispatch now requires a registered session to complete handshake before encrypted input/mode-action/ping/reconnect packets are accepted.
- Accepted handshakes promote the session from the temporary ticket signing key id to the development `client_to_server` key reference and retain the `server_to_client` key reference, selected AEAD, and transcript hash as session material for the future X25519/HKDF replacement.
- CTest coverage now rejects encrypted packets before handshake as `handshake_required`, rejects reuse of the ticket signing key after handshake as `session_key_mismatch`, and accepts packets only when the header key id matches the handshake-derived inbound key reference. `tools/check_battle_server.py` gates this boundary while real AEAD/protobuf remain pending.

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

- Current encrypted disconnect boundary sample: `python3 tools/check_battle_server.py` passes.
- Current encrypted disconnect boundary sample: direct `g++ -std=c++17 -Iinclude -I../PhK-Protocol/gen/cpp tests/battle_server_tests.cpp src/ticket.cpp src/handshake.cpp src/kcp_endpoint.cpp src/protocol.cpp src/result.cpp src/server.cpp src/simulation.cpp -o /tmp/phk_battle_tests && /tmp/phk_battle_tests` passes, including disconnected encrypted-input rejection before dispatcher state advances.
- Current encrypted disconnect boundary sample: `docker-compose run --rm test` passes with a clean container CMake build and CTest run.
- Current encrypted disconnect boundary sample: `env HOME=/root GOCACHE=/tmp/go-build-cache python3 /root/gotouhou/docs/ops/protocol_audit_check.py` passes across PhK-Protocol, Gensoulkyo, and PhK-BattleServer.
- Current KCP/AEAD adapter sample: `python3 tools/check_battle_server.py` passes and gates the adapter dispatch-before-forward boundary.
- Current KCP/AEAD adapter sample: direct `g++ -std=c++17 -Iinclude -I../PhK-Protocol/gen/cpp tests/battle_server_tests.cpp src/ticket.cpp src/handshake.cpp src/kcp_endpoint.cpp src/protocol.cpp src/result.cpp src/server.cpp src/simulation.cpp -o /tmp/phk_battle_tests && /tmp/phk_battle_tests` passes, including `KcpAeadPacketAdapterBoundary`.
- Current KCP/AEAD adapter sample: `docker-compose run --rm test` passes with a clean container CMake build and CTest run.
- Current KCP/AEAD adapter sample: `env HOME=/root GOCACHE=/tmp/go-build-cache python3 /root/gotouhou/docs/ops/protocol_audit_check.py` reaches and passes PhK-BattleServer checks, but the cross-repo audit currently fails in Gensoulkyo before completion: `runtime/core/service.go:4622:164: signed.Result.SettledAt undefined (type BattleResult has no field or method SettledAt)`.
- Current pinned 60Hz replay/result fingerprint sample: `python3 tools/check_battle_server.py` passes and now gates the fixed 60-tick authoritative replay hashes, canonical replay-summary record, and development signed-result canonical payload fingerprints.
- Current pinned 60Hz replay/result fingerprint sample: `docker-compose --profile test run --rm test` passes with a clean container CMake build and CTest run.
- Current pinned 60Hz replay/result fingerprint sample: `python3 /root/gotouhou/docs/ops/protocol_audit_check.py` passes across PhK-Protocol, Gensoulkyo, and PhK-BattleServer.
- Host `python3 tools/check_battle_server.py --build` remains blocked because this host does not have `cmake`; container regression covers the CMake/CTest path.
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
- Current final snapshot fixture body sample: `python3 tools/check_battle_server.py` passes.
- Current final snapshot fixture body sample: direct `g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -I../PhK-Protocol/gen/cpp src/handshake.cpp src/kcp_endpoint.cpp src/protocol.cpp src/result.cpp src/server.cpp src/simulation.cpp src/ticket.cpp tests/battle_server_tests.cpp -o /tmp/phk_battle_tests && /tmp/phk_battle_tests` passes, including final snapshot player/bullet/mode-state fixture tamper checks.
- Current final snapshot fixture body sample: `docker-compose run --rm test` passes with a clean container CMake build and CTest run. The host still has legacy `docker-compose` and no Docker Compose v2 `docker compose --profile` support.
- Current final snapshot fixture body sample: `python3 tools/check_battle_server.py --build` remains blocked on the host because `cmake` is not installed; Docker covers the CMake/CTest path.
- Current final snapshot fixture body sample: `env HOME=/root GOCACHE=/tmp/go-build-cache python3 /root/gotouhou/docs/ops/protocol_audit_check.py` reaches and passes PhK-BattleServer checks, but the cross-repo audit exits nonzero in Gensoulkyo: `TestBattleLifecycleAuditRepositoryReceivesAllocationTicketResultAndReplayRecords` expected one allocation audit and observed both server registration and allocation audit rows.

## 2026-06-28 Verification

- `python3 PhK-BattleServer/tools/check_battle_server.py` passes.
- Direct `g++ -std=c++17 ... /tmp/phk_battle_tests` build and `/tmp/phk_battle_tests` pass in the current Linux container.
- CMake/CTest could not be run in the current container because `cmake` is not installed; the source remains wired in `CMakeLists.txt` for the normal repository build path.
- The current simulation smoke includes mode/ruleset snapshot and replay projection, room capacity rejection, registered-ticket handshake rejection, handshake shape rejection, disconnected player snapshot projection, disconnected-input rejection, reconnect restoration and cursor bounds, invalid direction bits, invalid card slot, seq jump rejection, missing mode-action fields, far-future mode-action rejection, registered-player guards, and battle-result mode/ruleset/hash/replay/cursor mismatch rejection.
