"""Case registry and the helpers the cases share.

A case is a function in cases/*.py decorated with @case. It gets a Ctx and returns a
dict with "status" ("pass" or "fail") and whatever details explain the result; raising
Skip marks it skipped (missing ROM). A control case (control_of=<id>) runs its case's
check on input where the feature is absent and passes when the check catches it.
A case with known_failure=<reason> fails for a reason not fixed yet: the runner reports
that failure as "known" (exit code 0) and flags a pass, so the mark gets dropped.
"""

import json
import os
import subprocess
import time
from typing import Dict, List, Optional

from . import emu, recheck

HERE = os.path.dirname(os.path.abspath(__file__))
LUA_DIR = os.path.join(os.path.dirname(HERE), "lua")

CASES: Dict[str, "Case"] = {}


class Case:
    def __init__(self, id: str, title: str, tier: str, fn, control_of: Optional[str],
                 known_failure: Optional[str] = None):
        self.id = id
        self.title = title
        self.tier = tier
        self.fn = fn
        self.control_of = control_of
        self.known_failure = known_failure


def case(id: str, title: str, tier: str = "full", control_of: Optional[str] = None,
         known_failure: Optional[str] = None):
    assert tier in ("smoke", "full"), tier

    def register(fn):
        assert id not in CASES, id
        CASES[id] = Case(id, title, tier, fn, control_of, known_failure)
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


def finish(inst: emu.Instance, rc: Optional[int], timed_out: bool, t0: float) -> Probe:
    """The Probe of a finished instance; its exit is saved, and read back in a recheck."""
    path = os.path.join(inst.dir, "probe.json")
    if recheck.ACTIVE:
        rec = recheck.load(path, {})
        return Probe(inst, rec.get("rc", rc), rec.get("timed_out", timed_out), rec.get("elapsed", 0))
    p = Probe(inst, rc, timed_out, time.time() - t0)
    recheck.save(path, {"rc": p.rc, "timed_out": p.timed_out, "elapsed": p.elapsed})
    return p


class Ctx:
    def __init__(self, exe: str, out: str):
        self.exe = exe
        self.out = out

    def rom(self, name: str) -> str:
        from . import assets
        path = assets.rom(name)
        if path is None and recheck.ACTIVE:
            return name  # never opened in a recheck
        if path is None:
            raise Skip(f"no {name} image (GDXSV_ROM_DIR or {assets.ROMS[name][0]})")
        return path

    def probe(self, name: str, rom: str, script: str, params: dict, frames: int,
              config: Optional[List[str]] = None, loadstate: bool = True, timeout: float = 180,
              window: bool = False, states: Optional[str] = None, until_done: bool = False,
              emu_cfg: Optional[str] = None, env: Optional[Dict[str, str]] = None,
              files: Optional[Dict[str, str]] = None) -> Probe:
        """Run one headless instance for `frames` vblanks (gdxsv:headless_probe_frames exits it)
        with lua/<script>; with loadstate it resumes the game's slot-99 lobby savestate.
        window=True: a normal window and a cold boot, killed once the script is done.
        states: a directory whose *.state files go to the data dir instead (cold boot).
        until_done: also headless, stop once the script is done (`frames` is then an upper bound).
        emu_cfg, env, files: see emu.launch."""
        stop_on_done = window or until_done
        from . import assets
        d = os.path.join(self.out, name)
        os.makedirs(self.out, exist_ok=True)
        lua = write_lua(os.path.join(self.out, name + ".lua"), script, params)
        cfg = [f"gdxsv:headless_probe_frames={frames}", "log:Verbosity=3", "log:LogToFile=1"] + list(config or [])
        if loadstate and not window:
            cfg.append("gdxsv:headless_loadstate=yes")
        state = assets.state_dir(rom) if loadstate and not window else states
        t0 = time.time()
        inst = emu.launch(self.exe, rom, d, config=cfg, state_dir=state, lua=lua, headless=not window,
                           emu_cfg=emu_cfg, env=env, files=files)
        timed_out = False
        e2e_log = os.path.join(d, "e2e.log")
        while inst.poll() is None:
            if time.time() - t0 > timeout:
                timed_out = True
                break
            if stop_on_done and os.path.isfile(e2e_log):
                with open(e2e_log, encoding="utf-8", errors="replace") as f:
                    s = f.read()
                if "E2E done=" in s or "E2E error=" in s:
                    break
            time.sleep(0.2)
        rc = inst.poll()
        if rc is None:
            inst.kill()
            inst.proc.wait(10)
            rc = 0 if stop_on_done and not timed_out else inst.poll()
        p = finish(inst, rc, timed_out, t0)
        inst.remove_exe()
        return p

    def run(self, name: str, rom: str, config: List[str], timeout: float, script: Optional[str] = None,
            params: Optional[dict] = None) -> Probe:
        """One headless instance until it exits by itself or the timeout; with lua/<script> if given."""
        cfg = ["log:Verbosity=3", "log:LogToFile=1"] + list(config)
        lua = None
        if script:
            os.makedirs(self.out, exist_ok=True)
            lua = write_lua(os.path.join(self.out, name + ".lua"), script, params or {})
        t0 = time.time()
        inst = emu.launch(self.exe, rom, os.path.join(self.out, name), config=cfg, lua=lua)
        try:
            rc = inst.proc.wait(timeout)
            timed_out = False
        except subprocess.TimeoutExpired:
            inst.kill()
            inst.proc.wait(10)
            rc, timed_out = inst.poll(), True
        p = finish(inst, rc, timed_out, t0)
        inst.remove_exe()
        return p


def result(checks: List[dict], **facts) -> dict:
    """pass when every check has ok=True."""
    ok = bool(checks) and all(c["ok"] for c in checks)
    return {"status": "pass" if ok else "fail", "checks": checks, **facts}
