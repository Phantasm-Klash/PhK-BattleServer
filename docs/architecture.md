# PhK-BattleServer Architecture

Status: v0.1 skeleton.

## Runtime Role

`PhK-BattleServer` is the open-source C++ authority for high-frequency PVP and Boss battle simulation. Gensoulkyo allocates matches and signs short-lived battle tickets; SpellKard connects to the allocated battle endpoint and sends only input or intent packets.

## Module Layout

| Module | Responsibility |
| --- | --- |
| `phk/v1/manifest.hpp` | Generated dependency-light protocol manifest from `PhK-Protocol`. It currently supplies shared version/ruleset constants and message-field gates until full protobuf C++ bindings replace it. |
| `ticket` | Holds the battle ticket shape and development verifier. The current verifier checks Ed25519 key/signature shape and ticket binding, but does not perform production crypto yet. |
| `handshake` | Holds the ECDHE/AEAD handshake boundary. The current implementation derives deterministic development session ids and selects ChaCha20-Poly1305-compatible labels. |
| `kcp_endpoint` | Holds the KCP/UDP endpoint boundary. The current implementation is an echo placeholder for tests. |
| `protocol` | Holds battle packet headers and dispatcher guards until generated protobuf bindings are wired. |
| `simulation` | Holds the v0.1 deterministic battle-core slice: fixed 60Hz tick, match-bound mode/ruleset metadata, authoritative input and mode-action validation, player disconnect/reconnect state, milli-unit movement, simplified bullet generation/movement, canonical state hash, replay summary hashes, and lightweight last accepted mode-action projection. |
| `result` | Holds the battle result shape and development verifier. It checks match/mode binding, player ids, result hash, replay id, settled time, and Ed25519 signature field shape before Gensoulkyo accepts a battle result. |
| `server` | Composes ticket verification, capacity-checked session creation, registered-ticket handshake acceptance, packet dispatch, match simulation input/tick/snapshot calls, and idempotent battle result submission. |

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
- Battle input and mode actions must come from a player with a registered server session for that match.
- Client packets cannot submit battle results.
- Disconnected players cannot submit battle input or mode actions until the server facade marks them connected again.
- Battle results are bound to the allocated match id, frozen mode id, frozen ruleset version, player ids, result hash, replay id, settled time, and battle server key id before the business server can settle rewards.
- Client packet seq numbers must increase per `match_id:player_id`.
- Business session references must be opaque references, not raw bearer tokens.
