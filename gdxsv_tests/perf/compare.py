#!/usr/bin/env python3
"""Judge perf battle cells against a baseline with the pre-release tolerances; print a markdown table.

usage: compare.py baseline.json result.json   (exit 1 if a cell is outside tolerance)
"""
import json
import sys

FRAME_MS = 16.683
P99_MIN_N = 100  # a p99 is judged only on a bin with this many samples


def get(d, path):
    for k in path.split("."):
        if not isinstance(d, dict) or k not in d:
            return None
        d = d[k]
    return d


def checks(cell: str, base: dict):
    """(metric path, kind, limit): kind "add" = new <= base + limit, "rel" = new <= base * (1 + limit),
    "range" = |new - FRAME_MS| <= limit, "min" = new >= limit, "eq" = new == limit."""
    out = [("D1.failures", "eq", 0)]
    for m in ("A1", "A2"):
        out += [(f"{m}.avg", "add", 2.0), (f"{m}.p95", "add", 2.0)]
    out += [("B1.over_20ms_pct", "add", 0.5)]
    if cell.endswith("vs1"):
        out += [("B1.not_one_vblank_pct", "add", 0.5)]
    for m in ("input_read", "add"):
        out += [(f"B2.{m}.sd", "add", 0.0), (f"B2.{m}.p99", "add", 0.0)]
    if cell.startswith("20_"):
        out += [("B3.med", "range", 0.05), ("B3.within_1ms_pct", "min", 99.0), ("B3.sd", "add", 0.0)]
    else:
        out += [("B3.sd", "add", 0.0), ("B3.p99", "add", 0.0), ("B3.max", "add", 0.0)]
    for d in ("1", "2", "3", "4", "5"):
        out += [(f"C1.{d}.avg", "rel", 0.10)]
        if (get(base, f"C1.{d}.n") or 0) >= P99_MIN_N:
            out += [(f"C1.{d}.p99", "rel", 0.10)]
    out += [("C1.5_derived.avg", "rel", 0.10)]
    for m in ("emulation", "save", "load", "rerun", "render_present", "in_present"):
        out += [(f"C3.{m}.avg", "rel", 0.10), (f"C3.{m}.p99", "rel", 0.10)]
    return out


def judge(cell: str, base: dict, new: dict) -> list:
    """Failures as "metric: new vs base" strings; a metric missing on either side is not judged, except D1."""
    fails = []
    for path, kind, lim in checks(cell, base):
        b, n = get(base, path), get(new, path)
        if n is None:
            if kind == "eq":
                fails.append(f"{path}: missing")
            continue
        if kind == "eq":
            bad = n != lim
        elif kind == "range":
            bad = abs(n - FRAME_MS) > lim
        elif kind == "min":
            bad = n < lim
        elif b is None:
            continue
        elif kind == "add":
            bad = n > b + lim
        else:
            bad = n > b * (1 + lim)
        if bad:
            fails.append(f"{path}: {n} vs {b}")
    return fails


def spread(runs: list) -> dict:
    """max - min over runs for every judged leaf, to set tolerances against run-to-run noise."""
    out = {}
    for path, kind, lim in checks("20_vs1", runs[0]) + checks("30_vs1", runs[0]):
        v = [get(r, path) for r in runs]
        v = [x for x in v if isinstance(x, (int, float))]
        if len(v) > 1:
            out[path] = round(max(v) - min(v), 4)
    return out


ROWS = ["A1.avg", "A1.p95", "A2.avg", "A2.p95", "B1.over_20ms_pct", "B1.not_one_vblank_pct", "B2.input_read.sd",
        "B2.input_read.p99", "B3.med", "B3.sd", "B3.p99", "B3.within_1ms_pct", "C1.1.avg", "C1.2.avg", "C1.3.avg",
        "C1.3.p99", "C1.4.avg", "C1.5.avg", "C1.5_derived.avg", "C3.emulation.avg", "C3.save.avg", "C3.load.avg",
        "C3.rerun.avg", "C3.render_present.avg", "C3.in_present.avg", "D1.failures",
        "info.rolled_back_frames_per_min", "info.depth_hist", "info.barrier_hits"]


def fmt(v):
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:.2f}"
    if isinstance(v, dict):
        return " ".join(f"{k}:{x}" for k, x in v.items())
    return str(v)


def table(base, result: dict) -> str:
    cells = list(result)
    lines = ["| metric | " + " | ".join(cells) + " |", "|---|" + "---|" * len(cells)]
    for r in ROWS:
        row = []
        for c in cells:
            n = fmt(get(result[c], r))
            b = get(base.get(c, {}), r) if base else None
            row.append(n if b is None or isinstance(b, dict) else f"{n} ({fmt(b)})")
        lines.append(f"| {r} | " + " | ".join(row) + " |")
    if base:
        lines.append("\nIn parentheses: the baseline.")
    return "\n".join(lines)


def main() -> int:
    base, new = (json.load(open(p)) for p in sys.argv[1:3])
    bad = 0
    for c in new:
        if c in base:
            f = judge(c, base[c], new[c])
            bad += bool(f)
            print(f"{c}: {'FAIL ' + '; '.join(f) if f else 'ok'}")
    print(table(base, new))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
