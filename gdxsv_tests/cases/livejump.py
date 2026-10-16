"""Live round jump: a spectator joining a live battle mid-match jumps to the current round, and the
game's round counters (wins, losses, draws) are rebuilt from the earlier rounds' results.
rollback/fake_live_lbs.py streams a corpus replay as the live battle."""

import os
import socket
import subprocess
import sys

from lib import assets, recheck, replayfile, servers
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
