"""Input delay in a rollback battle."""

import os
import threading

from lib import assets, recheck
from lib.case import Skip, case, result, write_lua
from rollback import rbk_lib

# One local match at a time: the peers use fixed ports.
MATCH_LOCK = threading.Lock()

# InetBuf and the in-battle markers (gdxsv_backend_rollback.cpp, mcs_message.h)
DC2 = {"inetbuf": 0x0c3ab984, "battle": [0x0c3d16d4, 0x0c3d16d5]}


def mark_presser(match):
    with open(os.path.join(match.peers[0].dir, "presser"), "w") as f:
        f.write("1")


def measure(ctx, tag: str, delay: int) -> dict:
    """A 4-player local rollback match with input delay `delay` (TEST_GGPO_DELAY); peer 1
    holds B 180 vblanks into the battle and logs when its confirmed input buffer has it."""
    if recheck.ACTIVE:
        raise Skip("not recheckable: rbk_lib.run_match saves no record of its peers")
    rom = ctx.rom("dc2")
    s = rbk_lib.Settings(exe=ctx.exe, rom=rom, out=ctx.out, state_dir=assets.state_dir(rom))
    os.makedirs(ctx.out, exist_ok=True)
    lua = write_lua(os.path.join(ctx.out, tag + ".lua"), "input_lag.lua", {**DC2, "settle": 180, "window": 60})
    with MATCH_LOCK:
        m = rbk_lib.run_match(s, tag, [ctx.exe] * rbk_lib.N, [0] * rbk_lib.N, env={"TEST_GGPO_DELAY": str(delay)},
                              lua=lua, timeout=600, on_start=mark_presser)
    e2e = {}
    try:
        with open(os.path.join(m.peers[0].dir, "e2e.log"), encoding="utf-8") as f:
            for line in f:
                if line.startswith("E2E ") and "=" in line:
                    k, v = line[4:].rstrip("\n").split("=", 1)
                    e2e[k] = v
    except OSError:
        pass
    return {"ggpo_delay": delay, "delay": e2e.get("delay"), "slot": e2e.get("slot"), "lua_error": e2e.get("error"),
            "timed_out": m.timed_out, "stalled": m.stalled, "elapsed": round(m.elapsed, 1),
            "finished": [p.finished for p in m.peers]}


def check(runs: list) -> dict:
    """GGPO delays its local input by GGPODelay frames, so two delay settings 2 apart
    must show up 2 vblanks apart in the confirmed input buffer, in peer 1's slot."""
    try:
        diff = int(runs[1]["delay"]) - int(runs[0]["delay"])
    except (TypeError, ValueError):
        diff = None
    want = runs[1]["ggpo_delay"] - runs[0]["ggpo_delay"]
    return {"what": f"delay difference {diff} == {want}, slot 0", "ok": diff == want
            and all(r["slot"] == "0" for r in runs)}


@case("I1", "input delay follows the GGPO delay setting: GGPO delay 4 vs 2 shows 2 vblanks more")
def input_lag(ctx):
    runs = [measure(ctx, f"d{d}", d) for d in (2, 4)]
    return result([check(runs)], runs=runs)


@case("I1c", "I1's check on two runs both at GGPO delay 2 (as if the setting were ignored): the check fails it",
      control_of="I1")
def input_lag_control(ctx):
    runs = [measure(ctx, "d2", 2), measure(ctx, "d2b", 2)]
    runs[1]["ggpo_delay"] = 4
    c = check(runs)
    return result([{"what": "same delay twice fails the check", "ok": not c["ok"]}], detail=c, runs=runs)
