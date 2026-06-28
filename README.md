# PhK-BattleServer

Open-source C++ battle server core for SpellKard / Phantasm Klash.

This repository is the real-time battle-side counterpart to:

- `PhK-Protocol`: shared protobuf/ruleset contract.
- `Gensoulkyo`: Nakama + Go Runtime business backend.
- `SpellKard`: Godot client.

## Current Scope

This is a v0.1 skeleton. It deliberately keeps production dependencies out until the shared protocol/codegen is frozen.

Implemented now:

- CMake C++17 project layout.
- Generated `PhK-Protocol` C++ manifest consumption for version/ruleset constants and message-field compatibility checks.
- Battle ticket data shape aligned with `PhK-Protocol` and Gensoulkyo fallback fields.
- Structural Ed25519 ticket verification guard for development.
- Explicit placeholder for X25519/ECDHE handshake and AEAD selection.
- Explicit placeholder for KCP/UDP endpoint behavior.
- Battle packet dispatcher guard for protocol version, seq replay, and forbidden client result submission.
- Fixed 60Hz authoritative simulation slice with input validation, milli-unit player movement, simplified bullet snapshots, canonical state hash, and replay summary hashes.
- Minimal server facade that verifies a ticket, creates a battle session, and dispatches packets.
- CTest smoke coverage and a Python repository checker.

Not implemented yet:

- Real Ed25519 verification.
- Real X25519/ECDHE and HKDF.
- Real ChaCha20-Poly1305 encryption.
- Real KCP transport.
- Generated protobuf bindings.
- Full bullet/card/Boss production simulation.
- Gensoulkyo/Nakama service callback transport.

## Build

```powershell
python ..\PhK-Protocol\tools\export_descriptor.py
python ..\PhK-Protocol\tools\export_cpp_manifest.py
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build --output-on-failure -C Debug
```

Or run the repository checker:

```powershell
python tools\check_battle_server.py --build
```

## Boundary

The C++ Battle Server must never grant assets, write inventory, call Steam APIs, or decide commercial rewards. It owns only high-frequency battle authority: input validation, tick simulation, snapshots, battle events, replay input stream digests, and signed battle results. Gensoulkyo validates and persists final results.
