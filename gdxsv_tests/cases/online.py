"""Online features against a local gdxsv lobby (lib/servers.py)."""

import json
import os
import re
import shutil
import threading
import time

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


def session(ctx, tag: str, clients: list, mcs_region: str = "", udp_hello: bool = False,
            replay_state: bool = False, during=None):
    """clients = (name, gdxsv:loginkey, lua/login.lua params[, Ctx.probe options]) run at once
    against one lobby; returns ([probe per client], lobby log, fake HTTP requests).
    udp_hello: the fake P2P test answers over UDP (lib/servers.FakeHttp); replay_state: the
    replay boot savestate (slot 99) too; during: called in a thread with the clients' directories."""
    rom = ctx.rom("dc2")
    exe = servers.server_exe()
    if exe is None:
        raise Skip("no gdxsv server build (GDXSV_SERVER_EXE)")
    states = assets.login_state_dir()
    if states is None:
        raise Skip("no network menu savestate (GDXSV_LOGIN_STATE)")
    if replay_state and states:
        both = states + "_replay"
        os.makedirs(both, exist_ok=True)
        for d in (states, assets.state_dir(rom)):
            for name in os.listdir(d):
                shutil.copy(os.path.join(d, name), both)
        states = both
    lobby = servers.Lobby(exe, os.path.join(ctx.out, tag + "_lbs"))
    lobby.init([(servers.server_login_key(LOGIN_KEY),) + USER, (servers.server_login_key(LOGIN_KEY2),) + USER2])
    servers.add_training_lobby(lobby.db, mcs_region=mcs_region)
    probes = [None] * len(clients)
    with servers.FakeHttp(body=b"127.0.0.1", udp_hello=udp_hello) as http:
        def run(i, name, key, params, opts=None):
            battle = params.get("battle", False)
            opts = dict(opts or {})
            cfg = http.config() + ["gdxsv:server=127.0.0.1", f"gdxsv:loginkey={key}"] + opts.pop("config", [])
            kw = {"frames": BATTLE_FRAMES if battle else LOGIN_FRAMES, "until_done": battle, **opts}
            probes[i] = ctx.probe(name, rom, "login.lua", {"boot": BOOT, "slot": assets.LOGIN_SLOT, **params},
                                  config=cfg, loadstate=False, states=states, timeout=900, **kw)
        lobby.start()
        try:
            threads = [threading.Thread(target=run, args=(i,) + tuple(c)) for i, c in enumerate(clients)]
            for t in threads:
                t.start()
            if during:
                threading.Thread(target=during, args=([os.path.join(ctx.out, c[0]) for c in clients],),
                                 daemon=True).start()
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


# O7: a's flycast.log lines (gdxsv.cpp, gdxsv_replay_util.cpp)
TEST_STARTED = "test: starting replay"
TEST_REFUSED = "test: menu refused"
SUSPENDED = "lobby: suspended for replay playback"
RESUMED = re.compile(r"lobby: resumed with (\d+) held messages")
MATCH_FOUND = "GdxsvBackendRollback.Prepare"  # after lbsReadyBattle, the P2P matching
BATTLE_OPEN = "GdxsvBackendRollback.Open"  # the battle connection


def read_log_file(path: str) -> str:
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def read_log(d: str) -> str:
    return read_log_file(os.path.join(d, "flycast.log"))


