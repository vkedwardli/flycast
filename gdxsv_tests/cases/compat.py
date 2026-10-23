"""Compatibility with older releases: replays this build records, played on the previous release.
Rollback matches mixed with a release are the rollback suite's A3/A4 (rollback/run_suite.py)."""

import os

from lib import assets, replayfile
from lib.case import Ctx, case, result
from cases.match import save_run
from cases.replay import play

PREVIOUS = "gdxsv-1.9.6"  # the latest release; bump it with each release


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
