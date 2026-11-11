# Network wheel

Play with a steering wheel that is plugged into another machine (for example a Logitech G920 on a
Raspberry Pi), including force feedback, over the local network. Useful where the wheel cannot be
plugged into the device that runs the game, such as a standalone Meta Quest.

A small server next to the wheel, **[NetXWheel](https://github.com/IZImaster/NetXWheel)**, reads the
wheel and streams its state over UDP. The game shows it as a virtual SDL wheel
(`src/input/net_wheel.cpp`), so the wheel layout, bindings, steering path and force feedback settings
(`[ffb]`) work as for any wheel. The race's spring and steering force go back to the server, which
plays them on the wheel. Wheel-specific code lives only in the server (wheel profiles).

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

## Server side

See [NetXWheel](https://github.com/IZImaster/NetXWheel): install, settings, supported wheels.
The protocol (version 1, frozen) is its [PROTOCOL.md](https://github.com/IZImaster/NetXWheel/blob/main/PROTOCOL.md).
