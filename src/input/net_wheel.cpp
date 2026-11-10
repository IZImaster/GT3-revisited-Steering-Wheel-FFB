#include "input/net_wheel.h"

#include "base/log.h"

#include <SDL3/SDL_endian.h>

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t_;
#define NW_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef socklen_t socklen_t_;
#define NW_CLOSE ::close
#endif

static_assert(SDL_BYTEORDER == SDL_LIL_ENDIAN, "the network wheel protocol is little-endian and read as is");

namespace gt3r {

namespace {

constexpr uint8_t kVersion = 1;
constexpr uint8_t kHello = 1, kHelloAck = 2, kState = 3, kForce = 4, kBye = 5;
constexpr size_t kHeader = 8, kStateSize = 28, kAckSize = 35, kForceSize = 37;
constexpr double kHelloEvery = 1.0, kLostAfter = 2.0;

// The virtual wheel: axes 0 steering, 1 gas, 2 brake, 3 clutch (the wheel layout's order, pedals
// resting at +1); buttons below; one hat.
constexpr int kAxes = 4, kButtons = 19, kHats = 1;
// Server button bit -> virtual button. A, B, X, Y, right paddle, left paddle keep their numbers
// (the wheel layout's R1 = 4, L1 = 5, Circle = 1, Triangle = 3), View = 8, Menu = 9 (Select, Start),
// then Xbox 6, LSB 10, RSB 11, shifter reverse 12 and gears 1..6 as 13..18.
constexpr int kButtonMap[18] = {0, 1, 2, 3, 4, 5, 9, 8, 11, 10, 6, 12, 13, 14, 15, 16, 17, 18};

int16_t to_axis(float v) { return int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f); }
// A pedal: 0 released .. 1 floor, as the axis the wheel layout reads (rests at +1, floor at -1).
int16_t to_pedal(float v) { return int16_t(32767.0f - std::clamp(v, 0.0f, 1.0f) * 65535.0f); }

template <class T> T rd(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <class T> void wr(uint8_t*& p, T v) {
    std::memcpy(p, &v, sizeof v);
    p += sizeof v;
}

#if defined(_WIN32)
bool sock_init() {
    static bool ok = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    return ok;
}
#else
bool sock_init() { return true; }
#endif

} // namespace

NetWheel::~NetWheel() {
    if (sock_ >= 0) {
        if (have_ack_ || !broadcast_) send_packet(kBye, nullptr, 0);
        NW_CLOSE(static_cast<decltype(socket(0, 0, 0))>(sock_));
    }
    detach();
}

bool NetWheel::start(const std::string& host, int port) {
    if (!sock_init()) return false;
    auto s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        LOG_WARN("Network wheel: no socket");
        return false;
    }
#if defined(_WIN32)
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof yes);

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    broadcast_ = host.empty() || host == "auto";
    if (broadcast_) a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    else if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
        LOG_WARN("Network wheel: host \"%s\" is not an IPv4 address (use auto)", host.c_str());
        NW_CLOSE(s);
        return false;
    }
    static_assert(sizeof a <= sizeof target_, "target_ too small");
    std::memcpy(target_, &a, sizeof a);
    sock_ = intptr_t(s);
    LOG_INFO("Network wheel: looking for the server (%s, port %d)", broadcast_ ? "auto" : host.c_str(), port);
    return true;
}

bool NetWheel::valid_host(const std::string& host) {
    if (host.empty() || host == "auto") return true;
    in_addr a{};
    return inet_pton(AF_INET, host.c_str(), &a) == 1;
}

std::string NetWheel::server_address() const {
    if (!have_ack_) return "";
    sockaddr_in a;
    std::memcpy(&a, target_, sizeof a);
    char buf[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &a.sin_addr, buf, sizeof buf)) return "";
    return buf;
}

void NetWheel::send_packet(uint8_t type, const void* body, size_t n) {
    if (sock_ < 0) return;
    uint8_t buf[64];
    uint8_t* p = buf;
    std::memcpy(p, "GT3W", 4);
    p += 4;
    wr<uint8_t>(p, kVersion);
    wr<uint8_t>(p, type);
    wr<uint16_t>(p, seq_++);
    if (n && n <= sizeof buf - kHeader) {
        std::memcpy(p, body, n);
        p += n;
    }
    sockaddr_in a;
    std::memcpy(&a, target_, sizeof a);
    sendto(static_cast<decltype(socket(0, 0, 0))>(sock_), reinterpret_cast<const char*>(buf), int(p - buf), 0,
           reinterpret_cast<const sockaddr*>(&a), sizeof a);
}