def lobby_replay(ctx, tag: str, four_screen: bool = False):
    """a logs in to the P2P lobby and, on the plaza map, starts the last round of a corpus replay from the
    lobby (GDXSV_TEST_START_REPLAY, a in a window: a headless replay end exits the process). Once a's lobby
    has resumed, a joins lobby 2, enters and tries the replay again (refused); b joins, enters and
    force-starts. four_screen: the replay plays on four screens (its guests log to a's log dir)."""
    from cases.replay import newest_dc2
    from lib import replayfile
    replay = assets.replay(newest_dc2())
    last_round = len(replayfile.start_indexes(replay))

    def watch(dirs):
        a, b = dirs
        deadline = time.time() + 900
        while time.time() < deadline and not RESUMED.search(read_log(a)):
            time.sleep(0.5)
        open(os.path.join(a, "lobby_back.flag"), "w").close()
        open(os.path.join(b, "go.flag"), "w").close()

    trigger = ("four_screen " if four_screen else "") + replay
    first = {"battle": True, "chat": False, "in_battle": 600, "lobby_replay": ["start_replay.txt", trigger],
             "lobby_back": "lobby_back.flag"}
    first_opts = {"window": True, "frames": 80000, "env": {"GDXSV_TEST_START_REPLAY": "start_replay.txt"},
                  "config": [f"gdxsv:replay_target_round={last_round}"]}
    if four_screen:
        # The host starts its guests without its --config values: their logging comes from emu.cfg.
        first_opts.update(emu_cfg="[log]\nLogToFile = 1\nVerbosity = 3\n",
                          config=first_opts["config"] + ["window:width=480", "window:height=360"])
    second = {"battle": True, "chat_delay": 300, "in_battle": 600, "wait_file": "go.flag", "wait_file_max": 18000}
    return session(ctx, tag, [(tag + "a", LOGIN_KEY, first, first_opts), (tag + "b", LOGIN_KEY2, second)],
                   mcs_region="p2p", replay_state=True, during=watch)


def lobby_replay_checks(probes, lbs: str) -> list:
    a = probes[0].log
    m = RESUMED.search(a)
    started = a.find(SUSPENDED)
    resumed = m.start() if m else -1
    return [
        {"what": "a: replay started from the lobby", "ok": TEST_STARTED in a and started >= 0
         and "Loaded state" in a[started:]},
        {"what": "a: lobby resumed after the replay", "held": int(m.group(1)) if m else None, "ok": resumed > started >= 0},
        {"what": "a: back on the plaza map", "ok": probes[0].e2e.get("lobby_back") == "1"},
        {"what": "a: menu refused while waiting for a match", "ok": resumed >= 0 and TEST_REFUSED in a[resumed:]},
        {"what": "a: no replay started after the entry", "ok": resumed >= 0 and TEST_STARTED not in a[resumed:]},
        {"what": "a: menu refused between the match and the battle connection",
         "ok": 0 <= a.find(MATCH_FOUND) < a.find(BATTLE_OPEN)
         and TEST_REFUSED in a[a.find(MATCH_FOUND):a.find(BATTLE_OPEN)]},
    ] + pair_checks(probes, lbs)


@case("O7", "replay from the online lobby: the lobby resumes after it, the menu is refused while waiting for "
            "a match, and the match plays (local gdxsv, two clients)")
def online_lobby_replay(ctx):
    probes, lbs, _ = lobby_replay(ctx, "O7")
    return result(lobby_replay_checks(probes, lbs))


@case("O8", "O7 with the replay on four screens: three guests play it and close when it ends, then the lobby "
            "resumes and the match plays (local gdxsv, two clients)")
def online_lobby_replay_four_screen(ctx):
    probes, lbs, _ = lobby_replay(ctx, "O8", four_screen=True)
    a = probes[0]
    host = re.search(r"multi-pov: host session (\S+) with (\d+) guests", a.log)
    log_dir = os.path.dirname(a.inst.log_path)
    guests = []
    for n in (2, 3, 4):
        log = read_log_file(os.path.join(log_dir, f"flycast-{n}P.log"))
        guests.append({"what": f"guest {n}P played the host's replay and closed",
                       "ok": f"guest {n}P playing from the host's replay" in log and "closing this screen" in log})
    return result([{"what": "a: four-screen session with 3 guests", "ok": bool(host) and host.group(2) == "3"}]
                  + guests + lobby_replay_checks(probes, lbs))
