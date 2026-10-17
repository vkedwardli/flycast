"""SH4 fmac as fmul + fadd (GDXSV_FP_COMPAT): a replay gives the same player work RAM on every host."""

import os

from lib import emu, hashlog
from lib.case import Ctx, Skip, case, result
from cases.replay import newest_dc2, play

STEP = 100  # inputs between fixture lines: a float difference spreads, it does not heal
GOLDEN = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "golden")


def fixture_path(entry: dict) -> str:
    return os.path.join(GOLDEN, f"D1_{entry['id']}.hashlog")


def sampled(path: str) -> list:
    return [(fr, h) for fr, h in hashlog.read(path) if fr % STEP == 0]


def hashed_play(ctx: Ctx, entry: dict, name: str) -> tuple:
    """Play the replay with gdxsv:hashlog=yes; write its sampled lines to <out>/<name>.hashlog."""
    c = play(ctx, entry, extra=["gdxsv:hashlog=yes"], name=name)
    data_dir = emu.Instance(os.path.join(ctx.out, name), ctx.exe).data_dir
    rows = sampled(os.path.join(data_dir, "replay_hashlog.txt"))
    with open(os.path.join(ctx.out, name + ".hashlog"), "w", encoding="utf-8") as f:
        f.writelines(f"{fr} {h}\n" for fr, h in rows)
    return c, rows


def compare(rows: list, want: list) -> dict:
    got = dict(rows)
    first = next((fr for fr, h in want if got.get(fr) != h), None)
    return {"lines": len(want), "lines_got": len(rows), "first_diff_input": first,
            "same": bool(want) and first is None and len(rows) == len(want)}


def load_fixture(entry: dict) -> list:
    path = fixture_path(entry)
    if not os.path.isfile(path):
        raise Skip(f"no fixture {path}: copy D1/D1.hashlog of a run on the reference host there")
    return hashlog.read(path)


@case("D1", "newest Disc 2 replay with gdxsv:hashlog: the player work hash every 100 inputs equals the fixture (another host)")
def fp_same_ram(ctx):
    entry = newest_dc2()
    c, rows = hashed_play(ctx, entry, "D1")
    want = load_fixture(entry)
    cmp = compare(rows, want)
    return result([c, {"what": "player work hashes equal the fixture", "ok": cmp["same"], **cmp}])


@case("D1c", "D1 with a GDXSV_FP_COMPAT=OFF build (GDXSV_FP_OFF_EXE): hashes differ from the fixture where the CPU fuses fmac",
      control_of="D1")
def fp_same_ram_control(ctx):
    exe = os.environ.get("GDXSV_FP_OFF_EXE")
    if not exe:
        raise Skip("GDXSV_FP_OFF_EXE unset: a build with -DGDXSV_FP_COMPAT=OFF")
    entry = newest_dc2()
    want = load_fixture(entry)
    c, rows = hashed_play(Ctx(exe, ctx.out), entry, "D1c")
    cmp = compare(rows, want)
    return result([{"what": "fails: player work hashes equal the fixture", "ok": not cmp["same"], **cmp}], detail=c)
