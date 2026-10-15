"""Input delay in a rollback battle."""

import os

from lib import assets
from lib.case import case, result, write_lua
from rollback import rbk_lib

# InetBuf and the in-battle markers (gdxsv_backend_rollback.cpp, mcs_message.h)
DC2 = {"inetbuf": 0x0c3ab984, "battle": [0x0c3d16d4, 0x0c3d16d5]}


def measure(ctx, tag: str, delay: int) -> dict:
    """A 4-player local rollback match with input delay `delay` (TEST_GGPO_DELAY); every peer
    holds B 180 vblanks into the battle and logs when its confirmed input buffer has it."""
    rom = ctx.rom("dc2")
    s = rbk_lib.Settings(exe=ctx.exe, rom=rom, out=ctx.out, state_dir=assets.state_dir(rom))
    os.makedirs(ctx.out, exist_ok=True)
    lua = write_lua(os.path.join(ctx.out, tag + ".lua"), "input_lag.lua", {**DC2, "settle": 180, "window": 60})
    m = rbk_lib.run_match(s, tag, [ctx.exe] * rbk_lib.N, [0] * rbk_lib.N, env={"TEST_GGPO_DELAY": str(delay)},
                          lua=lua, timeout=600)
    peers = []
    for p in m.peers:
        e2e = {}
        try:
            with open(os.path.join(p.dir, "e2e.log"), encoding="utf-8") as f:
                for line in f:
                    if line.startswith("E2E ") and "=" in line:
                        k, v = line[4:].rstrip("\n").split("=", 1)
                        e2e[k] = v
        except OSError:
            pass
        peers.append({"peer": p.index, "delay": e2e.get("delay"), "slot": e2e.get("slot"),
                      "lua_error": e2e.get("error"), "finished": p.finished, "errors": p.errors[:3]})
    return {"ggpo_delay": delay, "timed_out": m.timed_out, "stalled": m.stalled, "elapsed": round(m.elapsed, 1),
            "peers": peers}


@case("I0", "input delay measured at GGPO delay 2 and 4 (no pass rule: a measurement)")
def input_lag_measure(ctx):
    runs = [measure(ctx, f"d{d}", d) for d in (2, 4)]
    return result([{"what": "measured", "ok": all(p["delay"] not in (None, "none")
                                                     for r in runs for p in r["peers"])}], runs=runs)
