"""The NAOMI arcade version (gundmxgd): BIOS naomi.zip, keychip gundmxgd.zip, GD gundmxgd/gdl-0006.chd."""

import os
import re

from lib.case import case, result

GAME_ID_RE = re.compile(r"NAOMI GAME ID \[([^\]]*)\]")
# The BIOS copies the game from the DIMM here and jumps to it.
CODE = 0x0c020000


def boot(ctx, name: str, rom: str) -> list:
    d = os.path.dirname(rom)
    p = ctx.probe(name, rom, "memread.lua", {"frame": 1200, "reads": [[CODE, 4]]}, frames=1220,
                  loadstate=False, timeout=240,
                  config=[f"config:Dreamcast.ContentPath={d}", f"config:Dreamcast.BiosPath={d}"])
    m = GAME_ID_RE.search(p.log)
    code = p.e2e.get(f"0x{CODE:08x}:4")
    return [{"what": "probe ran", "ok": p.clean, **p.facts()},
            {"what": "game id", "got": m.group(1).strip() if m else None, "ok": bool(m and "GUNDAM" in m.group(1))},
            {"what": "code resident", "addr": f"0x{CODE:08x}", "got": code, "ok": code not in (None, "0")}]


@case("N1", "NAOMI version boots: game id, code at 0x0c020000", tier="smoke")
def naomi_boot(ctx):
    return result(boot(ctx, "naomi", ctx.rom("naomi")))


