"""Compatibility with older releases: rollback matches that mix this build with a release
(the rollback suite's A3/A4, two peers each, both seat orders)."""

from lib import assets, recheck
from lib.case import Skip, case, result
from rollback import run_suite
from cases.input import MATCH_LOCK

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
