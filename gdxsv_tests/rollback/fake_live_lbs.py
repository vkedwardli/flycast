"""A stand-in for lbs's live spectator channel and the /status API, for
testing Live Spectate end to end without a match: it streams a replay as a
series of live battles, in real time, and closes each one at its end.

    python gdxsv_tests/rollback/fake_live_lbs.py --replay outsync-test/nanasu/1768580845982.pb --battles 3

Point flycast at it with
    --config gdxsv:server=127.0.0.1 --config gdxsv:LiveApiUrl=http://127.0.0.1:3390
and pick a battle from the Live tab, or pass --config gdxsv:spectate=<code>.

Battle i (code 9000000+i) starts --gap seconds after battle i-1 ended, or
--overlap seconds after it started when that is given. Each is already
--lead seconds in when it starts, like a battle found in progress.
"""
import argparse
import datetime
import json
import os
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from lib import replayfile as rf  # noqa: E402

FPS = 59.94
MAX_PUSH_FRAMES = 128
SUBSCRIBER_TIMEOUT = 10.0
RESEND_INTERVAL = 0.1

# Packet.type and Packet's message fields (gdxsv.proto MessageType)
PUSH, ACK, SUBSCRIBE = 20, 21, 24
# BattleLogFile fields left out of the live header: sent as the battle goes. The header keeps
# the replay's battle_code: the game is given it (lbs's answer) and it changes the play.
STREAMED = {7, 15, 16, 17, 18, 24, 25}


def _int32(v):
    return v - (1 << 64) if v >= 1 << 63 else v


class Log:
    """The parts of a replay file (BattleLogFile) the live channel sends."""

    def __init__(self, path, round_results=True):
        with open(path, "rb") as f:
            data = f.read()
        r = rf.Log(path)
        self.inputs, self.start_msg_indexes, self.start_msg_randoms = r.inputs, r.start_msg_indexes, r.start_msg_randoms
        self.round_data, self.patches, self.users, self.header = [], [], [], b""
        self.game_disk = self.close_reason = ""
        for num, wt, v in rf.fields(data):
            if num == 18 and round_results:
                self.round_data.append(v)
            elif num == 7:
                self.patches.append(v)
            elif num == 24:
                self.close_reason = v.decode("utf-8", "replace")
            if num == 11:  # BattleLogUser: user_id 1, user_name 2, pilot_name 3, team 9, pos 12
                u = {n: x for n, w, x in rf.fields(v)}
                self.users.append({"user_id": u.get(1, b"").decode("utf-8", "replace"),
                                   "user_name": u.get(2, b"").decode("utf-8", "replace"),
                                   "pilot_name": u.get(3, b"").decode("utf-8", "replace"),
                                   "team": _int32(u.get(9, 0)), "pos": _int32(u.get(12, 0))})
            if num == 5:
                self.game_disk = v.decode()
            if num not in STREAMED:
                self.header += rf.put_field(num, wt, v)


class Battle:
    def __init__(self, code, log, max_frames, start_at, lead):
        self.code = code
        self.log = log
        self.total = len(log.inputs) if max_frames <= 0 else min(len(log.inputs), max_frames)
        self.starts = [i for i in log.start_msg_indexes if i <= self.total]
        self.start_at = start_at
        self.lead_frames = int(lead * FPS)
        self.end_at = start_at + (self.total - self.lead_frames) / FPS
        self.header = log.header

    def available(self, now):
        if now < self.start_at:
            return 0
        return min(self.total, self.lead_frames + int((now - self.start_at) * FPS))

    def live(self, now):
        return self.start_at <= now < self.end_at

    def closed(self, now):
        return self.end_at <= now

    def rounds(self, avail):
        return sum(1 for i in self.starts if i <= avail)


class Subscriber:
    def __init__(self, addr, battle, from_frame):
        self.addr = addr
        self.battle = battle
        self.ack_frame = from_frame
        self.patch_ack = 0
        self.round_ack = 0
        self.last_seen = time.time()
        self.last_sent_frame = -1
        # Unacked data is resent on its own timer, apart from new inputs
        # streaming out every frame.
        self.last_resend = 0.0


def push_packet(battle, body):
    """Packet{type, spectator_input_push_data{battle_code, body}}; body: SpectatorInputPush fields."""
    return rf.put(1, PUSH) + rf.put(PUSH, rf.put(1, battle.code) + body)


def send_updates(sock, sub, now):
    b = sub.battle
    avail = b.available(now)
    pushes = []
    resend = RESEND_INTERVAL <= now - sub.last_resend

    patch_total = rf.put(13, len(b.log.patches))
    if sub.ack_frame == 0 and sub.patch_ack == 0 and resend:
        pushes.append(push_packet(b, rf.put(10, b.header) + patch_total))
    if sub.patch_ack < len(b.log.patches) and resend:
        for i in range(sub.patch_ack, len(b.log.patches)):
            pushes.append(push_packet(b, rf.put(11, b.log.patches[i]) + rf.put(12, i) + patch_total))

    rounds = b.rounds(avail)
    final = b.closed(now) and avail == b.total
    # A round's result is known once the next round starts, or at the close.
    results = rounds if final else max(0, rounds - 1)
    version = rounds * 2 + (1 if final else 0)
    if sub.round_ack < version and resend:
        pushes.append(push_packet(b, rf.put_packed(5, b.starts[:rounds]) +
                                  rf.put_packed(6, b.log.start_msg_randoms[:rounds]) +
                                  b"".join(rf.put(7, r) for r in b.log.round_data[:results]) + rf.put(14, version)))

    frame = sub.ack_frame
    if frame < avail and (resend or sub.last_sent_frame < avail):
        if not resend:
            frame = max(frame, sub.last_sent_frame)
        while frame < avail and len(pushes) < 16:
            n = min(MAX_PUSH_FRAMES, avail - frame)
            pushes.append(push_packet(b, rf.put(3, frame) + rf.put_packed(4, b.log.inputs[frame:frame + n], fixed64=True)))
            frame += n
        sub.last_sent_frame = frame

    if final and resend:
        pushes.append(push_packet(b, rf.put(3, b.total) + rf.put(8, b.log.close_reason or "game_end") + rf.put(14, version)))

    for pkt in pushes:
        sock.sendto(pkt, sub.addr)
    if resend:
        sub.last_resend = now


