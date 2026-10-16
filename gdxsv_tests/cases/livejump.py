"""Live round jump: a spectator joining a live battle mid-match jumps to the current round, and the
game's round counters (wins, losses, draws) are rebuilt from the earlier rounds' results.
Live auto-next: a spectator moves on to the next live battle when one ends.
rollback/fake_live_lbs.py streams a corpus replay as the live battles."""

import os
import socket
import subprocess
import sys
import time

from lib import assets, emu, recheck, replayfile, servers
from lib.case import case, result
from cases.replay import newest_dc2

FAKE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "rollback", "fake_live_lbs.py")
SESSION_POINTER = 0x0c394524  # Disc 2, gdxsv_round_counters.h kSessionPointer
CODE = "9000000"  # fake_live_lbs.py's first battle
LEAD = 5.0  # seconds into the last round when the spectator joins


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def jump_run(ctx, name: str, results: bool):
    e = newest_dc2()
    pb = assets.replay(e)
    starts = replayfile.start_indexes(pb)
    out = os.path.join(ctx.out, name)
    os.makedirs(out, exist_ok=True)
    http = free_port()
    cmd = [sys.executable, FAKE, "--replay", pb, "--battles", "1", "--lead", str(starts[-1] / 59.94 + LEAD),
           "--http-port", str(http)] + ([] if results else ["--no-round-results"])
    with servers.LOBBY_LOCK:  # the fake takes lbs's spectator UDP port
        fake = None
        if not recheck.ACTIVE:
            log = open(os.path.join(out, "fake_live_lbs.log"), "w")
            fake = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
            servers.wait_port(http, 10)
        try:
            p = ctx.run(name, ctx.rom("dc2"), ["gdxsv:server=127.0.0.1", f"gdxsv:LiveApiUrl=http://127.0.0.1:{http}",
                                               f"gdxsv:spectate={CODE}", "gdxsv:LiveAutoNext=no",
                                               "gdxsv:ReplayPOV=1", "gdxsv:ReplaySeekStates=no"],
                        timeout=600, script="counters.lua", params={"session": SESSION_POINTER})
        finally:
            if fake:
                fake.kill()
                fake.wait()
                log.close()
    return p, starts, replayfile.win_teams(pb)


def expected(wins: list, completed: int, team: int) -> tuple:
    """gdxsv_round_counters::BeforeRound: (completed, wins, losses, draws) after `completed` rounds."""
    w = sum(1 for x in wins[:completed] if x == team)
    d = sum(1 for x in wins[:completed] if x == -1)
    return completed, w, completed - w - d, d


def counters(p) -> dict:
    """slot -> {completed: last (completed, wins, losses, draws, team) logged with that count}."""
    seen = {}
    for k in sorted((k for k in p.e2e if k[0] == "c" and k[1:].isdigit()), key=lambda k: int(k[1:])):
        slot, vals = p.e2e[k].split(":")
        v = tuple(int(x) for x in vals.split(","))
        seen.setdefault(int(slot), {})[v[0]] = v
    return seen


def counter_check(seen: dict, wins: list, completed: int, what: str) -> dict:
    rows, ok = [], bool(seen)
    for slot, by in sorted(seen.items()):
        got = by.get(completed)
        team = got[4] if got else 0
        want = expected(wins, completed, team)
        rows.append({"slot": slot, "team": team, "got": got[:4] if got else None, "want": want})
        ok = ok and team in (1, 2) and got is not None and got[:4] == want
    return {"what": what, "players": rows, "ok": ok}


def jump_checks(p, starts: list, wins: list) -> list:
    last = len(starts)
    seen = counters(p)
    return [
        {"what": "spectator ended by itself, no errors", "ok": p.clean, **p.facts()},
        {"what": f"joined in round {last} of {last}: one jump to it",
         "ok": p.log.count(f"ctrl_change_round_:{last} ") == 1},
        {"what": "earlier rounds have decisive results (the check below can fail)",
         "wins": wins, "ok": len(wins) >= last - 1 and any(x in (1, 2) for x in wins[:last - 1])},
        counter_check(seen, wins, last - 1, f"every player's counters in round {last} = the results of rounds 1-{last - 1}"),
        counter_check(seen, wins, last, f"after round {last}, the game's own count on them = the results of rounds 1-{last}"),
    ]


