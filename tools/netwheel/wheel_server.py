#!/usr/bin/env python3
"""GT3 network wheel server for the Logitech G920 (protocol v1).

Reads the wheel with evdev, streams its state over UDP to the game, and plays the
game's force feedback back on the wheel. See wheel_protocol.md.
"""
import glob
import os
import select
import socket
import struct
import sys
import time

from evdev import InputDevice, list_devices, ecodes, ff

# ---------------------------------------------------------------- settings
PORT = 27920
GAIN = 0.6               # overall wheel force on the Pi side, 0.0 .. 1.0
INVERT_FORCE = False     # set True if the steering force pushes the wrong way
PEDALS_REST_HIGH = True  # G920 pedals read 255 when released and 0 when pressed
WHEEL_RANGE_DEGREES = 360      # lock-to-lock; GT3 steers full lock at the ends (its 2001 wheels turned ~200)
STATE_HZ = 100           # wheel state packets per second while a game is connected
FFB_TIMEOUT = 0.30       # seconds without a force packet before the wheel goes limp
CLIENT_TIMEOUT = 3.0     # seconds without any packet before the game is forgotten
FFB_MAX_HZ = 60          # most force updates per second sent to the wheel

# While no game is connected (idle):
IDLE_FORCES = True       # False: the wheel stays limp while no game is connected
IDLE_SPRING = 0.30       # centring force, 0.0 (none) .. 1.0 (the wheel's strongest spring)
WARN_START = 0.95        # rumble from this much of full lock (0.95 = 95%) ...
WARN_SOFT = 0.15         # ... this softly (0.0 .. 1.0) ...
WARN_STRONG = 0.45       # ... growing to this at 99% ...
WARN_STOP = 0.995        # ... and nothing from here on (full lock), so it doesn't buzz against the stop
CENTER_FILE = "~/.wheel_center"  # the straight-ahead position saved by: python3 wheel_cmd.py center

# ---------------------------------------------------------------- protocol
VERSION = 1
MAGIC = b"GT3W"
T_HELLO, T_HELLO_ACK, T_STATE, T_FFB, T_BYE = 1, 2, 3, 4, 5
HDR = struct.Struct("<4sBBH")              # magic, version, type, sequence
ACK = struct.Struct("<BH32s")              # capabilities, range degrees, device name
STATE = struct.Struct("<IffffIbbbB")       # ms, steer, throttle, brake, clutch, buttons, hat x, hat y, gear, pad
FFB = struct.Struct("<ffffffffBhh")        # speed, brake, abs, steer_force, jolt, traction,
                                           # understeer, oversteer, flags, spring, constant
CAP_FFB, CAP_SHIFTER = 1, 2
CMD_MAGIC = b"GT3C"      # local commands (wheel_cmd.py), accepted from this machine only

# Button bit positions in the state packet (evdev code -> bit)
BUTTON_BITS = {
    288: 0, 289: 1, 290: 2, 291: 3,        # A B X Y
    292: 4, 293: 5,                         # right paddle, left paddle
    294: 6, 295: 7,                         # menu, view
    296: 8, 297: 9, 298: 10,                # RSB, LSB, Xbox
    299: 11,                                # shifter reverse
    300: 12, 301: 13, 302: 14, 303: 15, 704: 16, 705: 17,  # shifter gears 1..6
}
GEAR_CODES = {300: 1, 301: 2, 302: 3, 303: 4, 704: 5, 705: 6}
REVERSE_CODE = 299


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


CENTER = 32768           # the wheel's raw straight-ahead value (re-centred by wheel_cmd.py center)


def load_center():
    global CENTER
    try:
        with open(os.path.expanduser(CENTER_FILE)) as f:
            CENTER = int(clamp(int(f.read().strip()), 1000, 64535))
        if CENTER != 32768:
            log("using the saved centre:", CENTER)
    except (OSError, ValueError):
        CENTER = 32768


def save_center(value):
    global CENTER
    CENTER = int(clamp(value, 1000, 64535))
    path = os.path.expanduser(CENTER_FILE)
    if CENTER == 32768:
        try:
            os.remove(path)
        except OSError:
            pass
    else:
        with open(path, "w") as f:
            f.write(str(CENTER))


def center_ff():
    """The centre as the spring effect's centre (-32767..32767 over the whole range)."""
    return int(clamp((CENTER - 32768) / 32768.0, -1.0, 1.0) * 32767)


def norm_steer(raw):
    return clamp((raw - CENTER) / 32768.0, -1.0, 1.0)


def norm_pedal(raw):
    v = raw / 255.0
    if PEDALS_REST_HIGH:
        v = 1.0 - v
    return clamp(v, 0.0, 1.0)


def gear_from_keys(keys):
    if keys.get(REVERSE_CODE):
        return -1
    for code, g in GEAR_CODES.items():
        if keys.get(code):
            return g
    return 0


def buttons_from_keys(keys):
    mask = 0
    for code, bit in BUTTON_BITS.items():
        if keys.get(code):
            mask |= 1 << bit
    return mask


