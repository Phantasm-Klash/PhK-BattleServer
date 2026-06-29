# PhK-BattleServer Architecture

Status: v0.1 skeleton.

## Runtime Role

`PhK-BattleServer` is the open-source C++ authority for high-frequency PVP and Boss battle simulation. Gensoulkyo allocates matches and signs short-lived battle tickets; SpellKard connects to the allocated battle endpoint and sends only input or intent packets.

## Module Layout

| Module | Responsibility |
| --- | --- |
| `phk/v1/manifest.hpp` | Generated dependency-light protocol manifest from `PhK-Protocol`. It currently supplies shared version/ruleset constants and message-field gates until full protobuf C++ bindings replace it. |
| `ticket` | Holds the battle ticket shape and development verifier. The current verifier checks Ed25519 key/signature shape and ticket binding, but does not perform production crypto yet. |
| `handshake` | Holds the ECDHE/AEAD handshake boundary. The current implementation checks hello key/random/AEAD shape, selects ChaCha20-Poly1305-compatible labels, derives deterministic development session/key references, and emits Ed25519-shaped transcript signature material. |
| `kcp_endpoint` | Holds the KCP/UDP endpoint boundary. The current implementation is an echo placeholder plus a server-backed encrypted packet adapter that gates AEAD/session/tick/reconnect checks before forwarding accepted datagrams to the placeholder endpoint. |
| `protocol` | Holds battle packet headers, encrypted packet adapter structs, and dispatcher guards until generated protobuf bindings are wired. It rejects client-authored result packets and now requires development key id/nonce/ciphertext/auth-tag shape plus nonce reuse rejection on encrypted-path packet types before accepting seq state. |
| `simulation` | Holds the v0.1 deterministic battle-core slice: fixed 60Hz tick, match-bound mode/ruleset metadata, authoritative input and mode-action validation, shared mode-action type gating, input seq/tick windows, tick-bound mode-action buffering/application, missing-input fallback audit counters, deterministic replay input/event trace material, manifest-compatible replay-summary record material, player disconnect/reconnect state, cursor-aware reconnect snapshots, milli-unit movement, simplified bullet generation/movement, canonical state hash, replay summary hashes, replay fixture material, and lightweight mode-action audit projection. |
| `result` | Holds the battle result shape and development verifier. It checks match/mode binding, player ids, result hash, replay id, replay event cursor, projection-only reward shape, settled time, and Ed25519 signature field shape before Gensoulkyo accepts a battle result. |
| `server` | Composes ticket verification, capacity-checked session creation, registered-ticket handshake acceptance, handshake-bound encrypted packet dispatch, match simulation input/tick/snapshot/reconnect calls, replay-bound development signed-result callback material, and idempotent battle result submission bound to the local replay summary. |

## Planned Production Replacements

1. Replace structural ticket checks with real Ed25519 verification.
2. Replace handshake placeholder with X25519 ECDHE, HKDF, transcript hashing, and server signatures.
3. Replace KCP echo placeholder with a real UDP/KCP event loop.
4. Replace the dependency-light C++ manifest plus local packet structs with generated `PhK-Protocol` C++ protobuf bindings.
5. Replace the structural battle result verifier with real Ed25519 signing/verification and submit the result to Gensoulkyo over the service-to-service channel.
6. Expand the current fixed-tick simulation from simplified movement/bullet snapshots into full card, Boss, hit/graze, scoring, snapshot/event encoding, replay input stream digests, and signed result submission back to Gensoulkyo.

## Security Rules

