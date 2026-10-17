"""Network status OSD in a rollback battle (window mode, judged by a cropped golden image)."""

import glob
import os
import threading
import time

from lib import assets, image, recheck
from lib.case import Skip, case, result
from rollback import rbk_lib
from cases.input import MATCH_LOCK

# The panel's labels and player names (Delay, Roll, P:, USER02, PILOT01, Ping, Lag, ...) as
# peer 1 of rbk_lib's match draws them in a 640x480 window. The numbers right of them are left
# out: they change.
LABELS_BOX = (14, 112, 60, 330)
LABELS_GOLDEN = "net_osd_labels.png"
SIZE = (640, 480)
# Measured with the golden's own run and a Stats=no run of the same seed: every grab from
# the battle start on 0.64-1.00, every grab without the panel 0.00-0.04.
SHOWN = 0.5
GRAB_EVERY = 2.0


def grab_match(ctx, tag: str, config: list) -> dict:
    """A window-mode local match; peer 1's window is saved every GRAB_EVERY s into
    out/<tag>/grabs (what the check reads, also in a recheck)."""
    grabs = os.path.join(ctx.out, tag, "grabs")
    facts = {}
    if not recheck.ACTIVE:
        rom = ctx.rom("dc2")
        s = rbk_lib.Settings(exe=ctx.exe, rom=rom, out=ctx.out, state_dir=assets.state_dir(rom))
        stop = threading.Event()
        errors = []

        def grabber(m):
            def run():
                os.makedirs(grabs, exist_ok=True)
                t0 = time.time()
                while not stop.is_set():
                    try:
                        image.grab(m.peers[0].inst.proc.pid,
                                   os.path.join(grabs, "p1_%03d.png" % int(time.time() - t0)))
                    except NotImplementedError as e:
                        errors.append(str(e))
                        return
                    stop.wait(GRAB_EVERY)
            threading.Thread(target=run, daemon=True).start()

        with MATCH_LOCK, image.SCREEN_LOCK:
            m = rbk_lib.run_match(s, tag, [ctx.exe] * rbk_lib.N, [0] * rbk_lib.N, headless=False,
                                  config=config, timeout=300, on_start=grabber)
        stop.set()
        if errors:
            raise Skip(errors[0])
        facts = {"finished": [p.finished for p in m.peers], "elapsed": round(m.elapsed, 1)}
    scores = {}
    for p in sorted(glob.glob(os.path.join(grabs, "p1_*.png"))):
        mask = image.white_mask(p, LABELS_BOX, SIZE)
        scores[os.path.basename(p)[3:6]] = round(image.recall(LABELS_GOLDEN, mask), 2)
    return {**facts, "scores": scores, "shown": sum(1 for v in scores.values() if v >= SHOWN)}


@case("G1", "network status OSD: the stats panel is drawn in a rollback battle")
def net_osd(ctx):
    r = grab_match(ctx, "G1", [])
    return result([{"what": "match finished on every peer", "ok": all(r.get("finished", [True])), **r},
                   # the battle is ~40 s of the ~60 s match
                   {"what": "the panel's labels in at least 5 grabs", "shown": r["shown"],
                    "ok": r["shown"] >= 5}])


@case("G1c", "G1 with network:Stats=no: no panel", control_of="G1")
def net_osd_control(ctx):
    r = grab_match(ctx, "G1c", ["network:Stats=no"])
    return result([{"what": "match finished on every peer", "ok": all(r.get("finished", [True])), **r},
                   {"what": "the panel check fails: labels in no grab", "shown": r["shown"],
                    "ok": len(r["scores"]) >= 5 and r["shown"] == 0}])
