"""A recorded battle played as a 4-player local rollback match.

    python gdxsv_tests/rollback/replay_match.py --rom D:\\rom\\gdx-disc2\\gdx-disc2.gdi
    python gdxsv_tests/rollback/replay_match.py --rom ... --corpus 1769098243820 --delays 30,50,60,80 --window
    python gdxsv_tests/rollback/replay_match.py --rom ... --replay <battle>.pb

Without --replay it plays a corpus replay (gdxsv_tests/corpus/replays.json, downloaded to out/replay), the newest
Disc 2 one by default. Only replays with inputs and round starts work (corpus 1727284586532 and newer).

The peers run as in run_suite.py (gdxsv:rbk_test) with gdxsv:rbk_replay=<replay>: each gives GGPO its player's
recorded input, and the game reads the recorded inputs by KeyMsg1 number as replay playback does, so the battle
is the recorded one, with real rollbacks and timesync over the GGPO session. Users, rule and patches come from
the replay.

Reported per peer: whether it finished, each round's start RNG against the recording (`RBKREPLAY round`), the
round winners against the recording, and how many local inputs GGPO carried that differ from the recording.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import rbk_lib as lib  # noqa: E402
from lib import assets  # noqa: E402
from lib.replayfile import Log as load_log, win_teams  # noqa: E402

DEFAULT_CORPUS = "1788627530784"


def round_starts(log: str):
    """{round: (rng, recorded, verdict)} from the last RBKREPLAY line of each round (a rollback can re-log it)."""
    out = {}
    for m in re.finditer(r"RBKREPLAY round (\d+) start frame=\d+ key_msg=\d+ rng=(\w+) recorded=(\w+) (\w+)", log):
        out[int(m.group(1))] = (m.group(2), m.group(3), m.group(4))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True)
    ap.add_argument("--replay", help="replay file (BattleLogFile .pb) of a rollback battle")
    ap.add_argument("--corpus", default=DEFAULT_CORPUS, help="id of a corpus replay, when --replay is not given")
    ap.add_argument("--exe", default=os.path.join(lib.REPO, "cmake-build-relwithdebinfo", lib.EXE_NAME))
    ap.add_argument("--state-dir", default=os.path.join(lib.REPO, "work", "state"))
    ap.add_argument("--out", default=os.path.join(lib.HERE, "out"))
    ap.add_argument("--tag", default="replay")
    ap.add_argument("--delays", default="16,16,16,16", help="GGPO_NETWORK_DELAY per peer, ms")
    ap.add_argument("--input-delay", default="", help="TEST_GGPO_DELAY: fixed input delay frames, so latency means prediction")
    ap.add_argument("--window", action="store_true", help="open the four windows instead of running headless")
    ap.add_argument("--config", action="append", default=[], help="more section:key=value for every peer")
    ap.add_argument("--timeout", type=int, help="seconds (default: the recording's length plus 5 minutes)")
    a = ap.parse_args()

    entry = None
    if a.replay:
        replay = os.path.abspath(a.replay)
    else:
        entry = next((e for e in assets.corpus() if e["id"] == a.corpus), None)
        if entry is None:
            ap.error(f"no corpus replay {a.corpus}")
        replay = assets.replay(entry)
    rec = load_log(replay)
    # Older replays have no round_data; the corpus lists their winners.
    rec_wins = win_teams(replay) or (entry or {}).get("winners", [])
    print(f"replay: {len(rec.inputs)} inputs, round starts {rec.start_msg_indexes}, winners {rec_wins}")

    s = lib.Settings(exe=os.path.abspath(a.exe), rom=os.path.abspath(a.rom), out=os.path.abspath(a.out),
                     state_dir=os.path.abspath(a.state_dir))
    lib.ensure_state(s.state_dir, s.rom)
    env = {"RBK_SAVE_REPLAY": "1"}
    if a.input_delay:
        env["TEST_GGPO_DELAY"] = a.input_delay
    timeout = a.timeout or int(len(rec.inputs) / 59.94) + 300
    m = lib.run_match(s, a.tag, [s.exe] * lib.N, [int(x) for x in a.delays.split(",")], env=env,
                      headless=not a.window, config=[f"gdxsv:rbk_replay={replay}"] + a.config, timeout=timeout)
    print(f"elapsed {round(m.elapsed)} s, timed_out {m.timed_out}, stalled {m.stalled}")

    ok = not m.timed_out and not m.stalled
    for p in m.peers:
        log = p.log
        starts = round_starts(log)
        wins = [int(w) for w in p.win_teams]
        mism = re.findall(r"RBKREPLAY local inputs (\d+), differing from the recording (\d+)", log)
        desync = [r for r, v in starts.items() if v[2] != "match"]
        same_wins = not rec_wins or wins == rec_wins
        ok &= p.finished and not p.errors and not desync and same_wins
        print(f"  p{p.index}: finished={p.finished} exit={p.exit_code} rounds={len(starts)} desync rounds={desync} "
              f"winners={wins}{'' if same_wins else ' (recording ' + str(rec_wins) + ')'} "
              f"local inputs/differing={'/'.join(mism[-1]) if mism else '-'} rolled back frames={p.rolled_back_frames} "
              f"errors={p.errors[:3]}")
    print("output:", os.path.join(s.out, a.tag))
    print("RESULT", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