- A battle ticket is bound to `match_id`, `player_id`, `mode_id`, `battle_server_id`, endpoint, deck snapshot hash, ruleset version, nonce, and expiry.
- A match simulation freezes the first registered ticket's `mode_id` and `ruleset_version`; later tickets for the same match must match both.
- Match session count cannot exceed the configured battle-server `max_players`.
- Reused ticket ids are rejected by the server facade.
- Handshake acceptance re-verifies the ticket structure/expiry and requires the ticket to be registered in the server facade.
- Handshake hello messages must provide non-empty client key/random material and at least one supported ChaCha20-Poly1305-compatible AEAD label.
- Handshake accepts include deterministic development client-to-server and server-to-client key references plus an Ed25519-shaped server signature over the transcript hash. These are boundary fixtures for future X25519/HKDF/transcript signing and do not replace production cryptography.
- Encrypted-path packet dispatch is not enabled by ticket registration alone. A registered session must first complete the handshake; after acceptance, the facade replaces the temporary ticket signing key with the handshake-derived client-to-server key reference for inbound packet `key_id` checks, while retaining the server-to-client key reference as outbound session material.
- Battle input and mode actions must come from a player with a registered server session for that match.
- Battle input and mode-action seq numbers must increase within the configured server window; old, replayed, or implausibly jumped seq values are rejected.
- A player may queue only one input for a given authoritative tick. A later higher-seq packet for the same player/tick is rejected instead of replacing the buffered input, so replay hash material and applied simulation state cannot diverge.
- Mode actions are limited to the shared server-interface action surface: `cast_card`, `select_round_card`, `transfer_card`, `ready`, and `reconnect`. Unknown action types are rejected until full protobuf/native mode dispatch replaces this scaffold.
- Accepted mode actions are buffered by target tick and only folded into replay/event/hash state when the authoritative simulation advances that tick.
- Client packets cannot submit battle results.
- Disconnected players cannot submit battle input or mode actions until the server facade marks them connected again.
- Reconnect snapshots are full authoritative snapshots that include the server event cursor, requested cursor, and missed event count; a client cursor ahead of the server cursor is rejected as `event_cursor_ahead`.
- Encrypted-path battle packets must carry a key id, nonce-shaped packet header, non-empty ciphertext, 16-byte AEAD tag, and an input/mode-action/ping/reconnect payload type in the dispatcher scaffold before seq replay state advances. Reusing the same `match_id:player_id:key_id:nonce` on the encrypted adapter path is rejected even if the seq is new.
- Server-facade encrypted packet dispatch additionally requires the packet's `match_id`/`player_id` to resolve to a registered battle session that has completed handshake and the packet `key_id` to match that session's inbound handshake key reference. Input and mode-action encrypted packets must also stay inside the authoritative simulation tick window before dispatcher seq/nonce state changes. Reconnect packet acknowledgements cannot claim an event cursor beyond the server replay summary. Unknown sessions, missing handshake, mismatched key ids, stale ticks, far-future input ticks, or impossible reconnect cursors are rejected at the facade.
- The development KCP/AEAD adapter calls the server encrypted dispatcher before handing a datagram to the KCP placeholder. Rejected encrypted packets do not increment KCP endpoint stats or produce replies; accepted packets are the only path forwarded to the placeholder endpoint. This is a testable replacement point for the future real KCP event loop and AEAD decrypt/decode path.
- Development signed-result callback material is generated only from the server-owned replay summary: allocated match id, frozen mode id/ruleset, player ids, replay-derived result hash, replay id, event cursor, final tick, missing-input fallback counters, accepted mode-action count, deterministic input/event trace material, projection-only reward JSON, and battle server key id.
- Development replay fixtures are generated only from the server-owned simulation summary and final authoritative snapshot. They include replay id, owner user id, match/mode/ruleset, ordered player ids, result hash, event cursor, deterministic input/event trace material, final snapshot, 60Hz tick-rate metadata, a manifest-compatible `ReplayInputStreamSummary` record, and the server-authoritative flag so generated protobuf replay/result bindings can replace the scaffold without changing the tested boundary.
- Battle results are bound to the allocated match id, frozen mode id, frozen ruleset version, player ids, local replay-derived result hash, replay id, replay event cursor, settled time, and battle server key id before the business server can settle rewards.
- Battle result reward JSON is treated as a projection only. Obvious inventory, wallet, grant, balance, database, or Steam inventory mutation fields are rejected at the C++ boundary so Nakama/Go remains the only asset-settlement authority.
- The battle server never writes inventory, rewards, wallet, or database state. It only emits replay/hash/cursor-bound result material that the business server can verify and settle idempotently.
- Client packet seq numbers must increase per `match_id:player_id`.
- Business session references must be opaque references, not raw bearer tokens.
