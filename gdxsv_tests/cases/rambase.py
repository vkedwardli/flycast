"""Fixed host address of guest RAM on Windows (core/windows/win_vmem.cpp): an outside tool reads
guest address A at host RAM_BASE + (A & 0x0fffffff)."""

import os
import re
import time

from lib import assets, emu, recheck
from lib.case import Skip, case, finish, result, write_lua

RAM_BASE = 0x210000000  # PREFERRED_RAM_BASE_ALIGNED: guest 0x0c000000 is host 0x21c000000
CODE = 0x8c010000  # game code: the same bytes all the time
SIZE = 64
# Reserved in the control before flycast starts: covers its preferred allocation (Sh4RCB below the base).
BLOCK = (0x200000000, 0x20000000)
WAIT = 60


def fixed_ram(r: dict) -> bool:  # addrspace.cpp logs "BASE ... RAM(16 MB) <host address of guest 0x0c000000>"
    return bool(r["ram"]) and int(r["ram"], 16) == RAM_BASE + 0x0c000000


def run(ctx, name: str, block: bool) -> dict:
    if not emu.IS_WINDOWS:
        raise Skip("Windows only")
    rom = ctx.rom("dc2")
    d = os.path.join(ctx.out, name)
    os.makedirs(ctx.out, exist_ok=True)
    key = f"0x{CODE:08x}:{SIZE}"
    lua = write_lua(os.path.join(ctx.out, name + ".lua"), "memread.lua", {"frame": 30, "reads": [[CODE, SIZE]]})
    cfg = ["gdxsv:headless_probe_frames=100000", "log:Verbosity=3", "log:LogToFile=1", "gdxsv:headless_loadstate=yes"]
    t0 = time.time()
    inst = emu.launch(ctx.exe, rom, d, config=cfg, state_dir=assets.state_dir(rom), lua=lua, suspended=block)
    saved = os.path.join(d, "host.json")  # the host read: saved for run.py --recheck
    host, reserved = None, None
    try:
        if recheck.ACTIVE:
            host, reserved = recheck.load(saved, [None, None])
        else:
            if block:
                reserved = emu.reserve_memory(inst.proc.pid, *BLOCK)
                emu.suspend(inst.proc.pid, False)
            e2e_log = os.path.join(d, "e2e.log")
            while inst.poll() is None and time.time() - t0 < WAIT:
                if os.path.isfile(e2e_log) and f"E2E {key}=" in open(e2e_log, encoding="utf-8").read():
                    raw = emu.read_memory(inst.proc.pid, RAM_BASE + (CODE & 0x0fffffff), SIZE)
                    host = raw.hex() if raw else None
                    break
                time.sleep(0.2)
            recheck.save(saved, [host, reserved])
    finally:
        rc = inst.poll()
        inst.kill()
        inst.proc.wait(10)
    p = finish(inst, 0 if rc is None else rc, False, t0)
    inst.remove_exe()
    return {"guest": p.e2e.get(key), "host": host, "reserved": reserved,
            "ram": "".join(re.findall(r"RAM\(16 MB\) ([0-9a-f]+)", p.log)[:1]), "errors": p.errors[:5]}


def checks(r: dict) -> list:
    return [{"what": "Lua read the guest bytes", "ok": bool(r["guest"])},
            {"what": f"the same bytes at host 0x{RAM_BASE + (CODE & 0x0fffffff):x}",
             "ok": bool(r["guest"]) and r["host"] == r["guest"]}]


@case("Y1", "fixed guest RAM address (Windows): guest 0x8c010000 read from outside at host 0x21c010000")
def ram_base(ctx):
    r = run(ctx, "Y1", False)
    return result(checks(r) + [{"what": "log: RAM at the fixed address", "ok": fixed_ram(r)}], **r)


@case("Y1c", "Y1 with the fixed range reserved before flycast starts: it falls back, the host read fails",
      control_of="Y1")
def ram_base_control(ctx):
    r = run(ctx, "Y1c", True)
    c = checks(r)
    return result([{"what": "the range was reserved", "ok": bool(r["reserved"])}, c[0],
                   {"what": "the host read check fails", "ok": not c[1]["ok"]},
                   {"what": "log: RAM elsewhere (fallback)", "ok": bool(r["ram"]) and not fixed_ram(r)}], detail=c, **r)
