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


def play(ctx, entry: dict, pb: str = None, extra: list = (), name: str = None) -> dict:
    """Play one replay headless to its end. It played when the process exited 0 by itself, every
    round in the file started, and playback stopped after the last round's start. Replays that
    record each round's winner (round_data) must also end each round with that winner."""
    rom = ctx.rom(entry["disk"])
    pb = pb or assets.replay(entry)
    p = ctx.run(name or "R_" + entry["id"], rom, [f"gdxsv:replay={pb}", "gdxsv:ReplayPOV=1", "gdxsv:ReplaySeekStates=no", *extra],
                timeout=900, script="outcome.lua", params=OUTCOME[entry["disk"]])
    log = p.log
    m = STARTS_RE.search(log)
    starts = [int(x) for x in m.group(1).split()] if m else []
    round_starts = [int(x) for x in ROUND_RE.findall(log)]
    rounds = len(round_starts)
    if not starts:  # files that list no round starts (log_file_version 20230426): the rounds that ran
        starts = round_starts
    stops = STOP_RE.findall(log)
    stop = int(stops[-1][0]) if stops else None
    inputs = int(stops[-1][1]) if stops else None
    played = (p.clean and bool(starts) and rounds == len(starts) and stop is not None and stop > starts[-1])
    # Recorded in the file (round_data), else as a past build played it (corpus "winners").
    want = replayfile.win_teams(pb) or entry.get("winners", [])
    got = [int(p.e2e[k]) for k in sorted((k for k in p.e2e if k.startswith("round")), key=lambda k: int(k[5:]))]
    outcome_ok = not want or got == want
    return {"id": entry["id"], "disk": entry["disk"], "version": entry["log_file_version"],
            "ok": played and outcome_ok, "played": played,
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


# Bytes that follow the replay's point of view: the game stores the player side the replay
# backend answers (lbsAskPlayerSide = ReplayPOV). Found by RAM diffs of POV 1-4 runs of two
# corpus replays; stable from the battle on.
POV_SIDE = 0x0c3d19d9  # POV - 1
POV_ID = 0x0c391d92  # POV
POV_FRAME = 4000  # vblanks after boot: in the first round


def pov_run(ctx, name: str, entry: dict, pov) -> dict:
    """Read the POV bytes at POV_FRAME of a replay played with gdxsv:ReplayPOV=pov (None: unset)."""
    cfg = [f"gdxsv:replay={assets.replay(entry)}", "gdxsv:ReplaySeekStates=no"]
    if pov is not None:
        cfg.append(f"gdxsv:ReplayPOV={pov}")
    p = ctx.probe(name, ctx.rom(entry["disk"]), "memread.lua",
                  {"frame": POV_FRAME, "reads": [[POV_SIDE, 1], [POV_ID, 1]]},
                  frames=POV_FRAME + 20, config=cfg, loadstate=False, until_done=True, timeout=300)
    return {"pov": pov, "side": p.e2e.get(f"0x{POV_SIDE:08x}:1"), "id": p.e2e.get(f"0x{POV_ID:08x}:1"),
            "clean": p.clean, **p.facts()}


def pov_checks(runs: list, want: list) -> list:
    return [{"what": f"ReplayPOV {w}: the game plays as player {w}",
             "ok": r["clean"] and r["side"] == str(w - 1) and r["id"] == str(w), **r} for r, w in zip(runs, want)]


@case("V1", "replay point of view: the game plays the replay as the chosen player (ReplayPOV 2 and 4)")
def replay_pov(ctx):
    e = newest_dc2()
    with concurrent.futures.ThreadPoolExecutor(2) as ex:
        runs = list(ex.map(lambda pov: pov_run(ctx, f"V1_pov{pov}", e, pov), (2, 4)))
    return result(pov_checks(runs, [2, 4]))


@case("V1c", "V1's check for POV 2 on a replay played without ReplayPOV: it fails", control_of="V1")
def replay_pov_control(ctx):
    r = pov_run(ctx, "V1c", newest_dc2(), None)
    c = pov_checks([r], [2])[0]
    return result([{"what": "probe ran", "ok": r["clean"]},
                   {"what": "POV 2 check fails (player 1 by default)", "ok": not c["ok"] and r["id"] == "1"}],
                  detail=c)


@case("V2", "replay round jump at start: gdxsv:replay_target_round=4 plays the last two rounds of five")
def replay_round_jump(ctx):
    r, log = jump_run(ctx, "V2", 4)
    return result(jump_checks(r, log, 4), **r)


@case("V2c", "V2's checks on the replay played from round 1: they fail", control_of="V2")
def replay_round_jump_control(ctx):
    r, log = jump_run(ctx, "V2c", None)
    c = jump_checks(r, log, 4)
    return result([{"what": "replay played to its end", "ok": c[0]["ok"] and c[3]["ok"]},
                   {"what": "jump checks fail", "ok": not c[1]["ok"] and not c[2]["ok"]}], detail=c)


def jump_run(ctx, name: str, round):
    e = newest_dc2()
    extra = [f"gdxsv:replay_target_round={round}"] if round else []
    r = play(ctx, e, extra=extra, name=name)
    with open(os.path.join(ctx.out, name, "flycast.log"), encoding="utf-8", errors="replace") as f:
        return r, f.read()


def jump_checks(r: dict, log: str, round: int) -> list:
    starts = [int(x) for x in STARTS_RE.search(log).group(1).split()] if STARTS_RE.search(log) else []
    played = [int(x) for x in ROUND_RE.findall(log)]
    # The game's first round starts at frame 0, then the jump lands on round `round`.
    return [
        {"what": "process ended by itself, no errors", "ok": r["rc"] == 0 and not r["timed_out"] and not r["errors"]},
        {"what": f"one jump to round {round}", "ok": log.count(f"ctrl_change_round_:{round} ") == 1},
        {"what": "rounds 2 to the jump skipped, the later ones played",
         "ok": bool(starts) and played == [0] + starts[round:]
         and len(r["winners_seen"]) == len(starts) - round + 1},
        {"what": "playback stopped at the last input", "ok": r["inputs"] is not None and r["stopped_at"] == r["inputs"]},
    ]


FRAME_JUMP_RE = re.compile(r"JumpToKeyMsg \d+->(\d+) target=(\d+)")


@case("V3", "replay seek at start: gdxsv:replay_target_frame=4000 lands on input 4000, then plays to the end")
def replay_frame_jump(ctx):
    r = play(ctx, newest_dc2(), extra=["gdxsv:replay_target_frame=4000"], name="V3")
    return result(frame_jump_checks(r, ctx.out, "V3", 4000), **r)


@case("V3c", "V3's checks on the replay played without a target frame: the seek check fails", control_of="V3")
def replay_frame_jump_control(ctx):
    r = play(ctx, newest_dc2(), name="V3c")
    c = frame_jump_checks(r, ctx.out, "V3c", 4000)
    return result([{"what": "replay played", "ok": c[1]["ok"]}, {"what": "seek check fails", "ok": not c[0]["ok"]}],
                  detail=c)


def frame_jump_checks(r: dict, out: str, name: str, frame: int) -> list:
    with open(os.path.join(out, name, "flycast.log"), encoding="utf-8", errors="replace") as f:
        jumps = FRAME_JUMP_RE.findall(f.read())
    return [{"what": f"one seek, landed on input {frame}", "ok": jumps == [(str(frame), str(frame))], "jumps": jumps},
            {"what": "every round played to the end with the recorded winners", "ok": r["ok"]}]
