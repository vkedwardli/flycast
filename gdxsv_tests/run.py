#!/usr/bin/env python3
"""Run the gdxsv e2e cases. See README.md.

    python gdxsv_tests/run.py --list
    python gdxsv_tests/run.py --exe build/flycast.exe --tier smoke
    python gdxsv_tests/run.py --exe build/flycast.exe --cases P1,P1c --jobs 2
    python gdxsv_tests/run.py --recheck gdxsv_tests/out --cases K1

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

from lib import recheck  # noqa: E402
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


def run_case(c, exe: str, out: str, saved: str = None) -> dict:
    """saved: an earlier out dir; its <case id> dir is copied and the case only rechecks it."""
    d = os.path.join(out, c.id)
    shutil.rmtree(d, ignore_errors=True)
    t0 = time.time()
    if saved and not os.path.isdir(os.path.join(saved, c.id)):
        r = {"status": "skip", "reason": "nothing saved in " + os.path.join(saved, c.id)}
        os.makedirs(d)
    else:
        if saved:
            shutil.copytree(os.path.join(saved, c.id), d, ignore=shutil.ignore_patterns("*.exe", "result.json"))
        else:
            os.makedirs(d)
        recheck.begin(d)
        try:
            r = c.fn(Ctx(exe, d))
        except Skip as e:
            r = {"status": "skip", "reason": str(e)}
        except Exception:
            r = {"status": "fail", "exception": traceback.format_exc()}
        if saved and recheck.missing():
            r["recheck_missing"] = recheck.missing()
    if saved:
        r["rechecked_from"] = os.path.join(saved, c.id)
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


def print_checks(r: dict) -> None:
    """Each check with the values it judged (a control's own checks under "detail")."""
    detail = r.get("detail") if isinstance(r.get("detail"), list) else []
    for chk in r.get("checks", []) + detail:
        if not isinstance(chk, dict):
            continue
        vals = {k: v for k, v in chk.items() if k not in ("what", "ok")}
        print(f"        {'ok  ' if chk.get('ok') else 'FAIL'} {chk.get('what', '')}" +
              (f"  {json.dumps(vals)[:300]}" if vals else ""))
    facts = {k: v for k, v in r.items() if k not in ("status", "checks", "detail", "seconds", "exception",
                                                     "recheck_missing", "reason", "rechecked_from")}
    if facts:
        print(f"        facts {json.dumps(facts)[:600]}")
    if r.get("recheck_missing"):
        print("        not saved by that run (defaults used): " + ", ".join(r["recheck_missing"]))
    if r.get("exception"):
        print("        " + r["exception"].strip().splitlines()[-1])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="list the cases and exit")
    ap.add_argument("--cases", help="comma-separated case ids")
    ap.add_argument("--tier", choices=["smoke", "full"], default="smoke", help="without --cases (default smoke)")
    ap.add_argument("--jobs", type=int, default=1, help="cases run in parallel")
    ap.add_argument("--exe", default=os.environ.get("GDXSV_EXE"), help="flycast executable (or GDXSV_EXE)")
    ap.add_argument("--out", help="default gdxsv_tests/out; with --recheck <dir>.recheck")
    ap.add_argument("--recheck", metavar="DIR",
                    help="an earlier --out: run the checks of the cases saved there (with --cases: those and "
                         "their controls) on a copy of their files, without flycast; prints each check")
    a = ap.parse_args()

    if a.list:
        for c in CASES.values():
            ctl = f"  (control of {c.control_of})" if c.control_of else ""
            kf = f"  (known failure: {c.known_failure})" if c.known_failure else ""
            print(f"{c.id:6} {c.tier:10} {c.title}{ctl}{kf}")
        return 0
    if a.cases:
        unknown = [i for i in a.cases.split(",") if i not in CASES]
        if unknown:
            ap.error("unknown case: " + ",".join(unknown))
        selected = [CASES[i] for i in a.cases.split(",")]
    else:
        selected = [c for c in CASES.values() if c.tier != ("smoke-only" if a.tier == "full" else "full")]
    saved = None
    if a.recheck:
        saved = os.path.abspath(a.recheck)
        if not os.path.isdir(saved):
            ap.error("--recheck: no directory " + saved)
        if a.cases:
            ids = [c.id for c in selected]
            selected += [c for c in CASES.values() if c.control_of in ids and c.id not in ids
                         and os.path.isdir(os.path.join(saved, c.id))]
        else:
            selected = [c for c in CASES.values() if os.path.isdir(os.path.join(saved, c.id))]
        a.out = a.out or saved.rstrip("/\\") + ".recheck"
        if os.path.abspath(a.out) == saved:
            ap.error("--out must differ from --recheck")
        recheck.ACTIVE = True
        exe = os.path.abspath(a.exe or "flycast")
        head = commit()

        def no_launch(args, *_, **__):
            raise RuntimeError(f"--recheck starts no process (a launcher without a recheck path): {args}")
        subprocess.Popen = no_launch
    elif not a.exe or not os.path.isfile(a.exe):
        ap.error("--exe (or GDXSV_EXE): the flycast executable to test")
    else:
        exe = os.path.abspath(a.exe)
    a.out = a.out or os.path.join(HERE, "out")
    os.makedirs(a.out, exist_ok=True)

    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as pool:
        futures = {pool.submit(run_case, c, exe, a.out, saved): c for c in selected}
        for fut in concurrent.futures.as_completed(futures):
            c = futures[fut]
            r = fut.result()
            results[c.id] = r
            why = r.get("reason") or r.get("known_failure") or ("" if r["status"] == "pass" else "see " + os.path.join(a.out, c.id))
            print(f"{r['status'].upper():5} {c.id:6} {r['seconds']:6.1f}s  {c.title}  {why}", flush=True)
            if saved:
                print_checks(r)

    summary = {"commit": head if saved else commit(), "exe": exe, "exe_mtime": int(os.path.getmtime(exe)) if not saved else None,
               "recheck": saved,
               "platform": sys.platform, "results": {i: results[i] for i in sorted(results)}}
    with open(os.path.join(a.out, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1)
    failed = [i for i, r in results.items() if r["status"] == "fail"]
    print(f"{len(results) - len(failed)}/{len(results)} passed, skipped or known failures" +
          (", failed: " + ",".join(sorted(failed)) if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
