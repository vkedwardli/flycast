"""Replays recorded by past releases still play (corpus/replays.json)."""

import concurrent.futures
import os
import re

from lib import assets, replayfile
from lib.case import case, result

STARTS_RE = re.compile(r"start_msg_indexes = ([\d ]*)")
ROUND_RE = re.compile(r"StartMsg key_msg_count (\d+)")
STOP_RE = re.compile(r"replay stopped at key_msg_count_ (\d+) of (\d+)")

# Winner byte and draw flag (core/gdxsv/gdxsv_round_counters.h, gdxsv_backend_rollback.cpp)
OUTCOME = {"dc1": {"winner": 0x0c3364b6, "draw": 0x0c3364c9},
           "dc2": {"winner": 0x0c3d1948, "draw": 0x0c3d1950}}


def play(ctx, entry: dict, pb: str = None) -> dict:
    """Play one replay headless to its end. It played when the process exited 0 by itself, every
    round in the file started, and playback stopped after the last round's start. Replays that
    record each round's winner (round_data) must also end each round with that winner."""
    rom = ctx.rom(entry["disk"])
    pb = pb or assets.replay(entry)
    p = ctx.run("R_" + entry["id"], rom, [f"gdxsv:replay={pb}", "gdxsv:ReplayPOV=1", "gdxsv:ReplaySeekStates=no"],
                timeout=900, script="outcome.lua", params=OUTCOME[entry["disk"]])
    log = p.log
    m = STARTS_RE.search(log)
    starts = [int(x) for x in m.group(1).split()] if m else []
    rounds = len(ROUND_RE.findall(log))
    stops = STOP_RE.findall(log)
    stop = int(stops[-1][0]) if stops else None
    inputs = int(stops[-1][1]) if stops else None
    played = (p.clean and bool(starts) and rounds == len(starts) and stop is not None and stop > starts[-1])
    want = replayfile.win_teams(pb)
    got = [int(p.e2e[k]) for k in sorted((k for k in p.e2e if k.startswith("round")), key=lambda k: int(k[5:]))]
    outcome_ok = not want or got == want
    return {"id": entry["id"], "disk": entry["disk"], "version": entry["log_file_version"],
            "expect": entry.get("expect", "play"), "ok": played and outcome_ok, "played": played,
            "inputs": inputs, "stopped_at": stop, "rounds_in_file": len(starts), "rounds_started": rounds,
            "winners_recorded": want, "winners_seen": got, **p.facts()}


def play_all(ctx, entries: list) -> list:
    entries = [e for e in entries if assets.rom(e["disk"])]
    jobs = int(os.environ.get("GDXSV_REPLAY_JOBS", "4"))
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        return list(pool.map(lambda e: play(ctx, e), entries))


@case("R1", "replays of every format in the corpus play to the end, with the recorded winners", tier="full")
def replay_compat(ctx):
    entries = assets.corpus()
    checks = play_all(ctx, entries)
    skipped = sorted({e["disk"] for e in entries} - {c["disk"] for c in checks})
    return result(checks, skipped_disks=skipped)


@case("R2", "the newest Disc 2 replay in the corpus plays to the end, with the recorded winners", tier="smoke")
def replay_newest(ctx):
    return result([play(ctx, newest_dc2())])


def newest_dc2() -> dict:
    return max((e for e in assets.corpus() if e["disk"] == "dc2"), key=lambda e: e["date"])


@case("R2c", "R2 on a copy with the inputs cut in the third round: the check fails it", control_of="R2")
def replay_newest_control(ctx):
    entry = newest_dc2()
    pb = assets.replay(entry)
    starts = replayfile.start_indexes(pb)
    os.makedirs(ctx.out, exist_ok=True)
    cut = os.path.join(ctx.out, entry["id"] + "_cut.pb")
    replayfile.truncate_inputs(pb, cut, (starts[2] + starts[3]) // 2)
    c = play(ctx, entry, pb=cut)
    return result([{"what": "the cut replay fails the check", "ok": not c["ok"]}], detail=c)