def udp_loop(sock, battles, subs, lock):
    by_code = {b.code: b for b in battles}
    sock.settimeout(0.005)
    while True:
        try:
            data, addr = sock.recvfrom(65536)
        except socket.timeout:
            data = None
        except ConnectionResetError:
            data = None
        now = time.time()
        with lock:
            if data:
                try:
                    pkt = {n: v for n, w, v in rf.fields(data)}
                    msg = {n: v for n, w, v in rf.fields(pkt.get(pkt.get(1, 0), b""))}
                except (ValueError, IndexError, TypeError):
                    pkt, msg = {}, {}
                code = msg.get(1, b"").decode("utf-8", "replace") if isinstance(msg.get(1), bytes) else ""
                # SpectatorSubscribeRequest{battle_code 1, from_frame 3},
                # SpectatorInputAck{battle_code 1, ack_frame 2, patch_ack 3, round_ack 4}
                if pkt.get(1) == SUBSCRIBE:
                    b = by_code.get(code)
                    if b is not None and b.start_at <= now:
                        key = (addr, b.code)
                        if key not in subs:
                            subs[key] = Subscriber(addr, b, msg.get(3, 0))
                            print(f"{time.strftime('%H:%M:%S')} subscribe {b.code} from {addr}", flush=True)
                        subs[key].last_seen = now
                elif pkt.get(1) == ACK:
                    s = subs.get((addr, code))
                    if s is not None:
                        s.ack_frame = max(s.ack_frame, msg.get(2, 0))
                        s.patch_ack = max(s.patch_ack, msg.get(3, 0))
                        s.round_ack = max(s.round_ack, msg.get(4, 0))
                        s.last_seen = now
            for key in list(subs):
                s = subs[key]
                if SUBSCRIBER_TIMEOUT < now - s.last_seen:
                    print(f"{time.strftime('%H:%M:%S')} drop {s.battle.code} {s.addr}", flush=True)
                    del subs[key]
                    continue
                send_updates(sock, s, now)


def make_handler(battles, subs, lock, disk):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, obj):
            body = json.dumps(obj).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            url = urlparse(self.path)
            now = time.time()
            with lock:
                if url.path.endswith("/spectators"):
                    code = parse_qs(url.query).get("battle_code", [""])[0]
                    self.reply({"spectators": sum(1 for s in subs.values() if s.battle.code == code)})
                    return
                if not url.path.endswith("/status"):
                    self.send_error(404)
                    return
                games, users = [], []
                for b in battles:
                    if not b.live(now):
                        continue
                    started = datetime.datetime.fromtimestamp(b.start_at, datetime.timezone.utc).isoformat()
                    games.append({"battle_code": b.code, "disk": disk, "state": "created", "lobby_id": 2,
                                  "live_spectate": True,
                                  "spectators": sum(1 for s in subs.values() if s.battle.code == b.code),
                                  "updated_at": started})
                    for u in b.log.users:
                        users.append({"battle_code": b.code, "user_id": u["user_id"], "name": u["user_name"],
                                      "pilot_name": u["pilot_name"], "battle_pos": u["pos"],
                                      "team": "renpo" if u["team"] == 1 else "zeon"})
            self.reply({"active_games": games, "battle_users": users})

    return Handler


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--replay", required=True, nargs="+", help="one or more replays, used in turn")
    ap.add_argument("--battles", type=int, default=3)
    ap.add_argument("--max-frames", type=int, default=0, help="cut each battle after this many inputs")
    ap.add_argument("--lead", type=float, default=0.0, help="seconds a battle is already in when it starts")
    ap.add_argument("--first", type=float, default=0.0, help="seconds until the first battle starts")
    ap.add_argument("--gap", type=float, default=20.0, help="seconds between a battle's end and the next start")
    ap.add_argument("--overlap", type=float, default=-1, help="start the next battle this many seconds after the previous one started")
    ap.add_argument("--udp-port", type=int, default=9876)
    ap.add_argument("--http-port", type=int, default=3390)
    ap.add_argument("--no-round-results", action="store_true", help="send no round results (round_data)")
    a = ap.parse_args()

    logs = [Log(path, round_results=not a.no_round_results) for path in a.replay]

    battles = []
    t = time.time() + a.first
    for i in range(a.battles):
        log = logs[i % len(logs)]
        b = Battle(str(9000000 + i), log, a.max_frames, t, a.lead)
        battles.append(b)
        print(f"battle {b.code}: {len(log.users)} players, {b.total} inputs, rounds at {b.starts}, "
              f"live {time.strftime('%H:%M:%S', time.localtime(b.start_at))}-{time.strftime('%H:%M:%S', time.localtime(b.end_at))}",
              flush=True)
        t = b.start_at + a.overlap if 0 <= a.overlap else b.end_at + a.gap

    subs, lock = {}, threading.Lock()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", a.udp_port))
    threading.Thread(target=udp_loop, args=(sock, battles, subs, lock), daemon=True).start()
    server = ThreadingHTTPServer(("127.0.0.1", a.http_port), make_handler(battles, subs, lock, logs[0].game_disk))
    print(f"udp :{a.udp_port}, http://127.0.0.1:{a.http_port}/status", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
