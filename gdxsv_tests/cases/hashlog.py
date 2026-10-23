"""Per-frame state hash log (gdxsv:hashlog=yes) of a local rollback match."""

import os
import re

from lib import assets, hashlog, recheck
from lib.case import Skip, case, result
from rollback import rbk_lib
from cases.input import MATCH_LOCK

# Network delay (ms) per peer, so the match rolls back.
DELAYS = [0, 4, 0, 4]


def run(ctx, tag: str, hashlog_on: bool):
    if recheck.ACTIVE:
        raise Skip("not recheckable: rbk_lib.run_match saves no record of its peers")
    rom = ctx.rom("dc2")
    s = rbk_lib.Settings(exe=ctx.exe, rom=rom, out=ctx.out, state_dir=assets.state_dir(rom))
    os.makedirs(ctx.out, exist_ok=True)
    # GGPO_TEST_LOG: each rollback logs "RBKTEST load seek=<loaded frame> from=<current frame>".
    with MATCH_LOCK:
        return rbk_lib.run_match(s, tag, [ctx.exe] * rbk_lib.N, DELAYS, env={"GGPO_TEST_LOG": "1"},
                                 config=[f"gdxsv:hashlog={'yes' if hashlog_on else 'no'}"], timeout=600)


def loads(peer) -> list:
    return [(int(a), int(b)) for a, b in re.findall(r"RBKTEST load seek=(\d+) from=(\d+)", peer.log)]


def peer_checks(peer) -> dict:
    """The log holds every frame of the match, and re-saves seek+1..from after each rollback load."""
    path = peer.inst.hashlog_path
    frames, rows, ld = hashlog.frames(path), hashlog.read(path), loads(peer)
    want = [(f, s + 1) for s, f in ld]
    got = hashlog.rewinds(path)
    bad = [(g, w) for g, w in zip(got, want) if g != w]
    settled = hashlog.settled(path)
    last = max(frames) if frames else 0
    return {"peer": peer.index, "lines": len(rows), "last": last, "loads": len(ld), "rewinds": len(got),
            "mismatch": bad[:3], "distinct_hashes": len(set(settled.values())),
            "ok": last > 1000 and sorted(settled) == list(range(1, last + 1)) and len(ld) > 0
            and len(got) == len(ld) and not bad and len(set(settled.values())) > last // 2}


@case("Q1", "state hash log: every frame of a rollback match, each rollback's frames saved again")
def hash_log(ctx):
    m = run(ctx, "Q1", True)
    per = [peer_checks(p) for p in m.peers]
    return result([{"what": "match finished on every peer", "ok": all(p.finished for p in m.peers)},
                   {"what": "every peer: frames 1..last, one rewind per rollback load (to seek+1, from the "
                            "load's frame), the hash changes", "peers": per, "ok": all(c["ok"] for c in per)}])
