#!/usr/bin/env python3
"""Commands for the running wheel server (on the same machine).

  python3 wheel_cmd.py center   hold the wheel straight, then run this: saves it as straight ahead
  python3 wheel_cmd.py reset    back to the wheel's own centre
  python3 wheel_cmd.py status   where the wheel is now (percent of full lock, degrees)
  python3 wheel_cmd.py watch    status ten times a second until Ctrl+C
"""
import socket
import sys
import time

PORT = 27920


def ask(sock, text):
    sock.sendto(b"GT3C" + text.encode(), ("127.0.0.1", PORT))
    try:
        return sock.recvfrom(512)[0].decode()
    except socket.timeout:
        return "no answer: is the wheel server running? (and is the wheel plugged in?)"


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd not in ("center", "reset", "status", "watch"):
        print(__doc__)
        return
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)
    if cmd == "watch":
        try:
            while True:
                print("\r" + ask(sock, "status").ljust(100), end="", flush=True)
                time.sleep(0.1)
        except KeyboardInterrupt:
            print()
        return
    print(ask(sock, cmd))


if __name__ == "__main__":
    main()
