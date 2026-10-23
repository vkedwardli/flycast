#!/usr/bin/env python3
"""Pre-release check: the full tier and the rollback suite in parallel, each failure again alone,
then an A/B against the last release for what still fails. One table at the end.

  python gdxsv_tests/precheck.py --exe build/flycast.exe --old-exe <last release flycast-gdxsv.exe> \\
      --release gdxsv-1.10.0 --rom <Disc 2 GDI> --state-dir <dir> --relay-exe <gdxsv.exe>

Verdicts:
  pass          passed in the parallel pass
  under load    failed in the parallel pass, passed alone
  broken test   fails alone, also on the last release: fix the test (does not block the release)
  regression    fails alone on this build only; the PRs merged since --release are listed
  not run       the time box (--minutes) ran out first

Run it on an idle box: no build, rig or other job. It stops starting new steps after --minutes.
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
RUNNERS = {"run": os.path.join(HERE, "run.py"), "suite": os.path.join(HERE, "rollback", "run_suite.py")}
BUSY = ("flycast", "ninja", "cc1plus", "presentmon")
AB_RUNS = 3


def busy_processes():
    if sys.platform == "win32":
        out = subprocess.run(["tasklist", "/fo", "csv", "/nh"], capture_output=True, text=True).stdout
        names = [line.split(",")[0].strip('"') for line in out.splitlines() if line]
    else:
        names = subprocess.run(["ps", "-A", "-o", "comm="], capture_output=True, text=True).stdout.split()
    return sorted({n for n in names if any(b in n.lower() for b in BUSY)})


def run(a, runner, exe, out, cases=None, jobs=1):
    """Runs one runner; returns {case id: "pass" | "fail" | other status} from its summary.json."""
    cmd = [sys.executable, RUNNERS[runner], "--exe", exe, "--out", out, "--jobs", str(jobs)]
    if runner == "run":
        cmd += ["--cases", ",".join(cases)] if cases else ["--tier", "full"]
    else:
        cmd += ["--rom", a.rom, "--state-dir", a.state_dir, "--old-exe", a.old_exe]
        cmd += ["--relay-exe", a.relay_exe] if a.relay_exe else []
        cmd += ["--cases", ",".join(cases)] if cases else []
    os.makedirs(out, exist_ok=True)
    t = time.time()
    with open(out + ".log", "w", encoding="utf-8") as log:
        subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, cwd=REPO)
    print(f"  {runner} {','.join(cases) if cases else 'all'} on {os.path.basename(exe)}: {time.time() - t:.0f}s, "
          f"log {out}.log", flush=True)
    try:
        with open(os.path.join(out, "summary.json"), encoding="utf-8") as f:
            s = json.load(f)
    except (OSError, ValueError):
        return {c: "fail" for c in cases or []} or {"(" + runner + ")": "fail"}
    results = s["results"] if runner == "run" else s
    return {c: r["status"].lower() for c, r in results.items()}


def merged_prs(release):
    out = subprocess.run(["git", "log", "--merges", "--format=%s", f"{release}..HEAD"],
                         capture_output=True, text=True, cwd=REPO).stdout
    return [line for line in out.splitlines() if line.startswith("Merge pull request")]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, help="the build to check")
    ap.add_argument("--old-exe", required=True, help="the last release, for the A/B and the suite's A3")
    ap.add_argument("--release", help="the last release's tag, to list the PRs since it for a regression")
    ap.add_argument("--rom", default=os.environ.get("ROM"), help="Disc 2 GDI, for the rollback suite (or ROM)")
    ap.add_argument("--state-dir", default=os.path.join(REPO, "work", "state"), help="rollback suite savestates")
    ap.add_argument("--relay-exe", help="gdxsv server executable, for the suite's relay cases")
    ap.add_argument("--jobs", type=int, default=4, help="run.py cases at once in the parallel pass")
    ap.add_argument("--suite-jobs", type=int, default=3, help="run_suite.py cases at once in the parallel pass")
    ap.add_argument("--minutes", type=float, default=60, help="time box: no new step starts after it")
    ap.add_argument("--out", default=os.path.join(HERE, "out", "precheck"))
    ap.add_argument("--allow-busy", action="store_true", help="run although a build or flycast is running")
    a = ap.parse_args()

    busy = busy_processes()
    if busy and not a.allow_busy:
        ap.error("the box is not idle: " + ", ".join(busy) + " (--allow-busy to run anyway)")
    exe, old = os.path.abspath(a.exe), os.path.abspath(a.old_exe)
    out = os.path.abspath(a.out)
    start = time.time()

    def time_left():
        return time.time() - start < a.minutes * 60

    rows = {}  # (runner, case) -> row
    print("== parallel pass", flush=True)
    for runner, jobs in (("run", a.jobs), ("suite", a.suite_jobs)):
        if not time_left():
            break
        for c, st in run(a, runner, exe, os.path.join(out, "parallel", runner), jobs=jobs).items():
            rows[(runner, c)] = {"parallel": st, "verdict": "pass" if st != "fail" else None}

    print("== failures alone", flush=True)
    for (runner, c), row in rows.items():
        if row["verdict"] or not time_left():
            continue
        row["alone"] = run(a, runner, exe, os.path.join(out, "alone", c), [c]).get(c, "fail")
        if row["alone"] != "fail":
            row["verdict"] = "under load"

    print(f"== A/B against the last release, {AB_RUNS} runs each, interleaved", flush=True)
    for (runner, c), row in rows.items():
        if row["verdict"] or "alone" not in row:
            continue
        fails = {"build": 0, "release": 0}
        for i in range(1, AB_RUNS + 1):
            for tag, e in (("build", exe), ("release", old)):
                if not time_left():
                    break
                fails[tag] += run(a, runner, e, os.path.join(out, "ab", f"{c}-{tag}-{i}"), [c]).get(c, "fail") == "fail"
        row["ab"] = f"{fails['build']}/{AB_RUNS}, release {fails['release']}/{AB_RUNS}"
        if not time_left():
            continue
        row["verdict"] = "under load" if not fails["build"] else "broken test" if fails["release"] else "regression"

    print(f"\n== {len(rows)} cases, {(time.time() - start) / 60:.0f} min{'' if time_left() else ', time box ran out'}")
    print(f"{'case':10} {'runner':6} {'parallel':9} {'alone':6} {'A/B fails':22} verdict")
    for (runner, c), row in sorted(rows.items(), key=lambda kv: (kv[1]["verdict"] == "pass", kv[0])):
        row["verdict"] = row["verdict"] or "not run"
        if row["verdict"] == "pass" and row["parallel"] == "pass":
            continue
        print(f"{c:10} {runner:6} {row['parallel']:9} {row.get('alone', '-'):6} {row.get('ab', '-'):22} {row['verdict']}")
    counts = {}
    for row in rows.values():
        counts[row["verdict"]] = counts.get(row["verdict"], 0) + 1
    print("totals: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())) + " (passing cases not listed)")
    if "regression" in counts and a.release:
        print(f"PRs merged since {a.release}:\n  " + "\n  ".join(merged_prs(a.release)))
    with open(os.path.join(out, "precheck.json"), "w", encoding="utf-8") as f:
        json.dump({f"{r}/{c}": row for (r, c), row in rows.items()}, f, indent=1)
    return 1 if counts.get("regression") or counts.get("not run") else 0


if __name__ == "__main__":
    sys.exit(main())