@case("S2", "live round jump: a spectator joining in the last round sees each player's wins, losses and draws "
            "of the earlier rounds (fake live lobby)")
def live_round_counters(ctx):
    p, starts, wins = jump_run(ctx, "S2", True)
    c = jump_checks(p, starts, wins)
    c.append({"what": f"log: Live round {len(starts)} counters: restored",
              "ok": f"Live round {len(starts)} counters: restored" in p.log})
    return result(c)


@case("S2c", "S2 with no round results on the live channel: the counter checks fail", control_of="S2")
def live_round_counters_control(ctx):
    p, starts, wins = jump_run(ctx, "S2c", False)
    c = jump_checks(p, starts, wins)
    return result([{"what": "spectator joined and jumped", "ok": c[0]["ok"] and c[1]["ok"]},
                   {"what": "both counter checks fail", "ok": c[2]["ok"] and not c[3]["ok"] and not c[4]["ok"]}],
                  detail=c)


NEXT = "9000001"  # fake_live_lbs.py's second battle
LIVE_FRAMES = 1800  # each battle stays live this long after the join point (the spectator boots in ~6 s)


def autonext_run(ctx, name: str, auto: bool) -> str:
    """A windowed spectator (headless exits at the battle end) joins battle 1 of two fake live
    battles, each LEAD s into its last round; returns its flycast.log."""
    pb = assets.replay(newest_dc2())
    last = replayfile.start_indexes(pb)[-1]
    out = os.path.join(ctx.out, name)
    os.makedirs(out, exist_ok=True)
    rom = ctx.rom("dc2")
    http = free_port()
    cmd = [sys.executable, FAKE, "--replay", pb, "--battles", "2", "--lead", str(last / 59.94 + LEAD),
           "--max-frames", str(last + LIVE_FRAMES), "--gap", "3", "--http-port", str(http)]
    with servers.LOBBY_LOCK:
        fake = None
        if not recheck.ACTIVE:
            log = open(os.path.join(out, "fake_live_lbs.log"), "w")
            fake = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
            servers.wait_port(http, 10)
        try:
            inst = emu.launch(ctx.exe, rom, os.path.join(out, "spectator"), headless=False,
                              state_dir=assets.state_dir(rom), config=[
                                  "gdxsv:server=127.0.0.1", f"gdxsv:LiveApiUrl=http://127.0.0.1:{http}",
                                  f"gdxsv:spectate={CODE}", f"gdxsv:LiveAutoNext={'yes' if auto else 'no'}",
                                  "gdxsv:ReplayPOV=1", "gdxsv:ReplaySeekStates=no", "gdxsv:ReplayFourScreen=no",
                                  "gdxsv:ConnectionMedium=Wired", "config:aica.Volume=0",
                                  "log:Verbosity=3", "log:LogToFile=1"])
            t0 = time.time()
            # Battle 1 ends LIVE_FRAMES after the join; battle 2 is joined after the 3 s gap, a 5 s
            # autoplay poll and a ~6 s boot.
            while inst.poll() is None and time.time() - t0 < 90:
                if inst.log.count("counters: restored") >= 2:
                    break
                time.sleep(1)
            inst.kill()
            inst.remove_exe()
        finally:
            if fake:
                fake.kill()
                fake.wait()
                log.close()
    return inst.log


def autonext_checks(text: str) -> list:
    return [
        {"what": "battle 1 joined: its round counters restored", "ok": "counters: restored" in text},
        {"what": f"log: live autoplay: moving on to {NEXT}", "ok": f"live autoplay: moving on to {NEXT}" in text},
        {"what": f"battle {NEXT} joined: its round counters restored too",
         "got": text.count("counters: restored"), "ok": text.count("counters: restored") >= 2},
    ]


@case("S5", "live auto-next: when a watched live battle ends, the spectator moves on to the next one "
            "(fake live lobby with two battles)")
def live_autonext(ctx):
    return result(autonext_checks(autonext_run(ctx, "S5", True)))


@case("S5c", "S5 with gdxsv:LiveAutoNext=no: battle 1 is watched, the spectator does not move on",
      control_of="S5")
def live_autonext_control(ctx):
    c = autonext_checks(autonext_run(ctx, "S5c", False))
    return result([c[0], {"what": "both move-on checks fail", "ok": not c[1]["ok"] and not c[2]["ok"]}], detail=c)
