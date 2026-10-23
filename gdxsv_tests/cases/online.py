"""Online features against a local gdxsv lobby (lib/servers.py)."""

import json
import os
import re
import threading

from lib import assets, servers
from lib.case import Skip, case, result

LOGIN_KEY = "E2ETEST1"
USER = ("E2E001", "e2e")  # user id, handle name the lobby lists for LOGIN_KEY
LOGIN_KEY2 = "E2ETEST3"  # the second client of the two-player cases
USER2 = ("E2E002", "e2e2")
BOOT = 300  # vblanks of cold boot before the network menu savestate is loaded
LOGIN_FRAMES = 4400  # boot, the menu path in lua/login.lua, and the lobby's plaza messages
BATTLE_FRAMES = 40000  # upper bound; the script ends 300 vblanks into the battle


def login(ctx, tag: str, client_key: str, battle: bool = False, mcs_region: str = "", udp_hello: bool = False):
    """Disc 2 logs in from the network menu savestate; returns (probe, lobby log, fake HTTP requests)."""
    probes, lbs, reqs = session(ctx, tag, [(tag, client_key, {"battle": battle})], mcs_region=mcs_region,
                                udp_hello=udp_hello)
    return probes[0], lbs, reqs


def session(ctx, tag: str, clients: list, mcs_region: str = "", udp_hello: bool = False):
    """clients = (name, gdxsv:loginkey, lua/login.lua params) run at once against one lobby;
    returns ([probe per client], lobby log, fake HTTP requests).
    udp_hello: the fake P2P test answers over UDP (lib/servers.FakeHttp)."""
    rom = ctx.rom("dc2")
    exe = servers.server_exe()
    if exe is None:
        raise Skip("no gdxsv server build (GDXSV_SERVER_EXE)")
    states = assets.login_state_dir()
    if states is None:
        raise Skip("no network menu savestate (GDXSV_LOGIN_STATE)")
    lobby = servers.Lobby(exe, os.path.join(ctx.out, tag + "_lbs"))
    lobby.init([(servers.server_login_key(LOGIN_KEY),) + USER, (servers.server_login_key(LOGIN_KEY2),) + USER2])
    servers.add_training_lobby(lobby.db, mcs_region=mcs_region)
    probes = [None] * len(clients)
    with servers.FakeHttp(body=b"127.0.0.1", udp_hello=udp_hello) as http:
        def run(i, name, key, params):
            battle = params.get("battle", False)
            cfg = http.config() + ["gdxsv:server=127.0.0.1", f"gdxsv:loginkey={key}",
                                   "config:rend.ThreadedRendering=no"]
            probes[i] = ctx.probe(name, rom, "login.lua", {"boot": BOOT, "slot": assets.LOGIN_SLOT, **params},
                                  frames=BATTLE_FRAMES if battle else LOGIN_FRAMES, config=cfg, loadstate=False,
                                  states=states, timeout=900, until_done=battle)
        lobby.start()
        try:
            threads = [threading.Thread(target=run, args=(i,) + c) for i, c in enumerate(clients)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
        finally:
            lobby.stop()
        return probes, lobby.log(), list(http.requests)


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


@case("O1", "lobby connection, login key, platform info, plaza, P2P port test (local gdxsv lbs)")
def online_login(ctx):
    p, lbs, reqs = login(ctx, "O1", LOGIN_KEY, udp_hello=True)
    return result(checks(p, lbs, reqs) + net_checks(p, lbs), requests=sorted(set(reqs)))


@case("O6", "P2P port test failure: a test server that sends no UDP Hello is reported as Failed (Timeout)")
def online_port_test_failed(ctx):
    p, lbs, reqs = login(ctx, "O6", LOGIN_KEY)
    c = checks(p, lbs, reqs) + net_checks(p, lbs)
    return result([{"what": "probe ran", "ok": c[0]["ok"]},
                   {"what": "logged in", "ok": c[2]["ok"]},
                   {"what": "P2P test still asked", "ok": c[5]["ok"]},
                   {"what": "port test check fails", "ok": not c[-1]["ok"]},
                   {"what": "port_test_v4 = Failed (Timeout)", "ok": c[-1]["got"] == "Failed (Timeout)"}],
                  detail=c)


def platform_info(lbs: str) -> dict:
    """The platform info the lobby logged with the login (LoginUser line)."""
    for l in lbs.splitlines():
        m = re.search(r'"platform_info": (\{.*?\})', l)
        if "LoginUser" in l and m:
            return json.loads(m.group(1))
    return {}


def net_checks(p, lbs: str) -> list:
    """Platform info fields against what this run measured or served."""
    info = platform_info(lbs)
    patch = PATCH_RE.findall(p.log)
    return [
        {"what": "platform info: payload patch_id", "got": info.get("patch_id"), "want": patch[-1:],
         "ok": bool(patch) and info.get("patch_id") == patch[-1]},
        {"what": "platform info: public IP from the local server", "got": info.get("public_ipv4"),
         "ok": info.get("public_ipv4") == "127.0.0.1"},
        {"what": "P2P port test: UDP Hello received", "got": info.get("port_test_v4"),
         "ok": info.get("port_test_v4") == "Success"},
    ]

PATCH_RE = re.compile(r"N\[COMMON\]: patch -?\d+ (\d+)$", re.M)


def payload_checks(p, lbs: str, requests: list) -> list:
    """The network payload (gdxsv_patch/) is installed after the state load and is what the login runs on."""
    ids = PATCH_RE.findall(p.log)
    want = ids[-1] if ids else None
    return [
        {"what": "payload installed (log: patch <in RAM> <current id>)", "ok": want is not None, "ids": ids},
        {"what": "current patch_id in RAM at the network menu", "got": p.e2e.get("patch_id_menu"), "want": want,
         "ok": want is not None and p.e2e.get("patch_id_menu") == want},
        {"what": "lobby connection not refused", "ok": "Lobby connection refused" not in p.log},
    ] + checks(p, lbs, requests)


@case("O5", "network patch: the SH4 payload is installed and the login runs through it (local gdxsv lbs)")
def online_payload(ctx):
    p, lbs, reqs = login(ctx, "O5", LOGIN_KEY)
    return result(payload_checks(p, lbs, reqs))


@case("O2", "lobby to battle: training lobby entry, force start, mobile suit select, battle (local gdxsv)")
def online_battle(ctx):
    p, lbs, _ = login(ctx, "O2", LOGIN_KEY, battle=True)
    return result(battle_checks(p, lbs, rollback=False))


@case("O3", "lobby to a rollback battle: P2P lobby, one player (local gdxsv)")
def online_rollback(ctx):
    p, lbs, _ = login(ctx, "O3", LOGIN_KEY, battle=True, mcs_region="p2p")
    return result(battle_checks(p, lbs, rollback=True))


def battle_checks(p, lbs: str, rollback: bool) -> list:
    log = p.log
    c = [{"what": "probe ran", "ok": p.clean, **p.facts()},
         {"what": "battle scene reached", "ok": p.e2e.get("battle") == "1"}]
    if rollback:
        return c + [
            {"what": "lobby sent the P2P matching", "ok": "Command: lbsP2PMatching, Direction: ServerToClient" in lbs},
            {"what": "GGPO session started", "ok": "StartGGPOSession" in log and "Peer count 1" in log},
            {"what": "battle loaded on the rollback backend", "ok": "LoadEndMsg Join" in log},
        ]
    return c + [
        {"what": "lobby gave the battle server", "ok": "Command: lbsAskMcsAddress, Direction: ClientToServer" in lbs},
        {"what": "battle server room joined over UDP", "ok": "new McsRoom" in lbs and "join udp peer" in lbs},
        {"what": "UDP backend logged in to the battle",
         "ok": "gdxsv_backend_udp" in log and f"user_id:{USER[0]}" in log},
    ]


def pair(ctx, tag: str):
    """Two clients, two users, one P2P lobby: the first one waits for the second's entry, then
    force-starts."""
    first = {"battle": True, "chat_delay": 900, "in_battle": 600}
    second = {"battle": True, "chat": False, "in_battle": 600}
    return session(ctx, tag, [(tag + "a", LOGIN_KEY, first), (tag + "b", LOGIN_KEY2, second)], mcs_region="p2p")


def pair_checks(probes, lbs: str) -> list:
    c = [{"what": "lobby sent the P2P matching to both",
          "ok": lbs.count("Command: lbsP2PMatching, Direction: ServerToClient") >= 2}]
    for name, p in zip("ab", probes):
        log = p.log
        c += [
            {"what": f"{name}: probe ran", "ok": p.clean, **p.facts()},
            {"what": f"{name}: battle scene reached", "ok": p.e2e.get("battle") == "1"},
            {"what": f"{name}: GGPO session of two peers", "ok": "StartGGPOSession" in log and "Peer count 2" in log},
            {"what": f"{name}: the other peer answered", "ok": re.search(r"Peer\d [\d.]+ms IP:", log) is not None},
            {"what": f"{name}: battle loaded on the rollback backend", "ok": "LoadEndMsg" in log},
        ]
    return c


@case("O4", "two clients, P2P lobby: a rollback battle between two real peers (local gdxsv)")
def online_rollback_pair(ctx):
    probes, lbs, _ = pair(ctx, "O4")
    return result(pair_checks(probes, lbs))
