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


SEEK_RE = re.compile(r"JumpToKeyMsg \d+->(\d+) target=\d+ load=-?\d+ ran=(\d+) frames in \d+\[ms\] \(([\d.]+)\[ms/fr\]\)")
SEEK_FRAME = 6000  # in round 1 (target 8000 landed on round 2's start, 6376)
# Measured on windows-mini (newest Disc 2 replay, 6400 frames run): hack on 2.0-2.1 ms/fr, off 3.1-3.3.
SEEK_SPEEDUP = 0.85


def seek_run(ctx, name: str, hack: bool) -> dict:
    r = play(ctx, newest_dc2(), name=name,
             extra=[f"gdxsv:replay_target_frame={SEEK_FRAME}", f"gdxsv:SkipRenderingHack={'yes' if hack else 'no'}"])
    with open(os.path.join(ctx.out, name, "flycast.log"), encoding="utf-8", errors="replace") as f:
        seeks = SEEK_RE.findall(f.read())
    r["seek"] = [{"landed": int(k), "ran": int(n), "ms_per_frame": float(ms)} for k, n, ms in seeks]
    return r


def seek_checks(fast: dict, slow: dict) -> list:
    one = all(len(r["seek"]) == 1 and r["seek"][0]["landed"] == SEEK_FRAME for r in (fast, slow))
    return [
        {"what": f"each run: one seek, landed on input {SEEK_FRAME}", "ok": one},
        {"what": "each run: every round played to the end with the recorded winners", "ok": fast["ok"] and slow["ok"]},
        {"what": f"seek with the hack under {SEEK_SPEEDUP} x the ms per frame without it",
         "ok": one and fast["seek"][0]["ms_per_frame"] < SEEK_SPEEDUP * slow["seek"][0]["ms_per_frame"],
         "ms_per_frame": [r["seek"][0]["ms_per_frame"] if r["seek"] else None for r in (fast, slow)]},
    ]


@case("K1", "fast seek: SkipRenderingHack seeks faster per frame, same outcome as without it")
def replay_fast_seek(ctx):
    fast, slow = seek_run(ctx, "K1_on", True), seek_run(ctx, "K1_off", False)
    return result(seek_checks(fast, slow), on=fast, off=slow)


@case("K1c", "K1's checks with the hack off in both runs: the faster check fails", control_of="K1")
def replay_fast_seek_control(ctx):
    a, b = seek_run(ctx, "K1c_a", False), seek_run(ctx, "K1c_b", False)
    c = seek_checks(a, b)
    return result([{"what": "both runs seeked and played", "ok": c[0]["ok"] and c[1]["ok"]},
                   {"what": "faster check fails", "ok": not c[2]["ok"]}], detail=c)


NAMES = 0x0c3d1b04  # pilot names in the battle, 16 bytes per player (SJIS)
PILOT = "826f8268826b826e8273824f"  # ＰＩＬＯＴ０


def scenes_run(ctx, name: str, extra: list) -> dict:
    e = newest_dc2()
    p = ctx.probe(name, ctx.rom("dc2"), "replay_scenes.lua",
                  {"max": 12000, "after": 60, "reads": [[NAMES + 16 * i, 14] for i in range(4)]}, frames=12100,
                  config=[f"gdxsv:replay={assets.replay(e)}", "gdxsv:ReplaySeekStates=no", *extra],
                  loadstate=False, until_done=True, timeout=300)
    names = [p.e2e.get(f"0x{NAMES + 16 * i:08x}", "") for i in range(4)]
    return {"clean": p.clean, "battle": p.e2e.get("battle"), "names": names,
            **p.facts()}


def hide_name_check(r: dict) -> dict:
    want = [PILOT + f"82{0x50 + i:02x}" for i in range(4)]
    return {"what": "players are named ＰＩＬＯＴ０1 to ０4 in the battle",
            "ok": r["clean"] and [n[:len(want[0])] for n in r["names"]] == want, "names": r["names"]}


@case("V4", "replay hidden names: ReplayHideName shows the players as ＰＩＬＯＴ０1-4 in the battle")
def replay_hide_name(ctx):
    return result([hide_name_check(scenes_run(ctx, "V4", ["gdxsv:ReplayHideName=yes"]))])


@case("V4c", "V4's check without ReplayHideName: the recorded names show", control_of="V4")
def replay_hide_name_control(ctx):
    r = scenes_run(ctx, "V4c", [])
    return result([{"what": "battle reached", "ok": r["clean"] and r["battle"] is not None},
                   {"what": "name check fails", "ok": not hide_name_check(r)["ok"]}], detail=r)


def four_screen_run(ctx, name: str, four: bool) -> dict:
    """Newest Disc 2 replay in a window with ReplayFourScreen; logging and the Lua script go in
    emu.cfg, since the host starts its guests without its --config values."""
    e = newest_dc2()
    rom = ctx.rom("dc2")
    cfg = [f"gdxsv:replay={assets.replay(e)}", "gdxsv:ReplaySeekStates=no", "window:width=480", "window:height=360"]
    if four:
        cfg.append("gdxsv:ReplayFourScreen=yes")
    p = ctx.probe(name, rom, "fourscreen.lua", {"frame": POV_FRAME, "after": 300, "side": POV_SIDE, "id": POV_ID},
                  frames=POV_FRAME + 400, config=cfg, loadstate=False, window=True, timeout=300,
                  states=assets.state_dir(rom),
                  emu_cfg=f"[config]\nLuaFileName = {name}.lua\n\n[log]\nLogToFile = 1\nVerbosity = 3\n")
    d = os.path.join(ctx.out, name)
    povs = {1: (p.e2e.get("side"), p.e2e.get("id"))}
    logs = {}
    for n in (2, 3, 4):
        vals = {}
        try:
            with open(os.path.join(d, f"e2e_pov{n}.log"), encoding="utf-8") as f:
                vals = dict(l[4:].strip().split("=", 1) for l in f if l.startswith("E2E "))
        except OSError:
            pass
        povs[n] = (vals.get("side"), vals.get("id"))
        try:
            with open(os.path.join(d, f"flycast-{n}P.log"), encoding="utf-8", errors="replace") as f:
                logs[n] = f.read()
        except OSError:
            logs[n] = ""
    return {"clean": p.clean, "host_log": p.log, "povs": povs, "logs": logs,
            **p.facts()}


