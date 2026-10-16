"""Update check (gdxsv:UpdateCheckUrl): a newer release on the release API is offered.
Judged by the check's log line: the popup itself waits behind any other popup (the wireless warning)."""

import json

from lib import emu, servers
from lib.case import case, result

NEW_TAG = "gdxsv-99.0.0"
OLD_TAG = "gdxsv-0.0.1"
# GdxsvUpdate only offers a release with the zip of this platform.
ZIPS = ["flycast-gdxsv-windows.zip", "flycast-gdxsv-macos-x86_64.zip", "flycast-gdxsv-linux-x86_64.zip"]
WAIT = 20


def release(tag: str) -> bytes:
    assets = [{"name": z, "browser_download_url": f"http://127.0.0.1:1/{z}", "size": 1} for z in ZIPS]
    return json.dumps({"tag_name": tag, "assets": assets}).encode()


def check_update(ctx, name: str, tag: str) -> dict:
    """Start in the main menu (no ROM; the check runs there) with the release API on a local server."""
    with servers.FakeHttp(release(tag)) as http:
        cfg = ["log:Verbosity=3", "log:LogToFile=1", f"gdxsv:UpdateCheckUrl=http://127.0.0.1:{http.port}/latest"]
        inst = emu.launch(ctx.exe, "", f"{ctx.out}/{name}", config=cfg, headless=False)
        try:
            inst.wait_log("update check:", WAIT)
        finally:
            inst.kill()
            inst.proc.wait(10)
            inst.remove_exe()
        requests = list(http.requests)
    line = next((l for l in inst.log.splitlines() if "update check:" in l), "")
    return {"requests": requests, "line": line, "errors": inst.errors[:5]}


@case("U1", f"update check: the release API (local) has {NEW_TAG}, offered as newer with its download")
def update_check(ctx):
    r = check_update(ctx, "U1", NEW_TAG)
    return result([{"what": "asked the release API", "ok": "GET /latest" in r["requests"]},
                   {"what": f"{NEW_TAG} newer, download found",
                    "ok": r["line"].endswith(f"update check: latest {NEW_TAG}, newer 1, download 1")},
                   {"what": "no error in the log", "ok": not r["errors"]}], **r)


@case("U1c", f"U1 with an older release ({OLD_TAG}): not offered", control_of="U1")
def update_check_control(ctx):
    r = check_update(ctx, "U1c", OLD_TAG)
    return result([{"what": "asked the release API", "ok": "GET /latest" in r["requests"]},
                   {"what": "the check ran", "ok": "update check: latest " in r["line"]},
                   {"what": "the newer check fails", "ok": "newer 1" not in r["line"]}], **r)
