"""Main-menu features (no ROM, a window): update check, texture pack download, wireless and VPN warnings.
Judged by log lines: a popup waits behind any other open popup, so it may never be drawn."""

import json
import os

from lib import emu, servers
from lib.case import case, result
from lib.xxh64 import hexdigest

WAIT = 30
# The wireless warning would hold back the update popup and the texture download (they need no popup open).
WIRED = "gdxsv:ConnectionMedium=Wired"


def main_menu(ctx, name: str, config: list, until: str, routes=None, clean: bool = True) -> dict:
    """Start in the main menu with a local HTTP server (routes: path -> body); stop once `until`
    is in the log, or after WAIT s."""
    with servers.FakeHttp(routes=routes or {}) as http:
        url = f"http://127.0.0.1:{http.port}"
        cfg = ["log:Verbosity=3", "log:LogToFile=1"] + [c.format(url=url) for c in config]
        inst = emu.launch(ctx.exe, "", os.path.join(ctx.out, name), config=cfg, headless=False, clean=clean)
        try:
            inst.wait_log(until, WAIT)
        finally:
            inst.kill()
            inst.proc.wait(10)
            inst.remove_exe()
        requests = list(http.requests)
    return {"inst": inst, "requests": requests, "log": inst.log}


def lines(log: str, text: str) -> list:
    return [l.split(": ", 1)[-1] for l in log.splitlines() if text in l]


# Update check (gdxsv:UpdateCheckUrl)

NEW_TAG = "gdxsv-99.0.0"
OLD_TAG = "gdxsv-0.0.1"
# GdxsvUpdate only offers a release with the zip of this platform.
ZIPS = ["flycast-gdxsv-windows.zip", "flycast-gdxsv-macos-x86_64.zip", "flycast-gdxsv-linux-x86_64.zip"]


def check_update(ctx, name: str, tag: str) -> dict:
    assets = [{"name": z, "browser_download_url": f"http://127.0.0.1:1/{z}", "size": 1} for z in ZIPS]
    r = main_menu(ctx, name, [WIRED, "gdxsv:UpdateCheckUrl={url}/latest"], "update check:",
                  {"/latest": json.dumps({"tag_name": tag, "assets": assets}).encode()})
    return {"requests": r["requests"], "line": "".join(lines(r["log"], "update check:")[:1]),
            "errors": r["inst"].errors[:5]}


@case("U1", f"update check: the release API (local) has {NEW_TAG}, offered as newer with its download")
def update_check(ctx):
    r = check_update(ctx, "U1", NEW_TAG)
    return result([{"what": "asked the release API", "ok": "GET /latest" in r["requests"]},
                   {"what": f"{NEW_TAG} newer, download found",
                    "ok": r["line"] == f"update check: latest {NEW_TAG}, newer 1, download 1"},
                   {"what": "no error in the log", "ok": not r["errors"]}], **r)


@case("U1c", f"U1 with an older release ({OLD_TAG}): not offered", control_of="U1")
def update_check_control(ctx):
    r = check_update(ctx, "U1c", OLD_TAG)
    return result([{"what": "asked the release API", "ok": "GET /latest" in r["requests"]},
                   {"what": "the check ran", "ok": r["line"].startswith("update check: latest ")},
                   {"what": "the newer check fails", "ok": "newer 1" not in r["line"]}], **r)


# Texture pack (UseTexturePack, TexturePackChannel, gdxsv:TexturePackUrl)