# ---------------------------------------------------------------- force feedback
class Ffb:
    """Plays the game's spring, steering force and vibration on the wheel."""

    def __init__(self, dev):
        self.dev = dev
        self.ids = {"spring": None, "const": None, "vib": None}
        self.last = {"spring": None, "const": None, "vib": None}
        self.last_sent = 0.0

    def setup(self):
        self.dev.write(ecodes.EV_FF, ecodes.FF_GAIN, int(clamp(GAIN, 0, 1) * 0xFFFF))
        self.dev.write(ecodes.EV_FF, ecodes.FF_AUTOCENTER, 0)

    def _play(self, name, effect):
        effect.id = self.ids[name] if self.ids[name] is not None else -1
        eid = self.dev.upload_effect(effect)
        if self.ids[name] is None:
            self.ids[name] = eid
            self.dev.write(ecodes.EV_FF, eid, 1)

    def _stop_one(self, name):
        eid = self.ids[name]
        if eid is not None:
            try:
                self.dev.write(ecodes.EV_FF, eid, 0)
                self.dev.erase_effect(eid)
            except OSError:
                pass
        self.ids[name] = None
        self.last[name] = None

    def stop(self):
        for name in self.ids:
            self._stop_one(name)

    def apply(self, spring, const, vib, now):
        """spring 0..32767, const +-32767 (positive pushes right), vib 0..1"""
        if now - self.last_sent < 1.0 / FFB_MAX_HZ:
            return
        spring = int(clamp(spring, 0, 32767))
        const = int(clamp(const, -32767, 32767))
        vib_mag = int(clamp(vib, 0.0, 1.0) * 9000)
        # Positive "right" force: the wheel's own convention for direction 0x4000 is leftwards,
        # so the sign is flipped here (INVERT_FORCE flips it back).
        push = const if INVERT_FORCE else -const
        sent = False

        spring_key = (spring, center_ff())
        if spring_key != self.last["spring"]:
            if spring > 0:
                c = ff.Condition(0x7FFF, 0x7FFF, spring, spring, 0, center_ff())
                self._play("spring", ff.Effect(
                    ecodes.FF_SPRING, -1, 0, ff.Trigger(0, 0), ff.Replay(0, 0),
                    ff.EffectType(ff_condition_effect=(c, c))))
            else:
                self._stop_one("spring")
            self.last["spring"] = spring_key
            sent = True

        if push != self.last["const"]:
            if push != 0:
                self._play("const", ff.Effect(
                    ecodes.FF_CONSTANT, -1, 0x4000, ff.Trigger(0, 0), ff.Replay(0, 0),
                    ff.EffectType(ff_constant_effect=ff.Constant(push, ff.Envelope(0, 0, 0, 0)))))
            else:
                self._stop_one("const")
            self.last["const"] = push
            sent = True

        if vib_mag != self.last["vib"]:
            if vib_mag > 300:
                self._play("vib", ff.Effect(
                    ecodes.FF_PERIODIC, -1, 0x4000, ff.Trigger(0, 0), ff.Replay(0, 0),
                    ff.EffectType(ff_periodic_effect=ff.Periodic(
                        ecodes.FF_SINE, 40, vib_mag, 0, 0, ff.Envelope(0, 0, 0, 0), 0, None))))
            else:
                self._stop_one("vib")
            self.last["vib"] = vib_mag
            sent = True

        if sent:
            self.last_sent = now


def idle_forces(steer):
    """The idle mode's spring and rumble for a steering position -1..1 (no game connected)."""
    p = abs(steer)
    if p < WARN_START or p >= WARN_STOP:
        vib = 0.0
    else:
        t = clamp((p - WARN_START) / max(0.001, (0.99 - WARN_START)), 0.0, 1.0)
        vib = WARN_SOFT + t * (WARN_STRONG - WARN_SOFT)
    return int(clamp(IDLE_SPRING, 0.0, 1.0) * 32767), vib


# ---------------------------------------------------------------- wheel + server
def find_wheel():
    for path in list_devices():
        d = InputDevice(path)
        if "G920" in d.name:
            return d
    return None


def set_range(dev):
    for path in glob.glob("/sys/class/input/%s/device/device/range" % dev.path.split("/")[-1]):
        try:
            with open(path, "w") as f:
                f.write(str(WHEEL_RANGE_DEGREES))
            log("wheel range set to", WHEEL_RANGE_DEGREES, "degrees")
        except OSError as e:
            log("could not set the wheel range:", e)


def command(text, steer_raw, client):
    """A local command from wheel_cmd.py; returns the answer shown there."""
    half = WHEEL_RANGE_DEGREES / 2.0
    if text == "center":
        save_center(steer_raw)
        log("new centre saved:", CENTER)
        return "centre saved: this position is now straight ahead"
    if text == "reset":
        save_center(32768)
        log("centre reset to the wheel's own")
        return "centre reset to the wheel's own calibration"
    if text == "status":
        s = norm_steer(steer_raw)
        side = "right" if s > 0 else "left" if s < 0 else ""
        return ("steering %.0f%% %s (%.0f of %.0f degrees), %s, centre %s" % (
            abs(s) * 100, side, abs(s) * half, half,
            "game connected from %s" % client[0] if client else "no game connected (idle forces %s)" %
            ("on" if IDLE_FORCES else "off"),
            "the wheel's own" if CENTER == 32768 else "saved (%d)" % CENTER)).replace("  ", " ")
    return "unknown command: %s (use center, reset or status)" % text


