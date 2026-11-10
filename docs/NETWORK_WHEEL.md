# Network wheel

Play with a steering wheel that is plugged into another machine (for example a Logitech G920 on a
Raspberry Pi), including force feedback, over the local network. Useful where the wheel cannot be
plugged into the device that runs the game, such as a standalone Meta Quest.

A small server next to the wheel (`tools/netwheel/wheel_server.py`, Python with `python3-evdev`)
reads the wheel and streams its state over UDP. The game shows it as a virtual SDL wheel
(`src/input/net_wheel.cpp`), so the wheel layout, bindings, steering path and force feedback settings
(`[ffb]`) work as for any wheel. The race's spring and steering force go back to the server, which
plays them on the wheel. Wheel-specific code lives only in the server; other wheels can be added there.

## Game side (Settings, or `config.ini`)

In the game: Settings > Controls > Network wheel. Switch it on; Server address stays Auto unless the
game doesn't find the server, then type its IPv4 address. Status shows whether the wheel is connected.
Changes apply at once and are saved to `config.ini`:

```ini
[netwheel]
enabled = on     ; off by default
host = auto      ; the server's IPv4 address, or auto to look for it on the local network
port = 27920
```

The wheel appears while the server answers and disappears when it stops. Virtual wheel layout:
axes 0 steering, 1 gas, 2 brake, 3 clutch (pedals resting at +1, as the wheel layout reads them);
buttons 0 A, 1 B, 2 X, 3 Y, 4 right paddle, 5 left paddle, 6 Xbox, 8 View, 9 Menu, 10 LSB, 11 RSB,
12 shifter reverse, 13..18 shifter gears 1..6; hat 0 the D-pad.

## Server side (Raspberry Pi, Logitech G920)

```
sudo apt install python3-evdev
sudo python3 tools/netwheel/wheel_server.py
```

The server sets the wheel's rotation (`WHEEL_RANGE_DEGREES`) through the driver's `range` file,
which only root may write by default. To run it without sudo, allow that once, then unplug and
replug the wheel's USB cable:

```
sudo tee /etc/udev/rules.d/99-wheel-range.rules > /dev/null <<'EOF'
ACTION=="bind", SUBSYSTEM=="hid", DRIVER=="logitech-hidpp-device", RUN+="/bin/chmod a+w /sys%p/range"
EOF
sudo udevadm control --reload-rules
```

`wheel_cmd.py` (center, reset, status, watch, buzz) talks to the running server. While no game is
connected the server keeps a centring spring on and vibrates near full lock (`IDLE_*`, `WARN_*`).

Settings are at the top of the file (force gain, direction, pedal polarity). `wheel_testclient.py`
shows the wheel state and plays test forces without the game.

# Protocol (version 1)

UDP, little-endian, one message per datagram. Default server port: **27920** (the Pi).
The game is the client: it sends HELLO to the server and the server answers to that address.

## Protocol: Header (8 bytes, every message)
| Field | Type | Notes |
|---|---|---|
| magic | 4 bytes | `GT3W` |
| version | u8 | sender's protocol version (1) |
| type | u8 | 1 HELLO, 2 HELLO_ACK, 3 STATE, 4 FFB, 5 BYE |
| seq | u16 | sender's counter; receiver drops STATE older than the last one seen (wrap-aware) |

## Protocol: Session
1. Game sends **HELLO** (payload: u8 client capabilities, bit 0 = wants force feedback) about once a second. It doubles as the keep-alive.
2. Server answers **HELLO_ACK**: u8 capabilities (bit 0 force feedback, bit 1 H-shifter), u16 steering range in degrees, 32-byte zero-padded device name.
3. While a game is connected the server streams **STATE** at 100 Hz.
4. Game sends **FFB** every frame while racing. If the server gets none for 300 ms it stops all forces (wheel goes limp, as the game's own devices do in menus).
5. If the server hears nothing at all for 3 seconds it forgets the game and stops sending. Game sends **BYE** on exit.
6. Only one game at a time; a HELLO from a new address replaces the old one.
A game that sees a server version it does not support should disconnect.

## Protocol: STATE (server to game), 28 bytes after the header
| Field | Type | Notes |
|---|---|---|
| ms | u32 | server milliseconds since start |
| steering | f32 | -1 (full left) .. +1 (full right) |
| throttle | f32 | 0 released .. 1 floor |
| brake | f32 | 0 .. 1 |
| clutch | f32 | 0 .. 1 |
| buttons | u32 | bit mask, see below |
| hat_x | i8 | -1 left, +1 right |
| hat_y | i8 | -1 up, +1 down |
| gear | i8 | -1 reverse, 0 neutral, 1..6 (H-shifter) |
| pad | u8 | 0 |

Button bits: 0 A, 1 B, 2 X, 3 Y, 4 right paddle, 5 left paddle, 6 menu, 7 view, 8 RSB, 9 LSB, 10 Xbox, 11 shifter reverse, 12..17 shifter gears 1..6.

## Protocol: FFB (game to server), 37 bytes after the header
The first eight fields are the game's `ForceFeedbackState`, for devices that want to build their own effects. The last two are what the game's own wheel code would play.
| Field | Type | Notes |
|---|---|---|
| speed_kmh | f32 | |
| brake | f32 | 0..1 |
| abs_level | f32 | 0..1 |
| steer_force | f32 | -1..1, positive turns the wheel right |
| jolt | f32 | 0..1 impact this frame |
| traction | f32 | 0..1 |
| understeer | f32 | 0..1 |
| oversteer | f32 | 0..1 |
| flags | u8 | bit 0: wheel force enabled in the game |
| spring | i16 | `ffb::wheel_spring_coefficient`, 0..32767 |
| constant | i16 | `ffb::wheel_constant_level`, -32767..32767, positive pushes right |
