#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 gdxsv contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build and run the native Windows four-way HTTPS latency comparison."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True)


def resolve_baseline(revision):
    # Avoid caret/brace revision syntax when invoking MSYS Git from native Python.
    object_id = git("rev-parse", "--verify", "--end-of-options", revision).strip()
    commit = git("rev-list", "--no-walk", "--max-count=1", object_id, "--").strip()
    if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", commit):
        raise ValueError("--baseline must identify a commit or a tag pointing to one")
    return commit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, help="Commit whose unmodified Windows helper is the baseline")
    parser.add_argument("--compiler", default="g++", help="MinGW C++ compiler")
    parser.add_argument("--output", type=Path, default=ROOT / "build/windows-https-latency")
    parser.add_argument("--build-only", action="store_true")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    baseline = resolve_baseline(args.baseline)
    commit = git("rev-parse", "HEAD").strip()
    if git("status", "--porcelain", "--", "core/gdxsv", "gdxsv_tests/rollback/https_latency_probe_win.cpp", "gdxsv_tests/rollback/windows_https_latency.py").strip():
        commit += "-dirty"
    source = "core/gdxsv/gdxsv_https_latency_win.cpp"
    baseline_source = git("show", baseline + ":" + source)
    (out / "baseline_win.cpp").write_text(baseline_source, encoding="utf-8")
    (out / "version.h").write_text('#define GIT_VERSION "vlatency-diagnostic"\n', encoding="utf-8")
    network = (ROOT / "core/gdxsv/gdxsv_network.cpp").read_text(encoding="utf-8")
    block = network.split("gcp_regions[] = {", 1)[1].split("};", 1)[0]
    regions = []
    for line in block.splitlines():
        if line.lstrip().startswith("//"):
            continue
        match = re.search(r'\{"([a-z0-9-]+)", "([^"]+)"\}', line)
        if match:
            regions.append(match.groups())
    if not regions or len({region for region, _ in regions}) != len(regions):
        raise RuntimeError("could not extract unique enabled cloud regions")
    header = "constexpr GdxsvProbeRegion gdxsv_probe_regions[] = {\n"
    header += "".join("    {" + json.dumps(region) + ", " + json.dumps(location) + "},\n" for region, location in regions)
    header += "};\nconstexpr char gdxsv_probe_commit[] = " + json.dumps(commit) + ";\n"
    header += "constexpr char gdxsv_probe_baseline[] = " + json.dumps(baseline) + ";\n"
    (out / "latency_probe_regions.h").write_text(header, encoding="utf-8")
    common = [args.compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-D_WIN32_WINNT=0x0601"]
    common += ["-I" + str(ROOT / path) for path in ["core", "core/gdxsv", "core/deps/nowide/include"]]
    common += ["-I" + str(out)]
    subprocess.run(common + ["-DmeasureGdxsvHttpsLatency=gdxsv_measure_https_latency_baseline", "-c",
                            str(out / "baseline_win.cpp"), "-o", str(out / "baseline.o")], check=True)
    executable = out / "windows-https-latency.exe"
    subprocess.run(common + [str(ROOT / source), str(ROOT / "core/gdxsv/gdxsv_https_latency_trace_win.cpp"),
                            str(ROOT / "gdxsv_tests/rollback/https_latency_probe_win.cpp"), str(out / "baseline.o"),
                            "-static", "-lwininet", "-lws2_32", "-o", str(executable)], check=True)
    (out / "run-latency.cmd").write_bytes(b'@echo off\r\ncd /d "%~dp0"\r\nwindows-https-latency.exe > latency.log 2>&1\r\nset "result=%errorlevel%"\r\ntype latency.log\r\npause\r\nexit /b %result%\r\n')
    print(f"Built {executable}; commit={commit}; baseline={baseline}; regions={len(regions)}", flush=True)
    if args.build_only:
        return 0
    if sys.platform != "win32":
        raise RuntimeError("Run the probe on native Windows; cross-compilation supports --build-only")
    log = out / "latency.log"
    with log.open("w", encoding="utf-8") as stream:
        try:
            result = subprocess.run([str(executable)], stdout=stream, stderr=subprocess.STDOUT, timeout=900)
            code = result.returncode
        except subprocess.TimeoutExpired:
            stream.write("\nFAIL: overall 900-second probe deadline exceeded\n")
            code = 1
    text = log.read_text(encoding="utf-8", errors="replace")
    print(text)
    summary = text[text.rfind("\nSUMMARY "):] if "\nSUMMARY " in text else "Probe failed before producing a complete table. See latency.log.\n"
    (out / "summary.md").write_text(summary, encoding="utf-8")
    return code


if __name__ == "__main__":
    sys.exit(main())
