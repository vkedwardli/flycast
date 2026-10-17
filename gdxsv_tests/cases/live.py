"""Live spectating: rollback/live_rig.py as a case (4 peers upload to a local lobby, one spectator),
also with the spectator or the peers on the previous release."""

import os
import re
import subprocess
import sys
import threading

from lib import assets, emu, recheck, servers
from lib.case import Skip, case, result
from cases.input import MATCH_LOCK

RIG = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "rollback", "live_rig.py")
HEADER_LOCK = threading.Lock()
# Input delay 3 with 30-80 ms between peers: the uplink sends rolled-back steps too.
LIVE = ["--rounds", "2", "--delays", "30,50,60,80", "--input-delay", "3"]


def rig(ctx, tag: str, args: list, timeout: int) -> str:
    if recheck.ACTIVE:  # the rig's saved output
        try:
            with open(os.path.join(ctx.out, tag, "rig.log"), encoding="utf-8") as f:
                return f.read()
        except OSError:
            recheck.CASE.missing.append(os.path.join(tag, "rig.log"))
            return ""
    exe = servers.server_exe()
    if exe is None:
        raise Skip("no GDXSV_SERVER_EXE (gdxsv with -spectator_test_session)")
    rom = ctx.rom("dc2")
    out = os.path.join(ctx.out, tag)
    os.makedirs(out, exist_ok=True)
    cmd = [sys.executable, RIG, "--rom", rom, "--exe", ctx.exe, "--gdxsv", exe, "--state-dir", assets.state_dir(rom),
           "--out", out, "--timeout", str(timeout)] + args
    env = dict(os.environ, TEST_FAKE_TIMESYNC="20")
    with servers.LOBBY_LOCK, MATCH_LOCK:
        p = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout + 300)
    text = p.stdout.decode("utf-8", "replace")
    with open(os.path.join(out, "rig.log"), "w", encoding="utf-8") as f:
        f.write(text)
    return text


def header(ctx) -> str:
    """A Disc 2 replay of a local match, so lbs hands spectators the peers' rule; one per run."""
    path = os.path.join(os.path.dirname(ctx.out), "live_header", "live", "p1", "data", "replays", "0123456.pb")
    if recheck.ACTIVE:
        return path
    with HEADER_LOCK:
        if not os.path.isfile(path):
            rig(ctx, os.path.join("..", "live_header"), ["--make-header", "--rounds", "1"], 600)
    if not os.path.isfile(path):
        raise RuntimeError("live_rig.py --make-header made no replay")
    return path


def checks(text: str) -> list:
    uplinks = re.search(r"uplink started on: \[(.*)\]", text)
    spec = re.search(r"^  s1: (\d+) steps, (\d+) shared with the replay, (\d+) differ", text, re.M)
    shared, differ = (int(spec.group(2)), int(spec.group(3))) if spec else (0, None)
    return [
        {"what": "a peer started its uplink", "got": uplinks.group(1) if uplinks else None,
         "ok": bool(uplinks and uplinks.group(1))},
        {"what": "lobby's recording equals one peer's replay", "ok": "<- recorded" in text},
        {"what": "spectator's state per step equals the replay played offline (>1000 steps shared, 0 differ)",
         "shared": shared, "differ": differ, "ok": shared > 1000 and differ == 0},
        {"what": "live_rig.py RESULT PASS", "ok": "RESULT PASS" in text},
    ]


@case("S1", "live spectating: peers upload to a local lobby, the recording is one peer's replay, "
            "a spectator follows it step by step (local gdxsv)")
def live_spectate(ctx):
    text = rig(ctx, "S1", ["--header", header(ctx)] + LIVE, 900)
    return result(checks(text), detail=text.splitlines()[-25:])


@case("S1c", "S1 with no uplink (no TEST_SPECTATOR_LBS on the peers): nothing recorded, nothing to watch",
      control_of="S1")
def live_spectate_control(ctx):
    text = rig(ctx, "S1c", ["--header", header(ctx), "--no-uplink"] + LIVE, 900)
    c = checks(text)
    return result([{"what": "every S1 check fails", "failed": [x["what"] for x in c if not x["ok"]],
                    "ok": not any(x["ok"] for x in c)}], detail=c)


PREVIOUS = "gdxsv-1.9.6"  # the latest release; bump it with each release


def release() -> str:
    return emu.fetch_release(PREVIOUS, os.environ.get("GDXSV_RELEASE_ROOT") or os.path.join(assets.TESTS, "out"))


def mixed(ctx, tag: str, side: str, no_uplink: bool) -> list:
    args = ["--header", header(ctx), f"--{side}-exe", "" if recheck.ACTIVE else release()] + LIVE
    return checks(rig(ctx, tag, args + (["--no-uplink"] if no_uplink else []), 900))


@case("S3", f"S1 with the spectator (and its offline reference play) on the previous release ({PREVIOUS})")
def live_old_spectator(ctx):
    return result(mixed(ctx, "S3", "spectator", False), release=PREVIOUS)


@case("S3c", "S3 with no uplink: every S3 check fails", control_of="S3")
def live_old_spectator_control(ctx):
    c = mixed(ctx, "S3c", "spectator", True)
    return result([{"what": "every S3 check fails", "failed": [x["what"] for x in c if not x["ok"]],
                    "ok": not any(x["ok"] for x in c)}], detail=c, release=PREVIOUS)


@case("S4", f"S1 with the 4 uploading peers on the previous release ({PREVIOUS}), the spectator on this build")
def live_old_peers(ctx):
    return result(mixed(ctx, "S4", "peer", False), release=PREVIOUS)


@case("S4c", "S4 with no uplink: every S4 check fails", control_of="S4")
def live_old_peers_control(ctx):
    c = mixed(ctx, "S4c", "peer", True)
    return result([{"what": "every S4 check fails", "failed": [x["what"] for x in c if not x["ok"]],
                    "ok": not any(x["ok"] for x in c)}], detail=c, release=PREVIOUS)
