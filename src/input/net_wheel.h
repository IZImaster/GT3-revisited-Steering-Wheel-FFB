#pragma once

// Network wheel: a steering wheel on another machine, shown to the game as a virtual SDL wheel.
//
// A NetXWheel server next to the wheel (https://github.com/IZImaster/NetXWheel, e.g. a Raspberry Pi
// with a Logitech G920) reads it and streams its state over UDP (input/netxwheel.h: finding
// servers, passwords, the session). This turns the connected wheel into a virtual joystick
// (SDL_AttachVirtualJoystick, type wheel), so the game's own wheel detection, bindings and steering
// path work unchanged, and sends the race's forces back.
//
// Home > Network wheel (app/screen_netwheel.cpp) picks the server and the wheel; the choice is
// kept in [netwheel] in config.ini and connected again at the next start:
//
//   enabled = off          ; connect at start
//   host = 192.168.1.50    ; the server's last address
//   port = 27920
//   server_id = ...        ; finds the server again if its address changes
//   server_name = ...
//   wheel = 1
//   server_password = ...  ; (stored as typed: config.ini is in the app's own storage)
//   wheel_password = ...
//
// The virtual wheel appears while connected and goes away when the session ends (hot-plug).

#include "input/netxwheel.h"

#include <SDL3/SDL_joystick.h>

#include <cstdint>
#include <string>

namespace gt3r {

// What the race feeds the wheel each frame (a ForceFeedbackState with the wheel effects computed).
struct NetWheelForce {
    float speed_kmh = 0, brake = 0, abs_level = 0, steer_force = 0, jolt = 0, traction = 0, understeer = 0, oversteer = 0;
    bool wheel_on = true;
    int spring = 0;   // 0..32767
    int constant = 0; // -32767..32767, positive pushes right
};

class NetWheel {
public:
    NetWheel() = default;
    ~NetWheel();
    NetWheel(const NetWheel&) = delete;
    NetWheel& operator=(const NetWheel&) = delete;

    bool start();                            // the socket; false: no network
    void poll(double dt);                    // once per frame: receive, keep alive, update the virtual wheel
    void send_force(const NetWheelForce& f); // every frame while driving

    void connect(const netxwheel::Target& t) { client_.connect(t); }
    void disconnect() { client_.disconnect(); }
    netxwheel::Client& client() { return client_; }
    const netxwheel::Client& client() const { return client_; }

    SDL_JoystickID joystick_id() const { return id_; }
    bool connected() const { return joy_ != nullptr; } // the virtual wheel is there
    const std::string& device_name() const { return device_name_; }
    std::string status_text() const;                   // one line for Settings and the Network wheel page

    static bool valid_host(const std::string& host);   // an IPv4 address
    static uint32_t parse_ip(const std::string& host); // host order; 0 if not an IPv4 address

private:
    void attach();
    void detach();
    void apply_state(const netxwheel::WheelState& s);

    netxwheel::Client client_;
    SDL_JoystickID id_ = 0;
    SDL_Joystick* joy_ = nullptr;
    std::string device_name_;
};

} // namespace gt3r