def four_screen_checks(r: dict) -> list:
    host = re.search(r"multi-pov: host session (\S+) with (\d+) guests", r["host_log"])
    session = host.group(1) if host else None
    checks = [{"what": "host started a session with 3 guests", "ok": bool(host) and host.group(2) == "3"}]
    for n, (side, id) in r["povs"].items():
        checks.append({"what": f"screen {n}: the game plays as player {n}", "side": side, "id": id,
                       "ok": side == str(n - 1) and id == str(n)})
    for n, log in r["logs"].items():
        checks.append({"what": f"guest {n}P joined the host's sync group and plays the host's replay",
                       "ok": session is not None and f"joined group {session}" in log
                       and f"guest {n}P playing from the host's replay" in log
                       and "start barrier released" in log})
    return checks


@case("V5", "four-screen replay: the host starts three guests on one sync group, each plays as its player")
def replay_four_screen(ctx):
    r = four_screen_run(ctx, "V5", True)
    return result([{"what": "probe ran", "ok": r["clean"]}] + four_screen_checks(r))


@case("V5c", "V5's checks on a replay played without ReplayFourScreen: no guests, the guest checks fail",
      control_of="V5")
def replay_four_screen_control(ctx):
    r = four_screen_run(ctx, "V5c", False)
    c = four_screen_checks(r)
    guests = [x for x in c if "guest" in x["what"] or x["what"].startswith(("screen 2", "screen 3", "screen 4"))]
    return result([{"what": "probe ran", "ok": r["clean"]},
                   {"what": "every guest check fails", "failed": [x["what"] for x in guests if not x["ok"]],
                    "ok": all(not x["ok"] for x in guests)}], detail=c)


TAKEOVER_FRAME = 3000
TAKEOVER_RE = re.compile(r"(StartTakeover|RetryTakeover) at key_msg_count_:(\d+)")
# B X Y C Z on the pad: MCS B X Y LT RT (0xb003), held together in no corpus replay
TAKEOVER_HOLD = 0x0703
SAVE_INTERVAL = 60  # ReplaySeekFrames: Take Over resumes from the newest state saved before the frame


def takeover_run(ctx, name: str, takeover: bool) -> dict:
    e = newest_dc2()
    extra = [f"gdxsv:replay_takeover_frame={TAKEOVER_FRAME}"] if takeover else []
    p = ctx.probe(name, ctx.rom("dc2"), "takeover.lua",
                  {"max": 12000, "window": 3600, "hold": TAKEOVER_HOLD, "mcs": 0xb003, "run": 60, "inetbuf": 0x0c3ab984},
                  frames=16000, config=[f"gdxsv:replay={assets.replay(e)}", "gdxsv:ReplaySeekStates=no",
                                        f"gdxsv:replay_target_frame={TAKEOVER_FRAME}", *extra],
                  loadstate=False, window=True, timeout=400)
    return {"clean": p.clean, "e2e": dict(p.e2e), "events": TAKEOVER_RE.findall(p.log), **p.facts()}


def takeover_checks(r: dict) -> list:
    at = [int(n) for _, n in r["events"]]
    kinds = [k for k, _ in r["events"]]
    return [{"what": f"takeover started at the last saved state before input {TAKEOVER_FRAME} (states every "
                     f"{SAVE_INTERVAL})", "events": r["events"],
             "ok": kinds[:1] == ["StartTakeover"] and TAKEOVER_FRAME - SAVE_INTERVAL <= at[0] < TAKEOVER_FRAME},
            {"what": "the game takes this pad's input, not the recording", "held": r["e2e"].get("held"),
             "ok": r["e2e"].get("held", "none") != "none"},
            {"what": "START retries from the same input, then plays on", "held2": r["e2e"].get("held2"),
             "ok": kinds[:3] == ["StartTakeover", "RetryTakeover", "StartTakeover"] and len(set(at[:3])) == 1
             and r["e2e"].get("held2", "none") != "none"}]


@case("V6", f"replay takeover: gdxsv:replay_takeover_frame={TAKEOVER_FRAME}, the pad plays from that input, "
            "START retries from it")
def replay_takeover(ctx):
    r = takeover_run(ctx, "V6", True)
    return result([{"what": "battle shown", "ok": "battle" in r["e2e"]}] + takeover_checks(r), **r)


@case("V6c", "V6's checks on the replay played without a takeover: they fail", control_of="V6")
def replay_takeover_control(ctx):
    r = takeover_run(ctx, "V6c", False)
    c = takeover_checks(r)
    return result([{"what": "battle shown", "ok": "battle" in r["e2e"]},
                   {"what": "every takeover check fails", "ok": all(not x["ok"] for x in c)}], detail=c, **r)
