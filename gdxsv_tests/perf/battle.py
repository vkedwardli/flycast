#!/usr/bin/env python3
"""Pre-release perf check: 2-peer local GGPO battle cells with the runtime trace, PresentMon on peer 1.

Peer 1 makes random-time dpad presses (GDXSV_PRESS_SWEEP); peer 2 plays a fixed test-joystick pattern, one input
change every PATTERN_FRAMES vblanks, so peer 1's rollbacks do not overlap. Input delay 2 (TEST_GGPO_DELAY).
Each cell: metrics from analyze.py -> <out>/<cell>_<run>.json; with --baseline, compare.py judges them and a
failing cell is re-run up to 2 more times and judged on the median.

usage: battle.py --exe flycast.exe [--cells all|20_tr1_vs1,...] [--runs N] [--baseline gdxsv_tests/perf/baseline.json]
                 [--out DIR] [--presentmon PresentMon.exe] [--write-baseline FILE]
Environment as run.py (GDXSV_ROM_DIR); GDXSV_STATE_DIR: the rollback suite's state dir; PRESENTMON: PresentMon 2.x exe.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
import threading
import time

PERF = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(PERF)
sys.path.insert(0, TESTS)
sys.path.insert(0, os.path.join(TESTS, "rollback"))
sys.path.insert(0, PERF)
import analyze  # noqa: E402
import compare  # noqa: E402
import rbk_lib  # noqa: E402

# name -> (send delay p1, p2 in ms, threaded rendering, vsync)
CELLS = {
    "20_tr0_vs0": (20, 20, "no", "no"),
    "20_tr0_vs1": (20, 20, "no", "yes"),
    "20_tr1_vs0": (20, 20, "yes", "no"),
    "20_tr1_vs1": (20, 20, "yes", "yes"),
    "30_170_tr0_vs1": (30, 170, "no", "yes"),
    "30_170_tr1_vs1": (30, 170, "yes", "yes"),
}
SETTLE, FRAMES = 180, 2100  # GGPO frames skipped while timesync converges, then measured (35 s)
RUN_S = 60  # wall time per match: ~5 s to battle frame 0, settle, window, margin
PATTERN_FRAMES = 12  # more than the deepest rollback + input delay
SWEEP = "0,0,100,250,400"  # player index 0, from 0 ms, hold 100 ms, gap 250-400 ms
DPAD_LEFT, DPAD_RIGHT = 13, 14


def pattern(path: str) -> None:
    """Peer 2: press left, release, press right, release, ... one change every PATTERN_FRAMES vblanks."""
    with open(path, "w") as f:
        for n in range(1, 4 * RUN_S * 60 // PATTERN_FRAMES):
            button = DPAD_LEFT if n // 2 % 2 == 0 else DPAD_RIGHT
            f.write(f"{n * PATTERN_FRAMES} b {button} {n % 2}\n")


def run_cell(s, name: str, tag: str, out: str, presentmon: str) -> dict:
    d1, d2, tr, vs = CELLS[name]
    p1_pad = os.path.join(out, "p1.joystick")
    p2_pad = os.path.join(out, "p2.joystick")
    open(p1_pad, "w").close()
    pattern(p2_pad)
    env = {"TEST_GGPO_DELAY": "2", "VITAL": "600", "GGPO_TEST_LOG": "1", "TEST_PING_MS": "2000",
           "GDXSV_PERF_TRACE": "perf.csv"}
    peer_env = {1: {"GDXSV_TEST_JOYSTICK": p1_pad, "GDXSV_PRESS_SWEEP": SWEEP},
                2: {"GDXSV_TEST_JOYSTICK": p2_pad}}
    cfg = [f"config:rend.ThreadedRendering={tr}", "input:JoystickPolling=yes", f"config:rend.vsync={vs}",
           "config:rend.DelayFrameSwapping=no", "config:pvr.rend=2"]
    pm_csv = os.path.join(out, tag + "_pm.csv")
    pm_proc = []

    def on_start(m):
        if presentmon:
            pm_proc.append(subprocess.Popen(
                [presentmon, "--process_id", str(m.peers[0].inst.proc.pid), "--qpc_time", "--no_console_stats",
                 "--stop_existing_session", "--output_file", pm_csv],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))

    # peer 2 without random input: its pad pattern alone drives it. Peer 1's sweep turns its random input off.
    m = rbk_lib.run_match(s, tag, [s.exe] * 2, [d1, d2], headless=False, env=env, config=cfg, timeout=RUN_S,
                          on_start=on_start, peer_env=peer_env, peer_config={2: ["gdxsv:rand_input=0"]})
    for p in pm_proc:  # --terminate_on_proc_exit does not fire when flycast is killed
        subprocess.run([presentmon, "--terminate_existing_session"], capture_output=True, timeout=30)
        try:
            p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            p.kill()
    p1, p2 = m.peers
    r = analyze.analyze(os.path.join(p1.dir, "perf.csv"), pm_csv if presentmon else None, SETTLE, FRAMES)
    # D1: the rollback suite's checks that apply to a match cut before its end (state hashes differ per peer
    # by design; round winners and last frames need a finished match, the hash log is cut at the kill).
    r["D1"] = {"failures": sum(len(p.errors) for p in m.peers) + len(m.stalled)}
    r["checks"] = {"delay2": all("delay=2" in p.log for p in m.peers), "rtt": all("rtt=" in p.log for p in m.peers),
                   "p2_no_rand": "RandomInput" not in p2.log, "sweep": "press sweep: player 0" in p1.log,
                   "errors": [e for p in m.peers for e in p.errors][:5], "stalled": m.stalled}
    with open(os.path.join(out, tag + ".json"), "w") as f:
        json.dump(r, f, indent=1)
    return r


def median_runs(runs: list) -> dict:
    """Per-leaf median over runs of the same cell."""
    if len(runs) == 1:
        return runs[0]
    out = {}
    for k, v in runs[0].items():
        if isinstance(v, dict):
            out[k] = median_runs([r.get(k, {}) for r in runs])
        elif isinstance(v, (int, float)) and not isinstance(v, bool):
            vals = [r[k] for r in runs if isinstance(r.get(k), (int, float))]
            out[k] = statistics.median(vals) if vals else None
        else:
            out[k] = v
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--exe", required=True)
    ap.add_argument("--cells", default="all")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--baseline")
    ap.add_argument("--write-baseline")
    ap.add_argument("--presentmon", default=os.environ.get("PRESENTMON"))
    ap.add_argument("--out", default=os.path.join(TESTS, "out", "perf"))
    a = ap.parse_args()
    rom = os.path.join(os.environ.get("GDXSV_ROM_DIR", ""), "gdx-disc2", "gdx-disc2.gdi")
    state = os.environ.get("GDXSV_STATE_DIR", os.path.join(TESTS, "out", "state"))
    os.makedirs(a.out, exist_ok=True)
    rbk_lib.N = 2
    s = rbk_lib.Settings(exe=a.exe, rom=rom, out=a.out, state_dir=state)
    pm = a.presentmon if a.presentmon and os.path.exists(a.presentmon) else None
    if not pm:
        print("no PresentMon: A1, A2, B1 not measured")
    base = json.load(open(a.baseline)) if a.baseline else None
    cells = list(CELLS) if a.cells == "all" else a.cells.split(",")
    result, ok = {}, True
    for name in cells:
        runs = []
        while True:
            t = time.time()
            runs.append(run_cell(s, name, f"{name}_{len(runs) + 1}", a.out, pm))
            print(f"{name} run {len(runs)}: {time.time() - t:.0f} s, checks {runs[-1]['checks']}", flush=True)
            if len(runs) < a.runs:
                continue
            med = median_runs(runs)
            if not base or name not in base:
                break
            fails = compare.judge(name, base[name], med)
            hard = [f for f in fails if f.startswith("D1")]
            if not fails or hard or len(runs) >= 3:
                break
            print(f"{name}: outside tolerance ({'; '.join(fails)}), re-run", flush=True)
        med["runs"] = len(runs)
        if a.runs > 1:
            med["spread"] = compare.spread(runs)
        result[name] = med
        if base and name in base:
            ok &= not compare.judge(name, base[name], med)
        ok &= med.get("D1", {}).get("failures", 1) == 0
    with open(os.path.join(a.out, "result.json"), "w") as f:
        json.dump(result, f, indent=1)
    if a.write_baseline:
        with open(a.write_baseline, "w") as f:
            json.dump(result, f, indent=1)
    print(compare.table(base, result))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
