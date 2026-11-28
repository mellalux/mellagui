#!/usr/bin/env python3
"""A stand-in for a XiongMai network camera (DVRIP, TCP): login, keep alive, claim and start of a stream.

    fake_dvrip.py DIR [--password PW]

DIR holds main.h264 and sub.h264 (Annex B, an access unit delimiter in front of every frame, no B frames); they are sent over and over, ten
frames a second, cut into messages anywhere. Prints "port N" when it listens; every login, claim and start is a line on stdout. A file
DIR/drop, when it appears, is removed and the stream's connection is closed (a camera that goes away for a moment).
DIR/alarm ("EVENT Start|Stop"), when it appears, is removed and told to whoever asked for alarms. DIR/files.json lists the recordings of the
card; each is 30 pictures of the small stream with six seconds of sound, sent slowly while DIR/slow is there.
"""
import hashlib
import json
import os
import re
import socket
import struct
import sys
import threading
import time

PARAM = [{"AeSensitivity": 5, "DayNightColor": "0x00000003", "PictureFlip": "0x1", "Odd": {"DayNightColors": ["}", "]"]}}]
LAMP = {"Brightness": 50, "WorkMode": "Close", "WorkPeriod": {"EHour": 6}}

HASHSET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"


def sofia(pw):
    d = hashlib.md5(pw.encode()).digest()
    return "".join(HASHSET[(d[2 * i] + d[2 * i + 1]) % 62] for i in range(8))


TONE = (b"\xaa" * 8 + b"\x2a" * 8) * 50          # a loud 500 Hz square wave, A-law, a tenth of a second


def say(*a):
    print(*a, flush=True)


def frames(path):
    data = open(path, "rb").read()
    cuts = [m.start() for m in re.finditer(b"\x00\x00\x00\x01\x09", data)]
    out = []
    for a, b in zip(cuts, cuts[1:] + [len(data)]):
        unit = data[a + 6:b]            # without the delimiter, as a camera sends it
        key = any((unit[m.end()] & 0x1f) == 5 for m in re.finditer(b"\x00\x00\x01", unit) if m.end() < len(unit))
        out.append((key, unit))
    return out


def recv_all(s, n):
    b = b""
    while len(b) < n:
        x = s.recv(n - len(b))
        if not x:
            raise OSError("closed")
        b += x
    return b


def recv_msg(s):
    h = struct.unpack("<BBHIIBBHI", recv_all(s, 20))
    return h[7], json.loads(recv_all(s, h[8]).rstrip(b"\x00\n"))


def send_msg(s, msg, body, session=1, lock=None):
    data = struct.pack("<BBHIIBBHI", 0xff, 1, 0, session, 0, 0, 0, msg, len(body)) + body
    if lock:
        with lock:
            s.sendall(data)
    else:
        s.sendall(data)


