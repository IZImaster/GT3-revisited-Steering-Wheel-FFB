#include "input/net_wheel.h"

#include "base/log.h"

#include <algorithm>
#include <cstdio>

namespace gt3r {

namespace {

// The virtual wheel: axes 0 steering, 1 gas, 2 brake, 3 clutch, 4 handbrake (the wheel layout's
// order, pedals resting at +1); buttons below; one hat.
constexpr int kAxes = 5, kButtons = 59, kHats = 1;
// Protocol button bit (0..17) -> virtual button. A, B, X, Y, right paddle, left paddle keep their
// numbers (the wheel layout's R1 = 4, L1 = 5, Circle = 1, Triangle = 3), View = 8, Menu = 9 (Select,
// Start), Home 6, LSB 10, RSB 11; L2, R2, +, -, Enter, shift up / down 20..26.
constexpr int kButtonMap[18] = {0, 1, 2, 3, 4, 5, 9, 8, 11, 10, 6, 20, 21, 22, 23, 24, 25, 26};
constexpr int kReverse = 12, kFirstGear = 13; // H-shifter: reverse 12, gears 1..7 as 13..19
constexpr int kFirstExtra = 27;               // extra buttons (bits 32..63) as 27..58

int16_t to_axis(float v) { return int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f); }
// A pedal: 0 released .. 1 floor, as the axis the wheel layout reads (rests at +1, floor at -1).
int16_t to_pedal(float v) { return int16_t(32767.0f - std::clamp(v, 0.0f, 1.0f) * 65535.0f); }

} // namespace

NetWheel::~NetWheel() {
    client_.close();
    detach();
}

bool NetWheel::start() { return client_.open(); }

bool NetWheel::valid_host(const std::string& host) { return parse_ip(host) != 0; }

uint32_t NetWheel::parse_ip(const std::string& host) {
    unsigned a, b, c, d;
    char tail;
    if (std::sscanf(host.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 ||
        d > 255)
        return 0;
    return (a << 24) | (b << 16) | (c << 8) | d;
}

void NetWheel::poll(double) {
    client_.poll();
    if (client_.connected() && !joy_) attach();
    if (!client_.connected() && joy_) detach();
    if (joy_ && client_.take_wheel_changed() && device_name_ != client_.wheel().title() + " (network)") {
        detach(); // the description changed (another wheel model): the game sees it plugged in again
        attach();
    }
    netxwheel::WheelState s;
    if (joy_ && client_.take_state(s)) apply_state(s);
}

std::string NetWheel::status_text() const {
    using S = netxwheel::Client::Status;
    const netxwheel::Client& c = client_;
    const netxwheel::Target& t = c.target();
    const std::string where = t.server_name.empty() ? c.server_address() : t.server_name;
    switch (c.status()) {
    case S::Idle: return "Not connected";
    case S::Connecting: return "Connecting to " + where + ", wheel " + std::to_string(t.wheel) + "...";
    case S::Connected:
        return "Connected: " + c.wheel().title() + " (wheel " + std::to_string(c.wheel().index) + " on " + where + ")" +
               (c.wheel().connected ? "" : " - UNPLUGGED at the server");
    case S::Reconnecting: return where + ": " + c.message();
    case S::Failed: return "Not connected: " + c.message();
    }
    return "";
}

void NetWheel::send_force(const NetWheelForce& f) {
    netxwheel::Forces x;
    x.on = f.wheel_on;
    x.constant = float(f.constant) / 32767.0f;
    x.spring = float(f.spring) / 32767.0f;
    x.rumble = std::clamp(std::max({f.abs_level * 0.5f, f.traction * 0.3f, f.jolt}), 0.0f, 1.0f);
    client_.send_forces(x);
}

void NetWheel::attach() {
    if (id_) return;
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_WHEEL;
    d.naxes = kAxes;
    d.nbuttons = kButtons;
    d.nhats = kHats;
    device_name_ = client_.wheel().title() + " (network)";
    d.name = device_name_.c_str();
    id_ = SDL_AttachVirtualJoystick(&d);
    if (id_) joy_ = SDL_OpenJoystick(id_);
    if (!joy_) {
        LOG_WARN("Network wheel: virtual wheel: %s", SDL_GetError());
        detach();
        return;
    }
    // Pedals up before the first state arrives.
    for (int a = 1; a < kAxes; ++a) SDL_SetJoystickVirtualAxis(joy_, a, 32767);
    LOG_INFO("Network wheel: %s connected", device_name_.c_str());
}

void NetWheel::detach() {
    if (joy_) SDL_CloseJoystick(joy_);
    if (id_) SDL_DetachVirtualJoystick(id_);
    joy_ = nullptr;
    if (id_) LOG_INFO("Network wheel: disconnected");
    id_ = 0;
}

void NetWheel::apply_state(const netxwheel::WheelState& s) {
    SDL_SetJoystickVirtualAxis(joy_, 0, to_axis(s.steer));
    SDL_SetJoystickVirtualAxis(joy_, 1, to_pedal(s.throttle));
    SDL_SetJoystickVirtualAxis(joy_, 2, to_pedal(s.brake));
    SDL_SetJoystickVirtualAxis(joy_, 3, to_pedal(s.clutch));
    SDL_SetJoystickVirtualAxis(joy_, 4, to_pedal(s.handbrake));
    for (int bit = 0; bit < 18; ++bit) SDL_SetJoystickVirtualButton(joy_, kButtonMap[bit], (s.buttons >> bit) & 1);
    for (int i = 0; i < 32; ++i) SDL_SetJoystickVirtualButton(joy_, kFirstExtra + i, (s.buttons >> (32 + i)) & 1);
    SDL_SetJoystickVirtualButton(joy_, kReverse, s.gear < 0);
    for (int g = 1; g <= 7; ++g) SDL_SetJoystickVirtualButton(joy_, kFirstGear + g - 1, s.gear == g);
    uint8_t hat = SDL_HAT_CENTERED;
    if (s.dpad & 1) hat |= SDL_HAT_UP;
    if (s.dpad & 2) hat |= SDL_HAT_RIGHT;
    if (s.dpad & 4) hat |= SDL_HAT_DOWN;
    if (s.dpad & 8) hat |= SDL_HAT_LEFT;
    SDL_SetJoystickVirtualHat(joy_, 0, hat);
}

} // namespace gt3r
