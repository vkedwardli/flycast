"""Local servers for the online cases: the real gdxsv lobby (inada-s/gdxsv `gdxsv lbs`,
SQLite) and a fake HTTP server for the startup network checks. The lobby binds its ports on every
interface (gdxsv uses only the port of GDXSV_LOBBY_ADDR); clients are pointed at 127.0.0.1."""

import http.server
import os
import sqlite3
import subprocess
import threading
import time
from typing import Dict, List, Optional


LOBBY_PORT = 9876  # the client's lobby port is fixed (kGdxsvLbsPort)
BATTLE_PORT = 9877
LOBBY_LOCK = threading.Lock()  # run.py --jobs runs cases in threads
LOBBY_HTTP_PORT = 3380


def server_exe() -> Optional[str]:
    """inada-s/gdxsv built without the flycast version gate:
    go build -ldflags "-X main.requiredFlycastVersion=" -o gdxsv ./gdxsv"""
    path = os.environ.get("GDXSV_SERVER_EXE")
    return os.path.abspath(path) if path and os.path.isfile(path) else None


class Lobby:
    """`gdxsv lbs` with a fresh database in dir; one at a time (fixed ports)."""

    def __init__(self, exe: str, dir: str):
        self.exe = exe
        self.dir = os.path.abspath(dir)
        self.db = os.path.join(self.dir, "gdxsv.db")
        self.log_path = os.path.join(self.dir, "lbs.log")
        self.proc: Optional[subprocess.Popen] = None
        self._lock = None

    def env(self) -> dict:
        e = os.environ.copy()
        e.update(GDXSV_DB_NAME=self.db,
                 GDXSV_LOBBY_ADDR=f"127.0.0.1:{LOBBY_PORT}", GDXSV_LOBBY_PUBLIC_ADDR=f"127.0.0.1:{LOBBY_PORT}",
                 GDXSV_BATTLE_ADDR=f"127.0.0.1:{BATTLE_PORT}", GDXSV_BATTLE_PUBLIC_ADDR=f"127.0.0.1:{BATTLE_PORT}",
                 GDXSV_LOBBY_HTTP_ADDR=f"127.0.0.1:{LOBBY_HTTP_PORT}",
                 GDXSV_GCP_PROJECT_ID="", GDXSV_MCSFUNC_KEY="", GDXSV_MCSFUNC_URL="", GDXSV_LOBBY_WEBHOOK_URL="")
        return e

    def init(self, users: List[tuple]) -> None:
        """New database; users = (login_key, user_id, handle_name) rows, accounts included."""
        os.makedirs(self.dir, exist_ok=True)
        for name in os.listdir(self.dir):
            if name.startswith("gdxsv.db"):
                os.remove(os.path.join(self.dir, name))
        subprocess.check_call([self.exe, "initdb"], cwd=self.dir, env=self.env(),
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        c = sqlite3.connect(self.db)
        for key, uid, name in users:
            c.execute("INSERT OR IGNORE INTO account (login_key, created_ip, created, last_login, system) "
                      "VALUES (?, '127.0.0.1', datetime('now'), datetime('now'), 0)", (key,))
            c.execute("INSERT INTO user (user_id, login_key, name, created) VALUES (?, ?, ?, datetime('now'))",
                      (uid, key, name))
        c.commit()
        c.close()

    def start(self) -> None:
        LOBBY_LOCK.acquire()
        self._lock = LOBBY_LOCK
        log = open(self.log_path, "w")
        self.proc = subprocess.Popen([self.exe, "-v=3", "lbs"], cwd=self.dir, env=self.env(),
                                     stdout=log, stderr=subprocess.STDOUT)
        if not wait_port(LOBBY_PORT, 20):
            self.stop()
            raise RuntimeError(f"gdxsv lbs did not listen on {LOBBY_PORT}; see {self.log_path}")

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(10)
        if self._lock:
            self._lock.release()
            self._lock = None

    def log(self) -> str:
        with open(self.log_path, encoding="utf-8", errors="replace") as f:
            return f.read()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()


def wait_port(port: int, timeout: float) -> bool:
    import socket
    t0 = time.time()
    while time.time() - t0 < timeout:
        with socket.socket() as s:
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return True
        time.sleep(0.2)
    return False


class FakeHttp:
    """Answers every request with 200 and `body`, and records "METHOD path" per request.
    Stands in for the public IP, P2P test and latency URLs (emu.net_config).
    routes: path (query dropped) -> body instead; other paths get 404."""

    def __init__(self, body: bytes = b"127.0.0.1", routes: Optional[Dict[str, bytes]] = None):
        self.requests: List[str] = []
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def handle_one(self):
                n = int(self.headers.get("Content-Length") or 0)
                if n:
                    self.rfile.read(n)
                owner.requests.append(f"{self.command} {self.path}")
                data = body if routes is None else routes.get(self.path.split("?")[0])
                self.send_response(200 if data is not None else 404)
                data = data or b""
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            do_GET = do_POST = do_HEAD = handle_one

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def config(self) -> List[str]:
        """Client settings that send the startup network checks here."""
        url = f"http://127.0.0.1:{self.port}"
        return [f"gdxsv:PublicIpUrl={url}/ip", f"gdxsv:UdpTestUrl={url}/udptest",
                f"gdxsv:LatencyHost=127.0.0.1:{self.port}", "network:EnableUPnP=no"]

    def stop(self) -> None:
        self.server.shutdown()
        self.server.server_close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()


def server_login_key(key: str) -> str:
    """The account key the lobby stores for a client's gdxsv:loginkey: the client sends the key
    XORed with a magic (gdxsv.cpp platform info), the server keeps its FNV-1 32 hash in hex."""
    magic = (0x46, 0xcf, 0x2d, 0x55)
    h = 0x811c9dc5
    for i, ch in enumerate(key.encode()):
        h = (h * 0x01000193) & 0xffffffff
        h ^= ch ^ magic[i & 3]
    return f"{h:08x}"


def add_training_lobby(db: str, no: int = 2, force_start: bool = True, mcs_region: str = "") -> None:
    """Disc 2 lobby `no` with rule 'training' (timer 4, vitals 1000: the lobby treats it as a
    training lobby, where one entry and the chat message "あ" start a battle if force_start).
    mcs_region "p2p": rollback battles between the clients; "": the local battle server."""
    c = sqlite3.connect(db)
    c.execute("INSERT OR REPLACE INTO m_rule VALUES ('training',3,2,4,0,0,1,1000,1000,0,0,0,0,0,0,"
              "4294967295,4294967295,0,1,255,1,4294967295,4294967295,0)")
    c.execute("DELETE FROM m_lobby_setting WHERE platform='emu-x86/64' AND disk='dc2' AND no=?", (no,))
    c.execute("INSERT INTO m_lobby_setting (platform, disk, no, name, enable_force_start, team_shuffle, ping_limit, mcs_region,"
              " patch_names, rule_id) VALUES ('emu-x86/64', 'dc2', ?, ?, ?, 0, 0, ?, '', 'training')",
              (no, f"e2e{no}", int(force_start), mcs_region))
    c.commit()
    c.close()
