"""Headless boot probe (gdxsv:headless_loadstate, gdxsv:headless_probe_frames), which the other cases run on."""

import re

from lib.case import case, result

FRAMES = 300
LOADED_RE = re.compile(r"Loaded state .*_99\.state")


def probe_checks(p) -> list:
    return [{"what": "exited by itself, rc 0", "rc": p.rc, "timed_out": p.timed_out,
             "ok": p.rc == 0 and not p.timed_out},
            {"what": f"exited after {FRAMES} vblanks", "ok": f"headless probe: exiting after {FRAMES} frames" in p.log},
            {"what": "resumed the slot-99 lobby savestate (no cold boot)", "ok": bool(LOADED_RE.search(p.log))}]


@case("B1", f"boot probe: headless_loadstate resumes the lobby savestate, headless_probe_frames={FRAMES} exits rc 0")
def boot_probe(ctx):
    p = ctx.probe("B1", ctx.rom("dc2"), "memread.lua", {"frame": 30, "reads": []}, frames=FRAMES, timeout=60)
    return result(probe_checks(p), **p.facts())


@case("B1c", "B1's checks on a headless run without the two options (killed after 30 s): they fail",
      control_of="B1")
def boot_probe_control(ctx):
    p = ctx.run("B1c", ctx.rom("dc2"), [], timeout=30)
    c = probe_checks(p)
    return result([{"what": "every probe check fails", "ok": all(not x["ok"] for x in c)}], detail=c, **p.facts())
