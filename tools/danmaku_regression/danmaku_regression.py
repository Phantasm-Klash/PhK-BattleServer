#!/usr/bin/env python3
"""Phantasm Klash 弹幕密度金标准回归台。

一句话：一条命令跑完 boss × difficulty × boss_hp 矩阵，把每个用例的密度指纹
和 `tools/danmaku_golden/<case>.json` 比，不一致就 exit 1。

    python tools/danmaku_regression.py            # 对比 golden
    python tools/danmaku_regression.py --update   # 重新生成 golden

设计要点
--------
* 指纹由 `tools/danmaku_fingerprint.py` 计算，**在网关上流式跑**。一份 300 tick
  的 trace 约 50 MB，24 个用例就是 1.2 GB；本地回归台只要几 KB 的指纹。
  原始 JSONL 留在网关 `/tmp/danmaku-regress/traces/`，加 `--fetch-traces`
  才会下载到 `tmp/danmaku-regression/traces/`。
* 二进制来自**独立构建目录 `build-regress`**，不碰 `build-linux`（那里可能有
  别的构建在跑）。
* 指纹里 `spawn_series_sha256` 是逐 tick 新增子弹数序列的哈希——比任何汇总
  数字都敏感，改一个 tick 的弹数就会翻。
* 差异不是噪声，是信号。只在**有意**改表/改运动学之后 `--update`。
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def _find_workspace_root() -> Path:
    """工作区根 = 含 `tools/rsh.sh` 的那一层。

    本脚本住在仓库里的 `PhK-BattleServer/tools/danmaku_regression/`（入库），但
    工作区布局是 `D:/gotouhou/tools/rsh.sh`，两者不在同一层 —— 所以显式搜索而
    不是假设相对深度，顺带允许 `PHK_WORKSPACE` 覆盖。
    """
    override = os.environ.get("PHK_WORKSPACE")
    candidates = [Path(override)] if override else []
    candidates += [HERE.parent.parent.parent, HERE.parent.parent, Path.cwd()]
    for candidate in candidates:
        if (candidate / "tools" / "rsh.sh").is_file():
            return candidate.resolve()
    return HERE.parent


ROOT = _find_workspace_root()
RSH = Path(os.environ.get("PHK_RSH", ROOT / "tools" / "rsh.sh"))
FINGERPRINT = HERE / "danmaku_fingerprint.py"
GOLDEN_DIR = HERE / "danmaku_golden"
WORK_DIR = ROOT / "tmp" / "danmaku-regression"

REMOTE_REPO = "/root/gotouhou/PhK-BattleServer"
REMOTE_BUILD = "build-regress"  # 不要改成 build-linux
REMOTE_BIN = f"{REMOTE_REPO}/{REMOTE_BUILD}/phk_danmaku_trace"
REMOTE_WORK = "/tmp/danmaku-regress"
REMOTE_CHAIN_NODE_HPP = f"{REMOTE_REPO}/include/phk/battle/danmaku/chain_node.hpp"

SEED = 12345
TICKS = 300
RULESET = "tna-1.40-danmaku"

# ---- 矩阵 ----------------------------------------------------------------
# 想加 boss / 难度 / 血量，改这三行就够了。
BOSS_IDS = ["boss21_tna", "boss21_tna_s20", "boss21"]
DIFFICULTIES = [0, 1, 2, 3]
# 血量三档：
#   6000 —— 正常值，300 tick 内打不掉也跨不过 66%，锁「稳定态」密度；
#   1200 —— 名义低值（团队口径），实测伤害只有 ~12/300 tick，**跨不过**阈值，
#           所以它实际锁的是「血量不影响密度」这条不变量；
#   24   —— 真正的阶段穿越值：300 tick 内依次跨过 66%/33% 两个阈值
#           （spell_phase 1→2），且对局仍在 running，专门覆盖符卡阶段切换。
BOSS_HPS = [6000, 1200, 24]

# 低血量的门槛：用来判定「会尽快跨过符卡阶段」的用例。
LOW_HP = 1200

# 阶段轮转断言的参数：boss21_tna 低血用例里必须同时出现这两张表的 pattern_id。
PHASE_BOSS_ID = "boss21_tna"
PHASE_PATTERN_IDS = ("chain:boss21_tna", "chain:boss21_tna_s20")
PHASE_CAPABILITY_SYMBOL = "BossStateBossIdForPhase"

# 表格/对比用的字段顺序。
METRIC_KEYS = [
    "bullets_total",
    "max_per_tick",
    "chains_records",
    "ticks_with_bullets",
    "trace_lines",
    "spell_phase_starts",
    "spell_phases",
    "pattern_ids",
    "spawn_series_sha256",
]


def build_matrix() -> list[dict]:
    cases = []
    for boss_id in BOSS_IDS:
        for difficulty in DIFFICULTIES:
            for boss_hp in BOSS_HPS:
                cases.append(
                    {
                        "case": f"{boss_id}-d{difficulty}-hp{boss_hp}",
                        "boss_id": boss_id,
                        "difficulty": difficulty,
                        "boss_hp": boss_hp,
                    }
                )
    return cases


def find_bash() -> str:
    """找 Git Bash 的 bash.exe。

    Windows 上裸 `bash` 常常解析到 WSL 的 shim（会被安全策略拦掉），所以优先
    显式路径，找不到再退回 PATH。
    """
    candidates = [
        os.environ.get("PHK_BASH", ""),
        r"C:\Program Files\Git\bin\bash.exe",
        r"C:\Program Files\Git\usr\bin\bash.exe",
        r"C:\Program Files (x86)\Git\bin\bash.exe",
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    found = shutil.which("bash")
    if found:
        return found
    raise SystemExit("[env] 找不到 bash.exe（Git Bash）。可用 PHK_BASH 环境变量指定。")


BASH = find_bash()


def rsh(args: list[str], timeout: int = 180, retries: int = 1) -> subprocess.CompletedProcess:
    """调 tools/rsh.sh。SSH 偶发抖动 → 失败重试一次。"""
    cmd = [BASH, str(RSH), *args]
    last: subprocess.CompletedProcess | None = None
    for attempt in range(retries + 1):
        last = subprocess.run(
            cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout
        )
        if last.returncode == 0:
            return last
        if attempt < retries:
            detail = (last.stderr or last.stdout or "").strip()[:200]
            sys.stderr.write(f"  [warn] rsh 失败（第 {attempt + 1} 次），重试：{detail}\n")
    assert last is not None
    return last


def ensure_remote_build() -> None:
    """增量构建 build-regress。

    每次都 build（不是只在缺二进制时）：回归台必须测**当前源码**，源码被别人改了
    却拿旧二进制跑，等于拿过期地图导航。
    """
    print(f"[build] 增量构建 {REMOTE_BUILD}/phk_danmaku_trace…")
    script = (
        f"cd {REMOTE_REPO} && "
        f"{{ [ -f {REMOTE_BUILD}/CMakeCache.txt ] || "
        f"cmake -S . -B {REMOTE_BUILD} -DCMAKE_BUILD_TYPE=Release; }} > /tmp/regress-cmake.log 2>&1 && "
        f"cmake --build {REMOTE_BUILD} -j4 --target phk_danmaku_trace > /tmp/regress-build.log 2>&1"
    )
    result = rsh([script], timeout=1800)
    if result.returncode != 0:
        sys.stderr.write((result.stdout or "")[-2000:] + "\n" + (result.stderr or "")[-2000:] + "\n")
        raise SystemExit("[build] 构建失败，见网关 /tmp/regress-build.log")


def phase_capability_present() -> bool:
    """服务端是否已实现「按符卡阶段轮转两张表」。

    判据：chain_node.hpp 里存在 BossStateBossIdForPhase。不存在 → 断言必须 SKIP，
    不能假装通过（否则就是假绿）。
    """
    probe = rsh([f"grep -q {PHASE_CAPABILITY_SYMBOL} {REMOTE_CHAIN_NODE_HPP} && echo YES || echo NO"], timeout=60)
    return "YES" in probe.stdout


def run_matrix(cases: list[dict], fetch_traces: bool) -> dict[str, dict]:
    WORK_DIR.mkdir(parents=True, exist_ok=True)
    local_fp = WORK_DIR / "fp"
    if local_fp.exists():
        shutil.rmtree(local_fp)

    lines = [
        "set -u",
        f"cd {REMOTE_REPO} || exit 1",
        f"mkdir -p {REMOTE_WORK}/fp {REMOTE_WORK}/traces",
        f"FP={REMOTE_WORK}/fingerprint.py",
        f"BIN=./{REMOTE_BUILD}/phk_danmaku_trace",
        "fail=0",
        "run() {",
        '  name="$1"; shift',
        f'  if ! "$BIN" --out "{REMOTE_WORK}/traces/$name.jsonl" "$@" > "{REMOTE_WORK}/traces/$name.log" 2>&1; then',
        '    echo "RUNFAIL $name"; fail=1; return',
        "  fi",
        f'  if ! python3 "$FP" "{REMOTE_WORK}/traces/$name.jsonl" > "{REMOTE_WORK}/fp/$name.json"; then',
        '    echo "FPFAIL $name"; fail=1; return',
        "  fi",
        '  echo "OK $name"',
        "}",
    ]
    for case in cases:
        lines.append(
            "run '{name}' --seed {seed} --ticks {ticks} --ruleset '{ruleset}' "
            "--boss-hp {hp} --boss-id '{boss}' --difficulty {diff}".format(
                name=case["case"],
                seed=SEED,
                ticks=TICKS,
                ruleset=RULESET,
                hp=case["boss_hp"],
                boss=case["boss_id"],
                diff=case["difficulty"],
            )
        )
    lines.append("exit $fail")

    run_script = WORK_DIR / "run_remote.sh"
    run_script.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")

    rsh([f"mkdir -p {REMOTE_WORK}/fp {REMOTE_WORK}/traces"], timeout=60)
    for local, remote in ((run_script, f"{REMOTE_WORK}/run.sh"), (FINGERPRINT, f"{REMOTE_WORK}/fingerprint.py")):
        uploaded = rsh(["-s", str(local), remote], timeout=120)
        if uploaded.returncode != 0:
            raise SystemExit(f"[upload] 上传失败 {local}：{uploaded.stderr.strip()}")

    print(f"[run] 网关执行 {len(cases)} 个用例…")
    executed = rsh([f"bash {REMOTE_WORK}/run.sh"], timeout=1800)
    ok = 0
    for line in executed.stdout.splitlines():
        line = line.strip()
        if line.startswith("OK "):
            ok += 1
        elif line.startswith("RUNFAIL ") or line.startswith("FPFAIL "):
            raise SystemExit(f"[run] {line}（网关 {REMOTE_WORK}/traces/ 有日志）")
    if ok != len(cases):
        raise SystemExit(f"[run] 只成功 {ok}/{len(cases)}：\n{executed.stdout}\n{executed.stderr}")
    print(f"[run] {ok}/{len(cases)} 完成")

    downloaded = rsh(["-g", f"{REMOTE_WORK}/fp", str(WORK_DIR)], timeout=300)
    if downloaded.returncode != 0:
        raise SystemExit(f"[fetch] 下载指纹失败：{downloaded.stderr.strip()}")

    if fetch_traces:
        print("[fetch] 下载原始 trace 到 tmp/danmaku-regression/traces（可能数百 MB）…")
        rsh(["-g", f"{REMOTE_WORK}/traces", str(WORK_DIR)], timeout=1800)

    fingerprints: dict[str, dict] = {}
    for case in cases:
        path = local_fp / f"{case['case']}.json"
        if not path.is_file():
            raise SystemExit(f"[fetch] 缺少指纹 {path}")
        fingerprints[case["case"]] = json.loads(path.read_text(encoding="utf-8"))
    return fingerprints


def golden_payload(case: dict, fp: dict) -> dict:
    payload = {
        "case": case["case"],
        "boss_id": case["boss_id"],
        "difficulty": case["difficulty"],
        "boss_hp": case["boss_hp"],
        "ticks": TICKS,
        "seed": SEED,
        "ruleset": RULESET,
    }
    for key in METRIC_KEYS:
        payload[key] = fp[key]
    return payload


def write_golden(cases: list[dict], fingerprints: dict[str, dict], prune: bool = True) -> None:
    GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
    for case in cases:
        payload = golden_payload(case, fingerprints[case["case"]])
        path = GOLDEN_DIR / f"{case['case']}.json"
        path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    if prune:
        # 清掉矩阵里已经不存在的旧 golden，避免「僵尸用例」悄悄通过。
        # --only 时不动：那次跑的是子集，删别人会误伤。
        names = {c["case"] + ".json" for c in cases}
        for path in sorted(GOLDEN_DIR.glob("*.json")):
            if path.name not in names:
                path.unlink()
                print(f"[golden] 删除过期用例 {path.name}")
    print(f"[golden] 写入 {len(cases)} 个文件到 {GOLDEN_DIR.relative_to(ROOT)}")


def load_golden(case_name: str) -> dict | None:
    path = GOLDEN_DIR / f"{case_name}.json"
    if not path.is_file():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


def compare(cases: list[dict], fingerprints: dict[str, dict], check_stale: bool = True) -> tuple[bool, list[str]]:
    rows: list[tuple[str, str, str, str, str, str, str]] = []
    diffs: list[str] = []
    failed = False

    for case in cases:
        name = case["case"]
        actual = fingerprints[name]
        expected = load_golden(name)
        if expected is None:
            rows.append((name, str(actual["bullets_total"]), str(actual["max_per_tick"]),
                         str(actual["chains_records"]), str(actual["ticks_with_bullets"]),
                         actual["spawn_series_sha256"][:12], "NEW"))
            diffs.append(f"{name}: golden 缺失（{GOLDEN_DIR / (name + '.json')}）—— 先跑 --update")
            failed = True
            continue

        field_diffs = []
        for key in METRIC_KEYS:
            if expected.get(key) != actual[key]:
                field_diffs.append(f"    {key}:\n      期望 {expected.get(key)}\n      实际 {actual[key]}")
        status = "OK" if not field_diffs else "DIFF"
        if field_diffs:
            failed = True
            diffs.append(f"{name}:")
            diffs.extend(field_diffs)
        rows.append((name, str(actual["bullets_total"]), str(actual["max_per_tick"]),
                     str(actual["chains_records"]), str(actual["ticks_with_bullets"]),
                     actual["spawn_series_sha256"][:12], status))

    names = {c["case"] for c in cases}
    for path in sorted(GOLDEN_DIR.glob("*.json")) if check_stale else []:
        if path.stem not in names:
            rows.append((path.stem, "-", "-", "-", "-", "-", "STALE"))
            diffs.append(f"{path.stem}: golden 存在但不在矩阵里 —— 确认要删就 --update")
            failed = True

    header = ("case", "bullets", "max/tick", "chains", "ticks", "spawn-sha", "status")
    widths = [max(len(r[i]) for r in [header, *rows]) for i in range(7)]
    print("")
    print("  ".join(h.ljust(w) for h, w in zip(header, widths)))
    print("  ".join("-" * w for w in widths))
    for row in rows:
        print("  ".join(c.ljust(w) for c, w in zip(row, widths)))
    print("")

    if diffs:
        print(f"差异 {len(diffs)} 处：")
        for line in diffs:
            print("  " + line if not line.startswith("    ") else line)
        print("")
    return (not failed), diffs


def check_phase_assertion(cases: list[dict], fingerprints: dict[str, dict], capability: bool) -> bool:
    """boss21_tna 低血用例必须同时出现两张表的 pattern_id。

    前提是该用例**真的跨过**了符卡阶段（spell_phase_starts > 0）。没跨阶段的用例
    谈不上「按阶段轮转」，打印 SKIP 而不是判红——但整张矩阵里必须至少有一个用例
    跨过阶段，否则断言等于没覆盖，直接判红。
    """
    targets = [c for c in cases if c["boss_id"] == PHASE_BOSS_ID and c["boss_hp"] <= LOW_HP]
    if not targets:
        print("[phase] 矩阵里没有低血 boss21_tna 用例，跳过阶段断言")
        return True
    if not capability:
        print(
            f"[phase] SKIP —— {REMOTE_CHAIN_NODE_HPP} 中不存在 {PHASE_CAPABILITY_SYMBOL}，"
            "服务端尚未实现「按符卡阶段轮转 boss21_tna / boss21_tna_s20 两张表」。\n"
            "        断言不成立即不判红（避免假绿），golden 仍照常生成。"
        )
        return True

    ok = True
    covered = 0
    for case in targets:
        fp = fingerprints[case["case"]]
        patterns = set(fp["pattern_ids"])
        if fp["spell_phase_starts"] == 0:
            print(f"[phase] SKIP {case['case']}：300 tick 内未跨过阶段阈值（spell_phases={fp['spell_phases']}），"
                  "无阶段可轮转")
            continue
        covered += 1
        missing = [p for p in PHASE_PATTERN_IDS if p not in patterns]
        if missing:
            ok = False
            print(f"[phase] FAIL {case['case']}：跨了 {fp['spell_phase_starts']} 次阶段但缺少 pattern_id "
                  f"{missing}（实际 {sorted(patterns)}）")
        else:
            print(f"[phase] PASS {case['case']}：跨 {fp['spell_phase_starts']} 次阶段，"
                  "两张表的 pattern_id 同时出现")
    if covered == 0:
        ok = False
        print("[phase] FAIL 没有任何低血用例跨过符卡阶段 —— 阶段断言形同虚设，请把 BOSS_HPS 里加一档真正能跨阈值的血量")
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description="Phantasm Klash 弹幕密度金标准回归台")
    parser.add_argument("--update", action="store_true", help="重新生成 tools/danmaku_golden/")
    parser.add_argument("--fetch-traces", action="store_true", help="同时下载原始 JSONL（很大）")
    parser.add_argument("--only", default="", help="只跑 case 名包含该子串的用例（调试用）")
    args = parser.parse_args()

    cases = build_matrix()
    if args.only:
        cases = [c for c in cases if args.only in c["case"]]
        if not cases:
            raise SystemExit(f"[matrix] --only {args.only!r} 没匹配到用例")
    print(f"[matrix] {len(cases)} 个用例（boss {len(BOSS_IDS)} × 难度 {len(DIFFICULTIES)} × 血量 {len(BOSS_HPS)}）"
          f"，ticks={TICKS} seed={SEED} ruleset={RULESET}")

    ensure_remote_build()
    capability = phase_capability_present()
    fingerprints = run_matrix(cases, args.fetch_traces)

    if args.update:
        write_golden(cases, fingerprints, prune=not args.only)
        check_phase_assertion(cases, fingerprints, capability)
        print("[done] golden 已更新")
        return 0

    green, _ = compare(cases, fingerprints, check_stale=not args.only)
    phase_ok = check_phase_assertion(cases, fingerprints, capability)
    if green and phase_ok:
        print("[done] 全绿")
        return 0
    print("[done] 不一致")
    return 1


if __name__ == "__main__":
    sys.exit(main())
