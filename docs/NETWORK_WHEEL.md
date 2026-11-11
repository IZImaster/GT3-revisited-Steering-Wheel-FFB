# Network wheel

Play with a steering wheel that is plugged into another machine (for example a Logitech G920 on a
Raspberry Pi), including force feedback, over the local network. Useful where the wheel cannot be
plugged into the device that runs the game, such as a standalone Meta Quest.

A small server next to the wheel, **[NetXWheel](https://github.com/IZImaster/NetXWheel)**, reads the
wheel and streams its state over UDP (its [protocol version 1](https://github.com/IZImaster/NetXWheel/blob/main/PROTOCOL.md):
several servers per network, several wheels per server, passwords, signed sessions). The game shows
the connected wheel as a virtual SDL wheel, so the wheel layout, bindings, steering path and force
feedback settings (`[ffb]`) work as for any wheel. The race's spring and steering force go back to
the server, which plays them on the wheel.

- `src/input/netxwheel.*`: the protocol client (no SDL): finding servers, passwords, the session.
  `tests/test_netxwheel.cpp` checks it against values from the server and a server in the test.
- `src/input/net_wheel.*`: the virtual wheel, and the race's forces.
- `src/app/screen_netwheel.cpp`: **Home > Network wheel**.

## Choosing the wheel: Home > Network wheel

1. The servers on the network are listed by name (with their address, number of wheels, and
   whether they need a password). A server the search doesn't reach: "Type a server's address".
2. Choose a server (its password, if it has one), then a wheel: make, model, rotation, full lock,
   force feedback, direct drive, torque, pedals, shifter, buttons, and whether it is free.
3. Choose the wheel to connect (its own password, if it has one). Square, or "Rumble the connected
   wheel", makes it vibrate for two seconds, to tell which one it is.

Settings > Controls > Network wheel switches it on and off and shows the status.

## `config.ini`

Saved when a wheel is chosen; the game connects to it again at the next start:

```ini
[netwheel]
enabled = on              ; connect at start (Disconnect switches it off)
host = 192.168.1.50       ; the server's last address
port = 27920
server_id = 6f1c2a90e4b84c31  ; finds the server again if its address changes
server_name = Living room Pi
wheel = 1
server_password =         ; as typed (config.ini is in the app's own storage)
wheel_password =
```

## The virtual wheel

Axes 0 steering, 1 gas, 2 brake, 3 clutch, 4 handbrake (pedals resting at +1, as the wheel layout
reads them). Buttons 0 A, 1 B, 2 X, 3 Y, 4 right paddle, 5 left paddle, 6 Home (Xbox), 8 View,
9 Menu, 10 LSB, 11 RSB, 12 shifter reverse, 13..19 shifter gears 1..7, 20 L2, 21 R2, 22 +, 23 -,
24 Enter, 25 shift up, 26 shift down, 27..58 the wheel's extra buttons; hat 0 the D-pad.
