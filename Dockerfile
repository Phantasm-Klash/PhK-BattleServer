FROM debian:bookworm-slim AS test

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace/PhK-BattleServer

CMD ["python3", "tools/check_battle_server.py", "--build"]
