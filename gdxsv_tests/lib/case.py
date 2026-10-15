"""Case registry and the helpers the cases share.

A case is a function in cases/*.py decorated with @case. It gets a Ctx and returns a
dict with "status" ("pass" or "fail") and whatever details explain the result; raising
Skip marks it skipped (missing ROM). A control case (control_of=<id>) runs its case's
check on input where the feature is absent and passes when the check catches it.
"""

import json
import os
import time
from typing import Dict, List, Optional

from . import emu

HERE = os.path.dirname(os.path.abspath(__file__))
LUA_DIR = os.path.join(os.path.dirname(HERE), "lua")

CASES: Dict[str, "Case"] = {}


class Case:
    def __init__(self, id: str, title: str, tier: str, fn, control_of: Optional[str]):
        self.id = id
        self.title = title
        self.tier = tier
        self.fn = fn
        self.control_of = control_of


def case(id: str, title: str, tier: str = "full", control_of: Optional[str] = None):
    assert tier in ("smoke", "full"), tier

    def register(fn):
        assert id not in CASES, id
        CASES[id] = Case(id, title, tier, fn, control_of)
        return fn
    return register


class Skip(Exception):
    pass


def lua_value(v) -> str:
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    if isinstance(v, str):
        return json.dumps(v)
    if isinstance(v, (list, tuple)):
        return "{" + ", ".join(lua_value(x) for x in v) + "}"
    if isinstance(v, dict):
        return "{" + ", ".join(f"[{json.dumps(k)}] = {lua_value(x)}" for k, x in v.items()) + "}"
    raise TypeError(type(v))


def write_lua(path: str, script: str, params: dict) -> str:
    """e2e.lua, then E2E_PARAMS, then lua/<script>: flycast loads a single file."""
    parts = []
    for name in ("e2e.lua", script):
        with open(os.path.join(LUA_DIR, name), encoding="utf-8") as f:
            parts.append(f.read())
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(parts[0] + "\nE2E_PARAMS = " + lua_value(params) + "\n\n" + parts[1])
    return path


class Probe:
    """A finished instance: exit code, E2E values from e2e.log, flycast.log."""

    def __init__(self, inst: emu.Instance, rc: Optional[int], timed_out: bool, elapsed: float):
        self.inst = inst
        self.rc = rc
        self.timed_out = timed_out
        self.elapsed = round(elapsed, 1)
        self.e2e: Dict[str, str] = {}
        try:
            with open(os.path.join(inst.dir, "e2e.log"), encoding="utf-8", errors="replace") as f:
                for line in f:
                    if line.startswith("E2E ") and "=" in line:
                        k, v = line[4:].rstrip("\n").split("=", 1)
                        self.e2e[k] = v
        except OSError:
            pass
        self.log = inst.log
        self.errors = inst.errors

    def facts(self) -> dict:
        return {"rc": self.rc, "timed_out": self.timed_out, "elapsed": self.elapsed,
                "lua_error": self.e2e.get("error"), "errors": self.errors[:5]}

    @property
    def clean(self) -> bool:
        return self.rc == 0 and not self.timed_out and "error" not in self.e2e and not self.errors


class Ctx:
    def __init__(self, exe: str, out: str):
        self.exe = exe
        self.out = out

    def rom(self, name: str) -> str:
        from . import assets
        path = assets.rom(name)
        if path is None:
            raise Skip(f"no {name} image (GDXSV_ROM_DIR or {assets.ROMS[name][0]})")
        return path

    def probe(self, name: str, rom: str, script: str, params: dict, frames: int,
              config: Optional[List[str]] = None, loadstate: bool = True, timeout: float = 180) -> Probe:
        """Run one headless instance for `frames` vblanks (gdxsv:headless_probe_frames exits it)
        with lua/<script>; with loadstate it resumes the game's slot-99 lobby savestate."""
        from . import assets
        d = os.path.join(self.out, name)
        os.makedirs(self.out, exist_ok=True)
        lua = write_lua(os.path.join(self.out, name + ".lua"), script, params)
        cfg = [f"gdxsv:headless_probe_frames={frames}"] + list(config or [])
        if loadstate:
            cfg.append("gdxsv:headless_loadstate=yes")
        state = assets.state_dir(rom) if loadstate else None
        t0 = time.time()
        inst = emu.launch(self.exe, rom, d, config=cfg, state_dir=state, lua=lua)
        timed_out = False
        try:
            inst.proc.wait(timeout)
        except Exception:
            timed_out = True
            inst.kill()
            inst.proc.wait(10)
        p = Probe(inst, inst.poll(), timed_out, time.time() - t0)
        inst.remove_exe()
        return p


def result(checks: List[dict], **facts) -> dict:
    """pass when every check has ok=True."""
    ok = bool(checks) and all(c["ok"] for c in checks)
    return {"status": "pass" if ok else "fail", "checks": checks, **facts}
