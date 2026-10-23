"""Replay browser (Live & Replays: Live, Local, Server tabs) in a window, judged by cropped golden
images. Opened as a player would: Tab for the pause menu, then mouse clicks."""

import json
import os
import shutil
import time

from lib import assets, emu, image, recheck, servers
from lib.case import Skip, case, result
from cases.replay import newest_dc2

SIZE = (640, 480)
# Client coordinates in a 640x480 window at 100 % display scale.
LIVE_AND_REPLAYS = (190, 198)  # pause menu button
TAB = {"live": (50, 93), "local": (145, 93), "server": (245, 93)}
# (grab, box, golden): the tab bar, and the players line of each tab's first row (the dates
# are left out: they are shown in local time).
CHECKS = {
    "tabs": ("live", (8, 82, 296, 106), "browser_tabs.png"),
    "live": ("live", (16, 195, 360, 213), "browser_live.png"),
    "local": ("local", (16, 193, 420, 212), "browser_local.png"),
    "server": ("server", (16, 203, 360, 221), "browser_server.png"),
}
# Measured: 1.00 with the rows (two runs), 0.00 without them.
MATCH = 0.9

USERS = range(1, 5)
STATUS = {"active_games": [{"battle_code": "e2e0001", "disk": "dc2", "state": "in_battle", "lobby_id": 2,
                            "live_spectate": True, "spectators": 3, "updated_at": "2026-10-17T00:00:00Z"}],
          "battle_users": [{"battle_code": "e2e0001", "user_id": f"E2EU0{i}", "name": f"E2E-USER{i}",
                            "pilot_name": f"PILOT{i}", "battle_pos": i, "team": "renpo" if i < 3 else "zeon"}
                           for i in USERS]}
REPLAYS = [{"disk": "dc2", "users": [{"user_id": f"E2EU0{i}", "user_name": f"E2E-USER{i}", "pilot_name": f"PILOT{i}",
                                      "team": 1 if i < 3 else 2, "pos": i} for i in USERS],
            "round": 3, "renpo_win": 2, "zeon_win": 1, "start_unix": 1790000000,
            "replay_url": "http://127.0.0.1:1/e2e.pb", "round_win": "121", "play_count": 7}]


def wait_request(http, prefix: str, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if any(r.startswith(prefix) for r in http.requests):
            return True
        time.sleep(0.2)
    return False


def browse(ctx, name: str, rows: bool) -> dict:
    """Open the browser on Disc 2 and grab each tab into out/<name>/<tab>.png. rows: the local
    fakes list one live battle and one server replay, and data/replays holds a corpus replay."""
    d = os.path.join(ctx.out, name)
    routes = {"/status": json.dumps(STATUS if rows else {}).encode(),
              "/replay": json.dumps(REPLAYS if rows else []).encode()}
    steps = {}
    with image.SCREEN_LOCK, servers.FakeHttp(routes=routes) as http:
        url = f"http://127.0.0.1:{http.port}"
        cfg = ["log:Verbosity=3", "log:LogToFile=1", "window:width=640", "window:height=480",
               "gdxsv:ConnectionMedium=Wired", f"gdxsv:LiveApiUrl={url}", f"gdxsv:ReplayApiUrl={url}",
               *http.config()]  # the online boot's public IP, P2P and latency checks
        inst = emu.launch(ctx.exe, ctx.rom("dc2"), os.path.join(d, "emu"), config=cfg, headless=False)
        if not recheck.ACTIVE:
            try:
                if rows:
                    os.makedirs(os.path.join(inst.data_dir, "replays"), exist_ok=True)
                    shutil.copy(assets.replay(newest_dc2()), os.path.join(inst.data_dir, "replays"))
                time.sleep(10)  # boot to the game's first screens
                pid = inst.proc.pid
                emu.click(pid, 320, 400)  # the game area: keyboard focus
                time.sleep(0.5)
                emu.press_key(0x09)  # Tab: pause menu
                time.sleep(1.5)
                emu.click(pid, *LIVE_AND_REPLAYS)
                steps["live fetched"] = wait_request(http, "GET /status", 10)
                time.sleep(2)
                image.grab(pid, os.path.join(d, "live.png"))
                emu.click(pid, *TAB["local"])
                time.sleep(2)
                image.grab(pid, os.path.join(d, "local.png"))
                emu.click(pid, *TAB["server"])
                steps["server fetched"] = wait_request(http, "GET /replay", 10)
                time.sleep(2)
                image.grab(pid, os.path.join(d, "server.png"))
            except NotImplementedError as e:
                raise Skip(str(e))
            finally:
                inst.kill()
                inst.proc.wait(10)
                inst.remove_exe()
            with open(os.path.join(d, "steps.json"), "w") as f:
                json.dump(steps, f)
        else:
            steps = recheck.load(os.path.join(d, "steps.json"), {})
    scores = {}
    for key, (grab, box, golden) in CHECKS.items():
        p = os.path.join(d, grab + ".png")
        scores[key] = round(image.recall(golden, image.white_mask(p, box, SIZE)), 2) if os.path.isfile(p) else None
    return {"steps": steps, "scores": scores}


def matched(r: dict, key: str) -> bool:
    return (r["scores"][key] or 0) >= MATCH


@case("G2", "replay browser: the Live, Local and Server tabs list a fake live battle, a local and a server replay")
def browser(ctx):
    r = browse(ctx, "G2", rows=True)
    return result([{"what": "the browser fetched the live list and the server replays", **r["steps"],
                    "ok": bool(r["steps"]) and all(r["steps"].values())}] +
                  [{"what": f"{key} matches golden/{CHECKS[key][2]}", "recall": r["scores"][key],
                    "ok": matched(r, key)} for key in CHECKS])