CHANNEL = "e2e"
PACK_VERSION = "e2e-1"
PACK = bytes((i * 7 + i // 251) & 0xff for i in range(100_000))  # stands in for the pack: only hashes are checked
CHUNKS = 3


def pack_routes(bad_chunk: bool = False) -> dict:
    step = -(-len(PACK) // CHUNKS)
    parts = [PACK[i:i + step] for i in range(0, len(PACK), step)]
    chunks = [{"path": f"c{i}", "size": len(p), "hash": hexdigest(p)} for i, p in enumerate(parts)]
    if bad_chunk:
        chunks[1]["hash"] = "0" * 16
    info = {"name": "T13306M.texp", "version": PACK_VERSION, "xxhash": hexdigest(PACK), "size": len(PACK),
            "chunks": chunks}
    routes = {f"/{CHANNEL}.json": json.dumps(info).encode()}
    routes.update({f"/c{i}": p for i, p in enumerate(parts)})
    return routes


def texture_runs(ctx, name: str, bad_chunk: bool) -> dict:
    """Run 1 from an empty data dir: download. Run 2, same dir: the pack is current, nothing fetched."""
    cfg = [WIRED, "gdxsv:UseTexturePack=yes", f"gdxsv:TexturePackChannel={CHANNEL}", "gdxsv:TexturePackUrl={url}/"]
    routes = pack_routes(bad_chunk)
    first = main_menu(ctx, name, cfg, "texture pack updated" if not bad_chunk else "chunk hash missmatch", routes)
    path = os.path.join(first["inst"].data_dir, "T13306M.texp")
    try:
        with open(path, "rb") as f:
            got = f.read()
    except OSError:
        got = None
    os.remove(first["inst"].log_path)  # else run 2 matches run 1's lines before flycast rewrites the file
    second = main_menu(ctx, name, cfg, "texture pack check:", routes, clean=False)
    return {"first": {"requests": first["requests"], "check": lines(first["log"], "texture pack"),
                      "errors": first["inst"].errors[:5]},
            "pack_bytes": None if got is None else len(got), "pack_equal": got == PACK,
            "second": {"requests": second["requests"], "check": lines(second["log"], "texture pack")}}


def texture_checks(r: dict) -> list:
    return [{"what": f"channel {CHANNEL}: asked /{CHANNEL}.json", "ok": any(f"/{CHANNEL}.json" in q for q in r["first"]["requests"])},
            {"what": f"downloaded {CHUNKS} chunks, updated to {PACK_VERSION}",
             "ok": sum(q.startswith("GET /c") for q in r["first"]["requests"]) == CHUNKS
             and f"texture pack updated to {PACK_VERSION}" in r["first"]["check"]},
            {"what": "T13306M.texp in the data dir equals the served pack", "ok": r["pack_equal"]},
            {"what": "second start: pack current (new 0), no chunk fetched",
             "ok": f"texture pack check: {CHANNEL}.json, new 0" in r["second"]["check"]
             and not any(q.startswith("GET /c") for q in r["second"]["requests"])}]


@case("X1", f"texture pack: channel {CHANNEL} on a local server, {CHUNKS} chunks downloaded, checked and installed; "
            "the next start finds it current")
def texture_pack(ctx):
    r = texture_runs(ctx, "X1", False)
    return result(texture_checks(r) + [{"what": "no error in the log", "ok": not r["first"]["errors"]}], **r)


@case("X1c", "X1 with one chunk's hash wrong: the pack is not installed", control_of="X1")
def texture_pack_control(ctx):
    r = texture_runs(ctx, "X1c", True)
    c = texture_checks(r)
    return result([{"what": "asked the channel", "ok": c[0]["ok"]},
                   {"what": "not installed (update and file checks fail)", "ok": not c[1]["ok"] and not c[2]["ok"]}],
                  detail=c, **r)


# Wireless warning and VPN toast (gdxsv:ConnectionMedium stands in for the detected adapter)

def warning(ctx, name: str, medium: str) -> dict:
    r = main_menu(ctx, name, [f"gdxsv:ConnectionMedium={medium}"], "warning shown")
    return {"lines": lines(r["log"], "warning shown")}


@case("M1", "connection warnings: Wireless opens the LAN cable popup, 'VPN: e2e' the VPN toast with the adapter")
def connection_warnings(ctx):
    w = warning(ctx, "M1w", "Wireless")
    v = warning(ctx, "M1v", "VPN: e2e")
    return result([{"what": "wireless warning", "ok": w["lines"] == ["wireless warning shown"]},
                   {"what": "VPN toast", "ok": v["lines"] == ["VPN warning shown: VPN: e2e"]}], wireless=w, vpn=v)


@case("M1c", "M1 with Wired: neither warning", control_of="M1")
def connection_warnings_control(ctx):
    r = main_menu(ctx, "M1c", [WIRED], "version check failure")  # the update check (OFFLINE: refused) ends after frame 1
    found = lines(r["log"], "warning shown")
    return result([{"what": "no warning", "ok": not found}], lines=found)