def run(dev, sock):
    set_range(dev)
    ffb = Ffb(dev)
    ffb.setup()
    log("wheel ready:", dev.name, "- listening on UDP port", PORT)

    steer_raw, ped = 32768, {1: 255, 2: 255, 5: 255}   # throttle, brake, clutch (rest = released)
    hat_x = hat_y = 0
    keys = {}
    client = None
    last_rx = 0.0
    last_ffb = 0.0
    ffb_active = False
    seq = 0
    t0 = time.monotonic()
    next_tick = t0
    ack = ACK.pack(CAP_FFB | CAP_SHIFTER, WHEEL_RANGE_DEGREES, dev.name.encode()[:32])

    def send(addr, typ, payload):
        nonlocal seq
        seq = (seq + 1) & 0xFFFF
        try:
            sock.sendto(HDR.pack(MAGIC, VERSION, typ, seq) + payload, addr)
        except OSError:
            pass

    try:
        while True:
            now = time.monotonic()
            r, _, _ = select.select([dev.fd, sock], [], [], max(0.0, next_tick - now))

            if dev.fd in r:
                for e in dev.read():
                    if e.type == ecodes.EV_ABS:
                        if e.code == ecodes.ABS_X:
                            steer_raw = e.value
                        elif e.code in ped:
                            ped[e.code] = e.value
                        elif e.code == ecodes.ABS_HAT0X:
                            hat_x = e.value
                        elif e.code == ecodes.ABS_HAT0Y:
                            hat_y = e.value
                    elif e.type == ecodes.EV_KEY:
                        keys[e.code] = e.value

            if sock in r:
                while True:
                    try:
                        data, addr = sock.recvfrom(512)
                    except BlockingIOError:
                        break
                    if data[:4] == CMD_MAGIC:
                        if addr[0] == "127.0.0.1":
                            reply = command(data[4:].decode(errors="ignore").strip(), steer_raw, client)
                            if reply.startswith("centre"):
                                ffb.last["spring"] = None   # the spring's centre moved: send it again
                            try:
                                sock.sendto(reply.encode(), addr)
                            except OSError:
                                pass
                        continue
                    if len(data) < HDR.size:
                        continue
                    magic, ver, typ, _s = HDR.unpack_from(data)
                    if magic != MAGIC:
                        continue
                    body = data[HDR.size:]
                    now = time.monotonic()
                    if typ == T_HELLO:
                        if client != addr:
                            log("game connected from", addr[0], "(its protocol version %d)" % ver)
                            ffb.stop()   # the idle forces end; the game's take over
                        client = addr
                        last_rx = now
                        send(addr, T_HELLO_ACK, ack)
                    elif typ == T_BYE and addr == client:
                        log("game disconnected")
                        client = None
                        ffb.stop()
                        ffb_active = False
                    elif typ == T_FFB and addr == client and len(body) >= FFB.size:
                        (speed, brake, abs_l, steer_f, jolt, traction, under, over,
                         flags, spring, const) = FFB.unpack_from(body)
                        last_rx = last_ffb = now
                        ffb_active = True
                        if flags & 1:    # wheel force enabled in the game
                            vib = max(abs_l * 0.5, traction * 0.3, jolt)
                            ffb.apply(spring, const, vib, now)
                        else:
                            ffb.apply(0, 0, 0, now)

            now = time.monotonic()
            if now >= next_tick:
                next_tick += 1.0 / STATE_HZ
                if next_tick < now:
                    next_tick = now + 1.0 / STATE_HZ
                if client and now - last_rx > CLIENT_TIMEOUT:
                    log("game stopped answering, forgetting it")
                    client = None
                    ffb.stop()
                    ffb_active = False
                if ffb_active and now - last_ffb > FFB_TIMEOUT:
                    ffb.stop()
                    ffb_active = False
                if client is None and IDLE_FORCES:
                    spring, vib = idle_forces(norm_steer(steer_raw))
                    ffb.apply(spring, 0, vib, now)
                if client:
                    send(client, T_STATE, STATE.pack(
                        int((now - t0) * 1000) & 0xFFFFFFFF,
                        norm_steer(steer_raw), norm_pedal(ped[1]), norm_pedal(ped[2]), norm_pedal(ped[5]),
                        buttons_from_keys(keys), hat_x, hat_y, gear_from_keys(keys), 0))
    finally:
        try:
            ffb.stop()
        except OSError:
            pass


def main():
    load_center()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", PORT))
    sock.setblocking(False)
    while True:
        dev = find_wheel()
        if dev is None:
            log("no G920 found, retrying in 3 seconds")
            time.sleep(3)
            continue
        try:
            run(dev, sock)
        except OSError as e:
            log("wheel connection lost:", e)
            time.sleep(2)
        except KeyboardInterrupt:
            log("stopped")
            return


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