void NetWheel::attach(const std::string& name) {
    if (id_) return;
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_WHEEL;
    d.naxes = kAxes;
    d.nbuttons = kButtons;
    d.nhats = kHats;
    device_name_ = name.empty() ? "Network wheel" : name + " (network)";
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

void NetWheel::apply_state(const uint8_t* p) {
    // ms u32, steer, throttle, brake, clutch f32, buttons u32, hat x, hat y, gear i8, pad u8
    if (!joy_) return;
    SDL_SetJoystickVirtualAxis(joy_, 0, to_axis(rd<float>(p + 4)));
    SDL_SetJoystickVirtualAxis(joy_, 1, to_pedal(rd<float>(p + 8)));
    SDL_SetJoystickVirtualAxis(joy_, 2, to_pedal(rd<float>(p + 12)));
    SDL_SetJoystickVirtualAxis(joy_, 3, to_pedal(rd<float>(p + 16)));
    const uint32_t bits = rd<uint32_t>(p + 20);
    for (int i = 0; i < 18; ++i) SDL_SetJoystickVirtualButton(joy_, kButtonMap[i], (bits >> i) & 1);
    const int8_t hx = int8_t(p[24]), hy = int8_t(p[25]);
    uint8_t hat = SDL_HAT_CENTERED;
    if (hx < 0) hat |= SDL_HAT_LEFT;
    if (hx > 0) hat |= SDL_HAT_RIGHT;
    if (hy < 0) hat |= SDL_HAT_UP;
    if (hy > 0) hat |= SDL_HAT_DOWN;
    SDL_SetJoystickVirtualHat(joy_, 0, hat);
}

void NetWheel::poll(double dt) {
    if (sock_ < 0) return;
    since_hello_ += dt;
    since_rx_ += dt;
    if (since_hello_ >= kHelloEvery) {
        since_hello_ = 0;
        const uint8_t caps = 1; // wants force feedback
        send_packet(kHello, &caps, 1);
    }
    for (;;) {
        uint8_t buf[256];
        sockaddr_in from{};
        socklen_t_ fl = sizeof from;
        auto n = recvfrom(static_cast<decltype(socket(0, 0, 0))>(sock_), reinterpret_cast<char*>(buf), sizeof buf, 0,
                          reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) break;
        if (size_t(n) < kHeader || std::memcmp(buf, "GT3W", 4) != 0) continue;
        const uint8_t ver = buf[4], type = buf[5];
        const uint16_t seq = rd<uint16_t>(buf + 6);
        if (ver != kVersion) {
            static bool warned = false;
            if (!warned) LOG_WARN("Network wheel: server speaks protocol version %d, this game %d", int(ver), int(kVersion));
            warned = true;
            continue;
        }
        const uint8_t* body = buf + kHeader;
        const size_t len = size_t(n) - kHeader;
        if (type == kHelloAck && len >= kAckSize) {
            if (broadcast_ && !have_ack_) { // the server found: talk to it alone from now on
                std::memcpy(target_, &from, sizeof from);
                broadcast_ = false;
            }
            have_ack_ = true;
            have_seq_ = false; // a restarted server counts from 0 again
            since_rx_ = 0;
            char name[33] = {};
            std::memcpy(name, body + 3, 32);
            attach(name);
        } else if (type == kState && len >= kStateSize && joy_) {
            if (have_seq_ && int16_t(uint16_t(seq - last_seq_)) <= 0) continue; // old or repeated
            have_seq_ = true;
            last_seq_ = seq;
            since_rx_ = 0;
            apply_state(body);
        }
    }
    if (id_ && since_rx_ > kLostAfter) {
        detach();
        have_ack_ = false;
        have_seq_ = false;
    }
}

void NetWheel::send_force(const NetWheelForce& f) {
    if (!have_ack_) return;
    uint8_t body[kForceSize];
    uint8_t* p = body;
    wr<float>(p, f.speed_kmh);
    wr<float>(p, f.brake);
    wr<float>(p, f.abs_level);
    wr<float>(p, f.steer_force);
    wr<float>(p, f.jolt);
    wr<float>(p, f.traction);
    wr<float>(p, f.understeer);
    wr<float>(p, f.oversteer);
    wr<uint8_t>(p, f.wheel_on ? 1 : 0);
    wr<int16_t>(p, int16_t(std::clamp(f.spring, 0, 32767)));
    wr<int16_t>(p, int16_t(std::clamp(f.constant, -32767, 32767)));
    send_packet(kForce, body, sizeof body);
}

} // namespace gt3r
