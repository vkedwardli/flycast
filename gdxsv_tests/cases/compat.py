"""Compatibility with older releases: rollback matches that mix this build with a release
(the rollback suite's A3/A4, two peers each, both seat orders); replays this build records,
played on the previous release."""

import os

from lib import assets, recheck, replayfile
from lib.case import Ctx, Skip, case, result
from rollback import run_suite
from cases.input import MATCH_LOCK
from cases.match import save_run
from cases.replay import play

PREVIOUS = "gdxsv-1.9.6"  # the latest release; bump it with each release
OLDER = "gdxsv-1.8.13"


def mixed(ctx, tag: str, release: str, lua=None, orders=("old-first", "new-first")):
    if recheck.ACTIVE:
        raise Skip("not recheckable: rbk_lib.run_match saves no record of its peers")
    rom = ctx.rom("dc2")
    s = run_suite.Settings(exe=ctx.exe, rom=rom, out=ctx.out, state_dir=assets.state_dir(rom))
    with MATCH_LOCK:
        bad, notes = run_suite._mixed(s, assets.release(release), tag, lua, orders)
    return {"failures": bad, "matches": notes}


def plain(ctx, tag: str, release: str) -> dict:
    r = mixed(ctx, tag, release)
    return result([{"what": "every match check passes (frames, winners, no errors)", "ok": not r["failures"], **r}],
                  release=release)


def desynced(ctx, tag: str, release: str) -> dict:
    # desync.lua holds player 1's HP on peer 2 only, so that peer leaves the others.
    r = mixed(ctx, tag, release, {2: run_suite.DESYNC}, ("old-first",))
    hit = [b for b in r["failures"] if any(sign in b for sign in run_suite.DESYNC_SIGNS)]
    return result([{"what": "the match checks catch the desync", "ok": bool(hit), **r}], release=release)


@case("A3", f"rollback match mixed with the previous release ({PREVIOUS})")
def mixed_previous(ctx):
    return plain(ctx, "A3", PREVIOUS)


@case("A3c", "A3 with desync.lua on peer 2: last frames or winners differ", control_of="A3")
def mixed_previous_control(ctx):
    return desynced(ctx, "A3c", PREVIOUS)


@case("A4", f"rollback match mixed with an older release ({OLDER})")
def mixed_older(ctx):
    return plain(ctx, "A4", OLDER)


@case("A4c", "A4 with desync.lua on peer 2: last frames or winners differ", control_of="A4")
def mixed_older_control(ctx):
    return desynced(ctx, "A4c", OLDER)


def release_play(ctx, tag: str, cut: bool) -> dict:
    m, files, checks = save_run(ctx, tag, save=True)
    if not checks[1]["ok"]:
        return result(checks)
    pb = files[0][0]
    if cut:  # inputs end halfway through the last round
        starts = replayfile.start_indexes(pb)
        n = len(replayfile.Log(pb).inputs)
        pb = os.path.join(ctx.out, "cut.pb")
        replayfile.truncate_inputs(files[0][0], pb, (starts[-1] + n) // 2)
    r = play(Ctx(assets.release(PREVIOUS), ctx.out), {"id": tag, "disk": "dc2", "log_file_version": "saved"},
             pb=pb, name=f"{tag}_play")
    # Releases before the e2e tests do not log where playback stopped: the round ends with its winner instead.
    played = (r["rc"] == 0 and not r["timed_out"] and r["rounds_in_file"] > 0
              and r["rounds_started"] == r["rounds_in_file"] and bool(r["winners_recorded"])
              and r["winners_seen"] == r["winners_recorded"])
    return result(checks + [{**r, "what": f"{PREVIOUS} plays the replay to the end with its recorded winners",
                             "ok": played}])


@case("R4", f"a replay this build saves plays on the previous release ({PREVIOUS}), with its winners")
def replay_on_release(ctx):
    return release_play(ctx, "R4", cut=False)


@case("R4c", "R4 on a copy with the inputs cut in the last round: the check fails it", control_of="R4")
def replay_on_release_control(ctx):
    r = release_play(ctx, "R4c", cut=True)
    *base, last = r["checks"]
    return result(base + [{"what": "the cut replay fails the release check", **last, "ok": not last["ok"]}])
