#pragma once

// NetXWheel client (protocol version 1, https://github.com/IZImaster/NetXWheel/blob/main/PROTOCOL.md):
// finds wheel servers on the local network, connects to one of their wheels (with passwords),
// receives the wheel's state and sends forces. No SDL here: net_wheel.h turns it into a virtual
// wheel for the game, and the tests run it against a server in the same process.

#include "base/bytes.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace gt3r::netxwheel {

constexpr u16 kDefaultPort = 27920;

// One wheel as a server describes it (ANNOUNCE; ACCEPT adds buttons, forces, extra axes).
struct WheelInfo {
    struct Button {
        int bit = 0;
        std::string role, label;
    };
    int index = 0;
    std::string vendor, model;
    int rotation = 0, steer_range = 0; // degrees, lock-to-lock
    bool ffb = false, direct_drive = false;
    float torque_nm = 0; // 0: unknown
    std::vector<std::string> pedals, forces, extra_axes;
    std::string shifter; // none, h6r, h7r, h6, h7, sequential
    bool password = false, in_use = false, connected = true;
    std::string in_use_by;
    std::vector<Button> buttons;
    bool dpad = false;

    std::string title() const; // "Logitech G920 Driving Force"
};

struct ServerInfo {
    u32 ip = 0; // host order
    u16 port = kDefaultPort;
    std::string address; // "192.168.1.50"
    std::string server_id, name, software;
    bool password = false;
    std::vector<WheelInfo> wheels;
    double seen = 0; // when it last answered (Client::now)
};

// The wheel's state (STATE).
struct WheelState {
    float steer = 0, throttle = 0, brake = 0, clutch = 0, handbrake = 0; // steer -1..1, the rest 0..1
    u64 buttons = 0;
    u8 dpad = 0; // 1 up, 2 right, 4 down, 8 left
    s8 gear = 0; // -1 reverse, 0 neutral, 1..7
    u8 status = 0;
    std::vector<float> extra;
    bool unplugged() const { return status & 2; }
};

// What the game wants the wheel to play (FORCES).
struct Forces {
    bool on = false;
    float constant = 0, spring = 0, spring_center = 0, damper = 0, friction = 0, rumble = 0, rumble_hz = 0;
};

// Where to connect, and the passwords (empty: none).
struct Target {
    u32 ip = 0; // host order; 0: find the server by server_id
    u16 port = kDefaultPort;
    int wheel = 1;
    std::string server_password, wheel_password;
    std::string server_id, server_name; // to find it again if its address changes; for display
};

class Client {
public:
    enum class Status : u8 {
        Idle,         // not connected, not trying
        Connecting,   // asking the server
        Connected,    // driving
        Reconnecting, // lost it, or the wheel is busy: trying again by itself
        Failed,       // stopped trying: a wrong password, the owner ended it... (message())
    };

    Client() = default;
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool open();                   // the socket; false if there is none
    bool is_open() const { return sock_ >= 0; }
    void close();

    // Finding servers: send DISCOVER to the local network (and to `extra`, e.g. a typed address);
    // the answers fill servers() as poll() reads them.
    void discover(const std::vector<std::pair<u32, u16>>& extra = {});
    const std::vector<ServerInfo>& servers() const { return servers_; }
    void forget_servers() { servers_.clear(); }

    void connect(const Target& t);
    void disconnect(); // ends the session (BYE); stops trying
    const Target& target() const { return target_; }

    // Once per frame: reads what arrived, keeps the session alive, retries.
    void poll();

    // The game's forces, every frame while it wants them (on = false or no calls: the wheel is limp).
    void send_forces(const Forces& f);
    // Vibrates the connected wheel for a while (to tell which one it is).
    void identify(double seconds = 2.0);
    bool identifying() const { return now() < identify_until_; }

    Status status() const { return status_; }
    const std::string& message() const { return message_; } // why it failed / what it waits for
    bool connected() const { return status_ == Status::Connected; }
    const WheelInfo& wheel() const { return wheel_; } // the connected wheel (ACCEPT)
    std::string server_address() const;
    // The newest state since the last call; false if none arrived.
    bool take_state(WheelState& out);
    // True once after the wheel's description changed or it was unplugged / plugged back.
    bool take_wheel_changed() { return std::exchange(wheel_changed_, false); }

    static double now(); // seconds, monotonic

    // The protocol's pieces (also for the tests).
    static Bytes packet(u8 type, ByteSpan body, u32 session, u32 seq, const u8* key /* 32 bytes or null */);
    static std::string hex(ByteSpan b);
    static Bytes unhex(const std::string& s);
    static std::array<u8, 32> derive_key(const std::string& server_pw, const std::string& wheel_pw,
                                         const std::string& salt_hex, int iterations);
    static std::string proof(const std::array<u8, 32>& key, const std::string& nonce, const std::string& client_nonce,
                             int wheel);
    static std::array<u8, 32> session_key(const std::array<u8, 32>& key, const std::string& nonce,
                                          const std::string& client_nonce);

private:
    void send_to(u32 ip, u16 port, const Bytes& data);
    void send_setup(u8 type, const std::string& json);
    void send_session(u8 type, ByteSpan body = {});
    void handle(const u8* data, size_t n, u32 ip, u16 port);
    void start_handshake();
    void lost(const std::string& why, bool retry);
    void fail(const std::string& why);

    intptr_t sock_ = -1;
    std::vector<ServerInfo> servers_;
    Target target_;
    Status status_ = Status::Idle;
    std::string message_;
    // the handshake
    std::string client_nonce_, server_nonce_;
    std::array<u8, 32> key_{}, pending_session_key_{};
    bool proving_ = false;
    double handshake_at_ = 0, retry_at_ = 0;
    int attempts_ = 0;
    std::string key_cache_id_;
    std::array<u8, 32> key_cache_{};
    // the session
    u32 session_ = 0, seq_out_ = 0, seq_in_ = 0;
    std::array<u8, 32> session_key_{};
    double last_rx_ = 0, last_tx_ = 0;
    WheelInfo wheel_;
    WheelState state_;
    bool have_state_ = false, wheel_changed_ = false;
    // forces
    Forces game_forces_;
    double game_forces_at_ = -1, forces_sent_at_ = 0, identify_until_ = 0;
    bool forces_were_on_ = false;
};

} // namespace gt3r::netxwheel
