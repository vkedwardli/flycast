"""Online features against a local gdxsv lobby (lib/servers.py)."""

import os

from lib import assets, servers
from lib.case import Skip, case, result

LOGIN_KEY = "E2ETEST1"
USER = ("E2E001", "e2e")  # user id, handle name the lobby lists for LOGIN_KEY
BOOT = 300  # vblanks of cold boot before the network menu savestate is loaded
LOGIN_FRAMES = 4400  # boot, the menu path in lua/login.lua, and the lobby's plaza messages


def login(ctx, tag: str, client_key: str):
    """Disc 2 logs in from the network menu savestate; returns (probe, lobby log, fake HTTP requests)."""
    rom = ctx.rom("dc2")
    exe = servers.server_exe()
    if exe is None:
        raise Skip("no gdxsv server build (GDXSV_SERVER_EXE)")
    states = assets.login_state_dir()
    if states is None:
        raise Skip("no network menu savestate (GDXSV_LOGIN_STATE)")
    lobby = servers.Lobby(exe, os.path.join(ctx.out, tag + "_lbs"))
    lobby.init([(servers.server_login_key(LOGIN_KEY),) + USER])
    with servers.FakeHttp() as http:
        cfg = http.config() + [f"gdxsv:loginkey={client_key}", "config:rend.ThreadedRendering=no"]
        lobby.start()
        try:
            p = ctx.probe(tag, rom, "login.lua", {"boot": BOOT, "slot": assets.LOGIN_SLOT},
                          frames=LOGIN_FRAMES, config=cfg, loadstate=False, states=states, timeout=600)
        finally:
            lobby.stop()
        return p, lobby.log(), list(http.requests)


def checks(p, lbs: str, requests: list) -> list:
    lines = lbs.splitlines()
    return [
        {"what": "probe ran", "ok": p.clean, **p.facts()},
        {"what": "lobby got the platform info",
         "ok": "Command: lbsPlatformInfo, Direction: ClientToServer" in lbs and '"cpu": "x86/64"' in lbs},
        # The account is found by the hash of the login key: the seeded user is listed and chosen.
        {"what": "login key's user logged in",
         "ok": any("login ok" in l and f'"user_id": "{USER[0]}"' in l for l in lines)},
        {"what": "plaza joined", "ok": "Command: lbsPlazaJoin, Direction: ClientToServer" in lbs},
        {"what": "public IP asked of the local server", "ok": "GET /ip" in requests},
        {"what": "P2P test asked of the local server", "ok": "POST /udptest" in requests},
    ]


@case("O1", "lobby connection, login key, platform info, plaza (local gdxsv lbs)")
def online_login(ctx):
    p, lbs, reqs = login(ctx, "O1", LOGIN_KEY)
    return result(checks(p, lbs, reqs), requests=sorted(set(reqs)))


@case("O1c", "O1 with a login key the lobby does not know: no user to log in", control_of="O1")
def online_login_control(ctx):
    p, lbs, reqs = login(ctx, "O1c", "E2ETEST2")
    c = checks(p, lbs, reqs)
    return result([{"what": "probe ran", "ok": c[0]["ok"]},
                   {"what": "platform info still sent", "ok": c[1]["ok"]},
                   {"what": "login check fails", "ok": not c[2]["ok"]}],
                  detail=c)
