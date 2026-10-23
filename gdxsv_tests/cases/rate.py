"""Fixed frame rate limiter (config:rend.FixedFrequency), timed by the wall clock."""

import os
import threading
import time

from lib import recheck
from lib.case import case, result

# FixedFrequency mode -> frames per second (core/gdxsv/gdxsv_frame_limiter.cpp)
MODES = {2: 1e6 / 16683, 4: 50.0, 5: 1e6 / 33333}
FRAMES = 600
TOLERANCE = 0.02  # a 10 ms poll on a 12-20 s span, plus scheduling noise


def watch(path: str, keys: list, seen: dict, stop: threading.Event) -> None:
    while not stop.is_set() and len(seen) < len(keys):
        if os.path.isfile(path):
            with open(path, encoding="utf-8", errors="replace") as f:
                s = f.read()
            for k in keys:
                if k not in seen and f"E2E {k}=" in s:
                    seen[k] = time.perf_counter()
        time.sleep(0.01)


def rate_run(ctx, mode: int, extra: list = (), frames: int = FRAMES) -> dict:
    name = f"F_{mode}" + "".join("_" + x.split("=")[-1] for x in extra)
    seen, stop = {}, threading.Event()
    t = threading.Thread(target=watch, args=(os.path.join(ctx.out, name, "e2e.log"), ["t0", "t1"], seen, stop))
    t.start()
    p = ctx.probe(name, ctx.rom("dc2"), "rate.lua", {"settle": 120, "frames": frames}, frames=120 + frames + 30,
                  config=[f"config:rend.FixedFrequency={mode}", *extra], until_done=True, timeout=120)
    stop.set()
    t.join()
    span = os.path.join(ctx.out, name, "watch.json")  # wall clock: saved for run.py --recheck
    if recheck.ACTIVE:
        seen = recheck.load(span, {})
    recheck.save(span, seen)
    fps = frames / (seen["t1"] - seen["t0"]) if "t0" in seen and "t1" in seen else None
    return {"mode": mode, "clean": p.clean, "fps": round(fps, 2) if fps else None, **p.facts()}


def rate_checks(runs: list, want: dict) -> list:
    return [{"what": f"FixedFrequency={r['mode']}: {want[r['mode']]:.2f} fps", "fps": r["fps"],
             "ok": r["clean"] and r["fps"] is not None and abs(r["fps"] / want[r["mode"]] - 1) <= TOLERANCE}
            for r in runs]


@case("F1", "fixed frame rate: FixedFrequency 2, 4, 5 run the game at 59.94, 50, 30 fps")
def fixed_rate(ctx):
    runs = [rate_run(ctx, m) for m in MODES]
    return result(rate_checks(runs, MODES), runs=runs)


@case("F2", "audio sync: with no fixed rate, LimitFPS=yes paces the game by the audio output at 59.94 fps")
def audio_sync(ctx):
    r = rate_run(ctx, 0, ["config:aica.LimitFPS=yes"])
    return result(rate_checks([r], {0: MODES[2]}), runs=[r])


IDLE_FRAMES = 6000
# Measured on windows-mini, LimitFPS=no from the lobby savestate: hack on 957-972 fps, off 317-324.
IDLE_SPEEDUP = 2.0


def idle_run(ctx, hack: bool, n: int) -> dict:
    return rate_run(ctx, 0, ["config:aica.LimitFPS=no", f"gdxsv:SlowIdleLoopHack={'yes' if hack else 'no'}",
                             f"run={n}"], frames=IDLE_FRAMES)


def idle_checks(fast: dict, slow: dict) -> list:
    ran = all(r["clean"] and r["fps"] for r in (fast, slow))
    return [{"what": "both probes ran", "ok": ran},
            {"what": f"unthrottled, the hack runs over {IDLE_SPEEDUP} x the fps without it",
             "fps": [fast["fps"], slow["fps"]], "ok": ran and fast["fps"] > IDLE_SPEEDUP * slow["fps"]}]


@case("H1", "slow idle loop hack: the game's idle loop block costs more cycles, so unthrottled frames finish sooner")
def idle_loop(ctx):
    fast, slow = idle_run(ctx, True, 0), idle_run(ctx, False, 1)
    return result(idle_checks(fast, slow), runs=[fast, slow])


@case("H1c", "H1's checks with the hack off in both runs: the faster check fails", control_of="H1")
def idle_loop_control(ctx):
    a, b = idle_run(ctx, False, 0), idle_run(ctx, False, 1)
    c = idle_checks(a, b)
    return result([{"what": "both probes ran", "ok": c[0]["ok"]}, {"what": "faster check fails", "ok": not c[1]["ok"]}],
                  detail=c, runs=[a, b])
