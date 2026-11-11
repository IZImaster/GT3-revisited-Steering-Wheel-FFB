#pragma once

#include <memory>

namespace gt3r {

class Screen;

// Home > Network wheel: the NetXWheel servers on the network, their wheels, passwords, connecting
// and a rumble to tell which wheel is which (input/net_wheel.h).
std::unique_ptr<Screen> make_netwheel_screen();

} // namespace gt3r
