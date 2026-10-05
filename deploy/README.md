# Deploying Phantasm Klash on the gateway host

This document covers the two server-side processes used for a match:

- **Gensoulkyo** — the Go lobby / authoritative service (matchmaking, tickets,
  result settlement). Lives in the sibling `../Gensoulkyo` repository.
- **PhK-BattleServer** — the C++ per-match battle server (this repository).
  The lobby spawns one short-lived process per match; it exits when the match
  settles.

> This file documents the build/launch contract only. It does **not** start any
> long-running service or install a systemd unit.

## Prerequisites

| Tool | Version | Notes |
| --- | --- | --- |
| cmake | >= 3.22 | server has 3.28 |
| ninja | >= 1.10 | server has 1.11 |
| g++ | C++17 | server has gcc 13.3 |
| go | >= 1.20 | server has 1.22 |

The C++ build is **self-contained**: the generated PhK-Protocol manifest is
vendored in `third_party/phk-protocol/gen/cpp`, so a `../PhK-Protocol` checkout
is optional (it is used automatically when present in the monorepo).

## 1. Build the C++ battle server

```sh
cd /root/gotouhou/PhK-BattleServer
scripts/build.sh                 # configure + build + ctest into build-linux/
```

Equivalent manual steps:

```sh
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-linux
ctest --test-dir build-linux --output-on-failure
```

Outputs:

- `build-linux/phk_battle_server` — the match server executable.
- `build-linux/libphk_battle_core.a` — the static core library.

Useful flags: `scripts/build.sh --no-test`, `--debug`, `--release`,
`BUILD_DIR=... scripts/build.sh`.

## 2. Build the Go lobby

```sh
cd /root/gotouhou/Gensoulkyo
go build -o bin/gensoulkyo ./cmd/gensoulkyo
```

## 3. Configure the lobby

Gensoulkyo reads its configuration from `GENSOULKYO_*` environment variables
(e.g. `GENSOULKYO_DATABASE_DRIVER`, `GENSOULKYO_DATABASE_URL`). The battle
server binary is pointed at with:

```sh
export GENSOULKYO_BATTLE_SERVER_BIN=/root/gotouhou/PhK-BattleServer/build-linux/phk_battle_server
```

The lobby launches this binary per match with arguments matching the CLI in
`apps/phk_battle_server/main.cpp`:

```
--port <P> --match-id <M> --seed <S> --players <a,b> \
--lobby <host:port> --ruleset <id> [--boss-hp <N>] [--max-ticks <N>]
```

The process prints `READY port=<P> match=<M>` on stdout once it is bound, and
exits after emitting `RESULT ...` / `SUBMIT ...`. It shuts down cleanly on
`SIGTERM` / `SIGINT`.

> `GENSOULKYO_BATTLE_SERVER_BIN` is the agreed convention for locating the
> binary; the launcher on the Gensoulkyo side must read the same variable.

## 4. Starting (manual)

```sh
cd /root/gotouhou/Gensoulkyo
GENSOULKYO_BATTLE_SERVER_BIN=/root/gotouhou/PhK-BattleServer/build-linux/phk_battle_server \
  ./bin/gensoulkyo
```

Do not install this as a managed system service from this repository; that is
handled by the deployment tooling elsewhere.

## 5. Verifying a build

```sh
# C++ core
cd /root/gotouhou/PhK-BattleServer && ctest --test-dir build-linux --output-on-failure

# C++ server smoke (ephemeral port, exits after the tick cap)
./build-linux/phk_battle_server --port 0 --match-id smoke --players a,b --max-ticks 1
```
