# The network wheel server moved: NetXWheel

The server for the network wheel (Raspberry Pi / Linux / Windows), its tools and the protocol now
live in their own project: **https://github.com/IZImaster/NetXWheel**

Install on a Raspberry Pi:

```
sudo apt install -y git
git clone https://github.com/IZImaster/NetXWheel.git ~/NetXWheel
cd ~/NetXWheel && ./install.sh
```

The game side stays here: `src/input/net_wheel.*` and Settings > Controls > Network wheel.
