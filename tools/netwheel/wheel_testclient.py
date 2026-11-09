#!/usr/bin/env python3
"""Stand-in for the game: shows the wheel state, and can play test forces.

  python3 wheel_testclient.py [host]        show the wheel state
  python3 wheel_testclient.py [host] ffb    also play gentle test forces for 15 seconds
"""
import math
import socket
import struct
import sys
import time

args = [a for a in sys.argv[1:] if a != "ffb"]
host = args[0] if args else "127.0.0.1"
play = "ffb" in sys.argv[1:]
PORT = 27920
HDR = struct.Struct("<4sBBH")
STATE = struct.Struct("<IffffIbbbB")
FFB = struct.Struct("<ffffffffBhh")
ACK = struct.Struct("<BH32s")

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setblocking(False)
addr = (host, PORT)
seq = 0


def send(typ, payload=b""):
    global seq
    seq = (seq + 1) & 0xFFFF
    sock.sendto(HDR.pack(b"GT3W", 1, typ, seq) + payload, addr)


start = time.time()
last_hello = 0.0
last_print = 0.0
st = None
print("Connecting to", host, "- Ctrl+C to stop")
try:
    while True:
        now = time.time()
        if now - last_hello > 1.0:
            send(1, b"\x01")
            last_hello = now
        try:
            while True:
                data, _ = sock.recvfrom(512)
                magic, ver, typ, _s = HDR.unpack_from(data)
                body = data[HDR.size:]
                if typ == 2:
                    caps, rng, name = ACK.unpack_from(body)
                    print("Connected to:", name.split(b"\0")[0].decode(), "- range", rng, "degrees")
                elif typ == 3:
                    st = STATE.unpack_from(body)
        except BlockingIOError:
            pass
        if play and now - start < 15 and st is not None:
            t = now - start
            push = int(6000 * math.sin(t * math.pi))          # positive = right
            send(4, FFB.pack(80.0, 0, 0, 0, 0, 0, 0, 0, 1, 8000, push))
        if st and now - last_print > 0.2:
            last_print = now
            ms, steer, thr, brk, clt, btn, hx, hy, gear, _p = st
            print("steer %+.2f  throttle %.2f  brake %.2f  clutch %.2f  gear %2d  buttons %05x  dpad %+d,%+d"
                  % (steer, thr, brk, clt, gear, btn, hx, hy))
        time.sleep(0.004)
except KeyboardInterrupt:
    send(5)
    print("bye")
