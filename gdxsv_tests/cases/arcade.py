"""The NAOMI arcade version (gundmxgd): BIOS naomi.zip, keychip gundmxgd.zip, GD gundmxgd/gdl-0006.chd."""

import os
import re
import shutil

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


def link(src: str, dst: str) -> None:
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy(src, dst)


@case("N1c", "N1 with the game files but no BIOS (naomi.zip)", control_of="N1")
def naomi_boot_control(ctx):
    rom = ctx.rom("naomi")
    d = os.path.join(ctx.out, "rom")
    link(rom, os.path.join(d, os.path.basename(rom)))
    gd = os.path.join(os.path.dirname(rom), "gundmxgd")
    for f in os.listdir(gd):
        link(os.path.join(gd, f), os.path.join(d, "gundmxgd", f))
    checks = boot(ctx, "nobios", os.path.join(d, os.path.basename(rom)))
    return result([{"what": "boot check fails", "ok": not all(c["ok"] for c in checks)}], detail=checks)
