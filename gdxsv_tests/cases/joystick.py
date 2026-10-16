"""Gamepad input through an SDL virtual gamepad (env GDXSV_TEST_JOYSTICK, gdxsv_input.cpp):
joystick polling on and off, opposite directions cancel (SOCD)."""

import os

from lib import emu
from lib.case import Skip, case, result

FRAMES = 600
JOYSTICK_NAME = "gdxsv test joystick"
# SDL_GameControllerButton
BTN_A, DPAD_LEFT, DPAD_RIGHT = 0, 13, 14
# kcode bits (active low)
DC_BTN_A = 1 << 2


def run(ctx, name: str, script: list, config: list, mapping: str = None) -> dict:
    """Headless DC2 lobby for FRAMES vblanks; the virtual gamepad plays `script`
    ((vblank, kind, index, value) steps) with `mapping` (a mapping file's text) if given;
    returns {vblank: (kcode, joyx)} of player 1."""
    os.makedirs(ctx.out, exist_ok=True)
    path = os.path.abspath(os.path.join(ctx.out, name + ".joystick"))
    with open(path, "w") as f:
        f.writelines(f"{v} {k} {i} {x}\n" for v, k, i, x in script)
    files = {f"mappings/SDL_{JOYSTICK_NAME}.cfg": mapping} if mapping else None
    p = ctx.probe(name, ctx.rom("dc2"), "joystick.lua", {"frames": FRAMES - 10}, frames=FRAMES,
                  config=config, timeout=120, env={"GDXSV_TEST_JOYSTICK": path}, files=files)
    seen = {}
    for k, v in p.e2e.items():
        if k.startswith("f"):
            kcode, joyx = v.split()
            seen[int(k[1:])] = (int(kcode, 16), int(joyx))
    return {"probe": p, "seen": dict(sorted(seen.items())),
            "attached": "test joystick: attached" in p.log}


def first(seen: dict, pred, after: int):
    return next((f for f, s in seen.items() if f >= after and pred(s)), None)


def press_release(ctx, name: str, polling: str, late: int = 0) -> dict:
    r = run(ctx, name, [(300 + late, "b", BTN_A, 1), (360 + late, "b", BTN_A, 0)],
            [f"input:JoystickPolling={polling}"])
    down = first(r["seen"], lambda s: not s[0] & DC_BTN_A, 300)
    up = first(r["seen"], lambda s: s[0] & DC_BTN_A, down or 360)
    return {"attached": r["attached"], "down": down, "up": up, "seen": r["seen"], **r["probe"].facts()}


def press_checks(r: dict) -> list:
    return [{"what": "virtual gamepad attached", "ok": r["attached"]},
            {"what": f"A down within 3 vblanks of 300 ({r['down']})", "ok": r["down"] is not None and r["down"] <= 303},
            {"what": f"A up within 3 vblanks of 360 ({r['up']})", "ok": r["up"] is not None and 360 <= r["up"] <= 363}]


@case("J1", "joystick polling on: a virtual gamepad's A press and release reach player 1 within 3 vblanks",
      tier="smoke")
def polling_on(ctx):
    r = press_release(ctx, "J1", "yes")
    return result(press_checks(r), **r)


@case("J2", "joystick polling off (SDL events): a virtual gamepad's A press and release reach player 1 within 3 vblanks")
def polling_off(ctx):
    r = press_release(ctx, "J2", "no")
    return result(press_checks(r), **r)


@case("J1c", "J1's checks on presses played 10 vblanks late (as if polling lagged): they fail", control_of="J1")
def polling_control(ctx):
    r = press_release(ctx, "J1c", "yes", late=10)
    c = press_checks(r)
    return result([{"what": "virtual gamepad attached", "ok": c[0]["ok"]},
                   {"what": "the timing checks fail", "ok": not c[1]["ok"] and not c[2]["ok"]}], detail=c, **r)


# D-pad left and right bound to the left stick, as digital directions (gdxsv_input_digital_axis)
SOCD_MAPPING = f"""[digital]
bind0 = {DPAD_LEFT}:btn_analog_left
bind1 = {DPAD_RIGHT}:btn_analog_right

[emulator]
mapping_name = {JOYSTICK_NAME}
version = 4
"""
# (vblank, step, expected stick X 3 vblanks later)
SOCD_STEPS = [(300, ("b", DPAD_LEFT, 1), -32768), (330, ("b", DPAD_RIGHT, 1), 0),
              (360, ("b", DPAD_LEFT, 0), 32767), (390, ("b", DPAD_RIGHT, 0), 0)]


def socd(ctx, name: str, steps: list) -> dict:
    r = run(ctx, name, [(v,) + s for v, s, _ in steps], [], SOCD_MAPPING)

    def x_at(f):
        prev = [s[1] for v, s in r["seen"].items() if v <= f]
        return prev[-1] if prev else None
    got = [{"vblank": v + 3, "want": want, "joyx": x_at(v + 3)} for v, _, want in SOCD_STEPS]
    return {"attached": r["attached"], "got": got, "seen": r["seen"], **r["probe"].facts()}


def socd_checks(r: dict) -> list:
    names = ["left: full left", "left + right: centre", "right: full right", "none: centre"]
    return [{"what": "virtual gamepad attached", "ok": r["attached"]}] + \
        [{"what": f"{n} ({g['joyx']} at vblank {g['vblank']})", "ok": g["joyx"] == g["want"]} for n, g in zip(names, r["got"])]


@case("J3", "SOCD: d-pad bound to the left stick, left + right held together centres the stick, one alone is full deflection",
      tier="smoke")
def socd_cancel(ctx):
    r = socd(ctx, "J3", SOCD_STEPS)
    return result(socd_checks(r), **r)


@case("J3c", "J3's checks with right never pressed (no opposite input): the centre check fails", control_of="J3")
def socd_control(ctx):
    r = socd(ctx, "J3c", [x for x in SOCD_STEPS if x[1][1] != DPAD_RIGHT])
    c = socd_checks(r)
    return result([{"what": "left alone still full left", "ok": c[1]["ok"]},
                   {"what": "the left + right check fails", "ok": not c[2]["ok"]}], detail=c, **r)


SWITCH_LINES = ["Disabling XInput, using DirectInput", "Disabling DirectInput"]


def switches(ctx, name: str, config: list) -> dict:
    if not emu.IS_WINDOWS:
        raise Skip("Windows only (SDL XInput / DirectInput hints)")
    p = ctx.probe(name, ctx.rom("dc2"), "joystick.lua", {"frames": 50}, frames=60, config=config, timeout=60)
    return {"lines": {l: l in p.log for l in SWITCH_LINES}, **p.facts()}


@case("J4", "input:UseXInput=no and input:UseDirectInput=no turn off SDL's XInput and DirectInput drivers (Windows)")
def input_switches(ctx):
    r = switches(ctx, "J4", ["input:UseXInput=no", "input:UseDirectInput=no"])
    return result([{"what": f"logged '{l}'", "ok": ok} for l, ok in r["lines"].items()], **r)


@case("J4c", "J4's checks with both switches at their defaults (on): they fail", control_of="J4")
def input_switches_control(ctx):
    r = switches(ctx, "J4c", ["input:UseXInput=yes", "input:UseDirectInput=yes"])
    return result([{"what": "no switch line logged", "ok": not any(r["lines"].values())}], **r)
