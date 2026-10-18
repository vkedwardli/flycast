"""A local 4-player rollback match (gdxsv:rbk_test): the replay it saves."""

import glob
import os

from lib import assets, recheck, replayfile
from lib.case import Skip, case, result
from rollback import rbk_lib
from cases.input import MATCH_LOCK
from cases.replay import play


def match(ctx, tag: str, seed: int = 55, env: dict = None, config: list = None):
    if recheck.ACTIVE:
        raise Skip("not recheckable: rbk_lib.run_match saves no record of its peers")
    rom = ctx.rom("dc2")
    s = rbk_lib.Settings(exe=ctx.exe, rom=rom, out=ctx.out, state_dir=assets.state_dir(rom))
    os.makedirs(ctx.out, exist_ok=True)
    with MATCH_LOCK:
        return rbk_lib.run_match(s, tag, [ctx.exe] * rbk_lib.N, [0] * rbk_lib.N, seed=seed, env=env,
                                 config=config, timeout=600)


def match_facts(m) -> dict:
    return {"timed_out": m.timed_out, "stalled": m.stalled, "elapsed": round(m.elapsed, 1),
            "finished": [p.finished for p in m.peers], "errors": [p.errors[:3] for p in m.peers]}


def saved_replays(m) -> list:
    return [sorted(glob.glob(os.path.join(p.inst.data_dir, "replays", "*.pb"))) for p in m.peers]


def save_run(ctx, tag: str, save: bool):
    # RBK_SAVE_REPLAY: a local test match saves its replay like a lobby battle (never uploaded).
    m = match(ctx, tag, env={"RBK_SAVE_REPLAY": "1"}, config=[f"gdxsv:SaveReplay={'yes' if save else 'no'}"])
    files = saved_replays(m)
    checks = [{"what": "match finished on every peer", "ok": all(p.finished for p in m.peers), **match_facts(m)},
              {"what": "every peer saved one replay", "files": files, "ok": all(len(f) == 1 for f in files)}]
    return m, files, checks


@case("R3", "a rollback match saves its replay: one file per peer, the same inputs, it plays to the end")
def replay_save(ctx):
    m, files, checks = save_run(ctx, "R3", save=True)
    if not checks[1]["ok"]:
        return result(checks)
    logs = [replayfile.Log(f[0]) for f in files]
    # Saved after the disconnect, like a lobby battle: inputs logged past the battle-end frame differ between peers,
    # so the files must end at that frame to be identical.
    key = [(l.inputs, l.start_msg_indexes, l.start_msg_randoms) for l in logs]
    checks.append({"what": "the peers' replays hold the same inputs, round starts and seeds",
                   "inputs": [len(l.inputs) for l in logs],
                   "ok": len(logs[0].inputs) > 0 and all(k == key[0] for k in key)})
    r = play(ctx, {"id": "R3", "disk": "dc2", "log_file_version": "saved"}, pb=files[0][0], name="R3_play")
    want = [int(t) for t in m.peers[0].win_teams]
    checks.append({"what": "peer 1's replay plays to the end with its recorded winners", **r})
    checks.append({"what": "the replay's winners are the match's", "match": want, "replay": r["winners_recorded"],
                   "ok": bool(want) and r["winners_recorded"] == want})
    return result(checks)


@case("R3c", "R3 with gdxsv:SaveReplay=no: no replay saved", control_of="R3")
def replay_save_control(ctx):
    m, files, checks = save_run(ctx, "R3c", save=False)
    return result([checks[0],
                   {"what": "replay check fails", "ok": not checks[1]["ok"], "files": files},
                   {"what": "every peer logged the skip",
                    "ok": all("Skip SaveReplay" in p.log for p in m.peers)}])
