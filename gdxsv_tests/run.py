#!/usr/bin/env python3
"""Run the gdxsv e2e cases. See README.md.

    python gdxsv_tests/run.py --list
    python gdxsv_tests/run.py --exe build/flycast.exe --tier smoke
    python gdxsv_tests/run.py --exe build/flycast.exe --cases P1,P1c --jobs 2

Exit code 0 when every selected case passed, was skipped or failed as known (known_failure).
"""

import argparse
import concurrent.futures
import importlib
import json
import os
import pkgutil
import shutil
import subprocess
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from lib.case import CASES, Ctx, Skip  # noqa: E402
import cases  # noqa: E402

for m in pkgutil.iter_modules(cases.__path__):
    importlib.import_module("cases." + m.name)


def commit() -> str:
    try:
        return subprocess.run(["git", "-C", HERE, "rev-parse", "HEAD"], capture_output=True, text=True,
                              check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def run_case(c, exe: str, out: str) -> dict:
    d = os.path.join(out, c.id)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    t0 = time.time()
    try:
        r = c.fn(Ctx(exe, d))
    except Skip as e:
        r = {"status": "skip", "reason": str(e)}
    except Exception:
        r = {"status": "fail", "exception": traceback.format_exc()}
    if c.known_failure and r["status"] in ("pass", "fail"):
        r["known_failure"] = c.known_failure
        if r["status"] == "fail":
            r["status"] = "known"
        else:
            r["reason"] = "known failure passed: drop the mark"
    r["seconds"] = round(time.time() - t0, 1)
    with open(os.path.join(d, "result.json"), "w", encoding="utf-8") as f:
        json.dump(r, f, indent=1)
    return r


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="list the cases and exit")
    ap.add_argument("--cases", help="comma-separated case ids")
    ap.add_argument("--tier", choices=["smoke", "full"], default="smoke", help="without --cases (default smoke)")
    ap.add_argument("--jobs", type=int, default=1, help="cases run in parallel")
    ap.add_argument("--exe", default=os.environ.get("GDXSV_EXE"), help="flycast executable (or GDXSV_EXE)")
    ap.add_argument("--out", default=os.path.join(HERE, "out"))
    a = ap.parse_args()

    if a.list:
        for c in CASES.values():
            ctl = f"  (control of {c.control_of})" if c.control_of else ""
            kf = f"  (known failure: {c.known_failure})" if c.known_failure else ""
            print(f"{c.id:6} {c.tier:6} {c.title}{ctl}{kf}")
        return 0
    if a.cases:
        unknown = [i for i in a.cases.split(",") if i not in CASES]
        if unknown:
            ap.error("unknown case: " + ",".join(unknown))
        selected = [CASES[i] for i in a.cases.split(",")]
    else:
        selected = [c for c in CASES.values() if a.tier == "full" or c.tier == "smoke"]
    if not a.exe or not os.path.isfile(a.exe):
        ap.error("--exe (or GDXSV_EXE): the flycast executable to test")
    exe = os.path.abspath(a.exe)
    os.makedirs(a.out, exist_ok=True)

    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as pool:
        futures = {pool.submit(run_case, c, exe, a.out): c for c in selected}
        for fut in concurrent.futures.as_completed(futures):
            c = futures[fut]
            r = fut.result()
            results[c.id] = r
            why = r.get("reason") or r.get("known_failure") or ("" if r["status"] == "pass" else "see " + os.path.join(a.out, c.id))
            print(f"{r['status'].upper():5} {c.id:6} {r['seconds']:6.1f}s  {c.title}  {why}", flush=True)

    summary = {"commit": commit(), "exe": exe, "exe_mtime": int(os.path.getmtime(exe)),
               "platform": sys.platform, "results": {i: results[i] for i in sorted(results)}}
    with open(os.path.join(a.out, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1)
    failed = [i for i, r in results.items() if r["status"] == "fail"]
    print(f"{len(results) - len(failed)}/{len(results)} passed, skipped or known failures" +
          (", failed: " + ",".join(sorted(failed)) if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