class Camera:
    def __init__(self, folder, password):
        self.folder, self.password = folder, password
        self.claimed = {}               # (session, stream type) -> the data connection
        self.sessions = 41
        self.config = {"Camera.Param": PARAM, "Camera.WhiteLight": LAMP}

    def stream(self, kind, sock):
        name = "main" if kind == "Main" else "sub"
        units = frames(os.path.join(self.folder, name + ".h264"))
        w, h = (int(v) for v in open(os.path.join(self.folder, name + ".size")).read().split())
        pending = b""
        n = 0
        try:
            while True:
                key, unit = units[n % len(units)]
                n += 1
                if key:
                    head = struct.pack("<4sBBBBII", b"\x00\x00\x01\xfc", 2, 10, w // 8, h // 8, 0, len(unit))
                else:
                    head = struct.pack("<4sI", b"\x00\x00\x01\xfd", len(unit))
                pending += head + unit + struct.pack("<4sBBH", b"\x00\x00\x01\xfa", 14, 2, 800) + TONE
                while len(pending) > 700:                       # messages end anywhere in a frame
                    send_msg(sock, 1412, pending[:700])
                    pending = pending[700:]
                send_msg(sock, 1412, pending)
                pending = b""
                drop = os.path.join(self.folder, "drop")
                if os.path.exists(drop):
                    os.unlink(drop)
                    say("dropped")
                    break
                time.sleep(0.1)
        except OSError:
            pass
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    def alarms(self, sock, session, lock):
        path = os.path.join(self.folder, "alarm")
        try:
            while True:
                time.sleep(0.05)
                if os.path.exists(path):
                    event, status = open(path).read().split()
                    os.unlink(path)
                    body = {"Name": "AlarmInfo", "SessionID": session, "AlarmInfo": {"Channel": 0, "Event": event, "Status": status, "StartTime": "2026-01-01 00:00:00"}}
                    send_msg(sock, 1504, json.dumps(body).encode() + b"\n\x00", lock=lock)
                    say("alarm", event, status)
        except OSError:
            pass

    def playback(self, sock):
        units = frames(os.path.join(self.folder, "sub.h264"))
        w, h = (int(v) for v in open(os.path.join(self.folder, "sub.size")).read().split())
        data = b""
        for n in range(30):
            key, unit = units[n % len(units)]
            head = struct.pack("<4sBBBBII", b"\x00\x00\x01\xfc", 2, 10, w // 8, h // 8, 0, len(unit)) if key else struct.pack("<4sI", b"\x00\x00\x01\xfd", len(unit))
            data += head + unit + struct.pack("<4sBBH", b"\x00\x00\x01\xfa", 14, 2, 1600) + TONE + TONE      # (6 s of sound: not the rate the stream claims)
        try:
            for at in range(0, len(data), 900):
                send_msg(sock, 1426, data[at:at + 900])
                while os.path.exists(os.path.join(self.folder, "slow")):
                    send_msg(sock, 1426, b"\x00")
                    time.sleep(0.2)
            send_msg(sock, 1426, b"")
        except OSError:
            pass

    def client(self, s):
        lock = threading.Lock()
        try:
            while True:
                msg, body = recv_msg(s)
                if msg == 1000:
                    good = body.get("UserName") == "admin" and body.get("PassWord") == sofia(self.password)
                    say("login", body.get("UserName"), "ok" if good else "refused")
                    self.sessions += 1
                    answer = {"Ret": 100, "SessionID": "0x%08X" % self.sessions, "AliveInterval": 4, "ChannelNum": 1} if good else {"Ret": 203}
                    send_msg(s, 1001, json.dumps(answer).encode() + b"\n\x00")
                elif msg == 1413:
                    kind = body["OPMonitor"]["Parameter"]["StreamType"]
                    say("claim", kind)
                    self.claimed[body["SessionID"], kind] = s      # (before the answer: the start may follow at once)
                    send_msg(s, 1414, b'{"Ret":100}\n\x00')
                    while s.recv(4096):                          # the data connection only listens from here
                        pass
                    return
                elif msg == 1410:
                    kind = body["OPMonitor"]["Parameter"]["StreamType"]
                    say("start", kind)
                    send_msg(s, 1411, b'{"Ret":100}\n\x00')
                    who = body["SessionID"], kind
                    if who in self.claimed:
                        threading.Thread(target=self.stream, args=(kind, self.claimed.pop(who)), daemon=True).start()
                elif msg == 1400:
                    p = body["OPPTZControl"]
                    say("ptz", p["Command"], p["Parameter"]["Preset"], p["Parameter"]["Step"])
                    send_msg(s, 1401, b'{"Ret":100}\n\x00')
                elif msg == 1042:
                    name = body["Name"]
                    answer = {"Name": name, "Ret": 100, name: self.config[name]} if name in self.config else {"Name": name, "Ret": 607}
                    send_msg(s, 1043, json.dumps(answer).encode() + b"\n\x00")
                elif msg == 1040:
                    name = body["Name"]
                    self.config[name] = body[name]
                    say("set", name, json.dumps(body[name], sort_keys=True))
                    send_msg(s, 1041, b'{"Ret":100}\n\x00')
                elif msg == 1500:
                    say("subscribe")
                    send_msg(s, 1501, b'{"Ret":100}\n\x00', lock=lock)
                    threading.Thread(target=self.alarms, args=(s, body["SessionID"], lock), daemon=True).start()
                elif msg == 1440:
                    q = body["OPFileQuery"]
                    say("files", q["BeginTime"].replace(" ", "_"), q["EndTime"].replace(" ", "_"))
                    path = os.path.join(self.folder, "files.json")
                    found = [f for f in (json.load(open(path)) if os.path.exists(path) else []) if q["BeginTime"] <= f["BeginTime"] <= q["EndTime"]]
                    answer = {"Ret": 100, "OPFileQuery": found[:64]} if found else {"Ret": 119}
                    send_msg(s, 1441, json.dumps(answer).encode() + b"\n\x00")
                elif msg == 1424:
                    name = body["OPPlayBack"]["Parameter"]["FileName"]
                    say("claim play", name)
                    self.claimed[body["SessionID"], "play"] = s
                    send_msg(s, 1425, b'{"Ret":100}\n\x00')
                    while s.recv(4096):
                        pass
                    return
                elif msg == 1420:
                    action = body["OPPlayBack"]["Action"]
                    say("playback", action)
                    send_msg(s, 1421, b'{"Ret":100}\n\x00')
                    who = body["SessionID"], "play"
                    if action == "DownloadStart" and who in self.claimed:
                        threading.Thread(target=self.playback, args=(self.claimed.pop(who),), daemon=True).start()
                elif msg == 1006:
                    say("keepalive")
                    send_msg(s, 1007, b'{"Ret":100}\n\x00', lock=lock)
        except (OSError, ValueError):
            pass
        finally:
            s.close()


def main():
    folder = sys.argv[1]
    password = sys.argv[sys.argv.index("--password") + 1] if "--password" in sys.argv else ""
    cam = Camera(folder, password)
    l = socket.socket()
    l.bind(("127.0.0.1", 0))
    l.listen(8)
    say("port", l.getsockname()[1])
    while True:
        s, _ = l.accept()
        threading.Thread(target=cam.client, args=(s,), daemon=True).start()


if __name__ == "__main__":
    main()
