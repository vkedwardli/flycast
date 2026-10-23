"""Audio rate control: a windowed replay without audio sync, its output queue level from the log.
Needs an audio output device."""

import os
import re
import time

from lib import assets, emu, recheck
from lib.case import case, result
from cases import replay

SECONDS = 100
# 60Hz instead of the replay's 59.94Hz: the game makes 0.1 % more audio than the device plays,
# so without rate control the queue fills up and overflows.
FAST = "gdxsv:ReplayFixedFrequency=3"
# "Audio queue" line every ~10 s (core/gdxsv/gdxsv_audio.cpp); the first two cover the start.
QUEUE_RE = re.compile(r"Audio queue: mean ([\d.]+) min ([\d.]+) max ([\d.]+) rate control (on|off)")
SKIP_WINDOWS = 2
# A level of 0 or 1 is a dry or full queue: the device plays silence or the backend drops samples.
# Measured on windows-mini, windows after the start (a round's loading screen also fills it):
# rate control on 0.18-0.91, off 0.28-1.00 with 1.00 in every window.


def queue_run(ctx, name: str, extra: list) -> dict:
    pb = assets.replay(replay.newest_dc2())
    cfg = ["log:Verbosity=3", "log:LogToFile=1", "gdxsv:ConnectionMedium=Wired", f"gdxsv:replay={pb}",
           "gdxsv:ReplayPOV=1", "gdxsv:ReplaySeekStates=no", FAST, *extra]
    inst = emu.launch(ctx.exe, ctx.rom("dc2"), os.path.join(ctx.out, name), config=cfg, headless=False)
    try:
        if not recheck.ACTIVE:
            time.sleep(SECONDS)
    finally:
        inst.kill()
        inst.proc.wait(10)
        inst.remove_exe()
    windows = [{"mean": float(m[0]), "min": float(m[1]), "max": float(m[2]), "control": m[3]}
               for m in QUEUE_RE.findall(inst.log)]
    return {"windows": windows, "errors": inst.errors[:5]}


def queue_checks(r: dict) -> list:
    after = r["windows"][SKIP_WINDOWS:]
    levels = [(w["min"], w["max"]) for w in after]
    return [{"what": "queue level logged after the start", "windows": len(after), "ok": len(after) >= 5},
            {"what": "the queue never ran dry or full", "min_max": levels,
             "ok": bool(levels) and all(0 < lo and hi < 1 for lo, hi in levels)}]


@case("F3", "audio rate control: a replay paced 0.1 % faster than the device: the audio queue never runs dry or full",
      tier="full")
def audio_rate(ctx):
    r = queue_run(ctx, "F3", [])
    on = all(w["control"] == "on" for w in r["windows"])
    return result(queue_checks(r) + [{"what": "rate control on", "ok": on}], **r)
