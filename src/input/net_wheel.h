#pragma once

// Network wheel: a steering wheel on another machine, shown to the game as a virtual SDL wheel.
//
// A small server next to the wheel (tools/netwheel/wheel_server.py, a Raspberry Pi with a Logitech
// G920) reads it and streams its state over UDP; this client turns that into a virtual joystick
// (SDL_AttachVirtualJoystick, type wheel), so the game's own wheel detection, bindings and steering
// path work unchanged, and sends the race's force feedback back. Protocol: docs/NETWORK_WHEEL.md.
//
// Configuration, [netwheel] in config.ini:
//
//   enabled = off      ; master switch
//   host    = auto     ; the server's address, or auto: look for it on the local network (broadcast)
//   port    = 27920
//
// The virtual wheel appears while the server answers and goes away when it stops (hot-plug).
// Settings > Controls > Network wheel switches it and sets the host (InputManager::set_netwheel).

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

    bool start(const std::string& host, int port); // false: no socket
    void poll(double dt);                          // once per frame: receive, keep alive, update the virtual wheel
    void send_force(const NetWheelForce& f);       // every frame while driving
    SDL_JoystickID joystick_id() const { return id_; }
    bool connected() const { return joy_ != nullptr; }           // the virtual wheel is there
    const std::string& device_name() const { return device_name_; }
    std::string server_address() const;                          // the server's IPv4 once found, else ""
    static bool valid_host(const std::string& host);             // auto (or empty), or an IPv4 address

private:
    void attach(const std::string& name);
    void detach();
    void apply_state(const uint8_t* p);
    void send_packet(uint8_t type, const void* body, size_t n);

    intptr_t sock_ = -1;
    uint8_t target_[32] = {}; // sockaddr_in of where packets go
    bool broadcast_ = false;
    bool have_ack_ = false;
    double since_hello_ = 1e9, since_rx_ = 1e9;
    uint16_t seq_ = 0, last_seq_ = 0;
    bool have_seq_ = false;
    SDL_JoystickID id_ = 0;
    SDL_Joystick* joy_ = nullptr;
    std::string device_name_;
};

} // namespace gt3r
