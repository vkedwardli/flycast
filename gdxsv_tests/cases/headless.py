"""Headless mode (gdxsv:headless=yes): the emulator runs with no window and no graphics API."""

import os
import re
import time

from lib import assets, emu, recheck
from lib.case import case, finish, result, write_lua

FRAMES = 300
WAIT = 90
# The first renderer line of each graphics API (DX11, DX9, Vulkan, OpenGL / GLES).
RENDERER_RE = re.compile(r"N\[RENDERER\]: (DX11 Context initializing|DX9 Context initializing|VulkanRenderer::Init|"
                         r"OpenGL(?: ES)? version \d)")


def run(ctx, name: str, headless: bool) -> dict:
    rom = ctx.rom("dc2")
    d = os.path.join(ctx.out, name)
    os.makedirs(ctx.out, exist_ok=True)
    lua = write_lua(os.path.join(ctx.out, name + ".lua"), "memread.lua", {"frame": 60, "reads": []})
    cfg = [f"gdxsv:headless_probe_frames={FRAMES}", "log:Verbosity=3", "log:LogToFile=1",
           "gdxsv:headless_loadstate=yes"]
    t0 = time.time()
    inst = emu.launch(ctx.exe, rom, d, config=cfg, state_dir=assets.state_dir(rom), lua=lua, headless=headless)
    saved = os.path.join(d, "window.json")  # the window samples: saved for run.py --recheck
    window, done_at = None, None
    if recheck.ACTIVE:
        window = recheck.load(saved, None)
    else:
        # Windows: sample for a visible window until the exit; the windowed run is killed 3 s after the Lua is done.
        e2e_log = os.path.join(d, "e2e.log")
        while inst.poll() is None and time.time() - t0 < WAIT:
            if emu.IS_WINDOWS and emu.hung_windows([inst.proc.pid])[inst.proc.pid] is not None:
                window = True
            elif emu.IS_WINDOWS and window is None:
                window = False
            if done_at is None and os.path.isfile(e2e_log) and "E2E done=" in open(e2e_log, encoding="utf-8").read():
                done_at = time.time()
            if not headless and done_at is not None and time.time() - done_at > 3:
                break
            time.sleep(0.2)
        recheck.save(saved, window)
    rc = inst.poll()
    timed_out = rc is None and done_at is None
    if rc is None:
        inst.kill()
        inst.proc.wait(10)
    p = finish(inst, rc, timed_out, t0)
    inst.remove_exe()
    return {"rc": p.rc, "timed_out": p.timed_out, "lua_done": "done" in p.e2e, "gameid": p.e2e.get("gameid"),
            "window": window, "probe_exit": f"headless probe: exiting after {FRAMES} frames" in p.log,
            "renderer": "".join(m.group(1) for m in RENDERER_RE.finditer(p.log))[:80], "errors": p.errors[:5]}


def checks(r: dict) -> list:
    c = [{"what": "the game ran (Lua read its id)", "ok": r["lua_done"] and bool(r["gameid"])},
         {"what": f"exited by itself after {FRAMES} vblanks, rc 0", "ok": r["probe_exit"] and r["rc"] == 0},
         {"what": "log: no graphics API started", "ok": not r["renderer"]}]
    if emu.IS_WINDOWS:
        c.append({"what": "no visible window", "ok": r["window"] is False})
    return c


@case("E1", "headless mode: the game runs and exits with no window and no graphics API", tier="smoke")
def headless(ctx):
    r = run(ctx, "E1", True)
    return result(checks(r), **r)
