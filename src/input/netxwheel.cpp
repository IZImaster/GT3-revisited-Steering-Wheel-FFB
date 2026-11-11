#include "input/netxwheel.h"

#include "base/sha256.h"
#include "formats/json.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX // (windows.h would make min and max macros)
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#define NXW_CLOSE closesocket
using nxw_socklen = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define NXW_CLOSE ::close
using nxw_socklen = socklen_t;
#endif

namespace gt3r::netxwheel {

namespace {

constexpr u8 kDiscover = 1, kAnnounce = 2, kConnect = 3, kChallenge = 4, kAccept = 5, kReject = 6;
constexpr u8 kState = 16, kForces = 17, kKeepalive = 18, kNotice = 19, kBye = 20;
constexpr size_t kHeader = 16, kSig = 16, kStateSize = 36;
constexpr double kLostAfter = 2.0, kKeepEvery = 0.5, kRetryEvery = 2.0, kForcesEvery = 1.0 / 60, kAnswerWait = 1.5;

#if defined(_WIN32)
bool sock_init() {
    static bool ok = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    return ok;
}
#else
bool sock_init() { return true; }
#endif

using sock_t = decltype(socket(0, 0, 0));

ByteSpan text_span(const std::string& s) { return ByteSpan(reinterpret_cast<const u8*>(s.data()), s.size()); }

template <class T> void put(Bytes& b, T v) {
    const size_t at = b.size();
    b.resize(at + sizeof v);
    std::memcpy(b.data() + at, &v, sizeof v);
}
template <class T> T get(const u8* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

// A JSON string literal.
std::string q(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += char(c);
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else out += char(c);
    }
    return out + "\"";
}

std::string random_hex(size_t bytes) {
    static std::random_device rd;
    Bytes b(bytes);
    for (u8& x : b) x = u8(rd());
    return Client::hex(b);
}

bool is_hex(const std::string& s, size_t len) {
    return s.size() == len && std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(u8(c)); });
}

std::string ip_text(u32 ip) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return buf;
}

std::vector<std::string> strings(const json::Value& v) {
    std::vector<std::string> out;
    for (const json::Value& x : v.items())
        if (x.is_string()) out.push_back(x.string());
    return out;
}

WheelInfo parse_wheel(const json::Value& w) {
    WheelInfo i;
    i.index = w["index"].integer();
    i.vendor = w["vendor"].string();
    i.model = w["model"].string();
    i.rotation = w["rotation"].integer();
    i.steer_range = w["steer_range"].integer();
    i.ffb = w["ffb"].boolean();
    i.direct_drive = w["direct_drive"].boolean();
    i.torque_nm = float(w["torque_nm"].number());
    i.pedals = strings(w["pedals"]);
    i.forces = strings(w["forces"]);
    i.shifter = w["shifter"].string();
    i.password = w["password"].boolean();
    i.in_use = w["in_use"].boolean();
    i.in_use_by = w["in_use_by"].string();
    i.connected = w["connected"].boolean(true);
    i.dpad = w["dpad"].boolean();
    for (const json::Value& b : w["buttons"].items())
        i.buttons.push_back({b["bit"].integer(), b["role"].string(), b["label"].string()});
    for (const json::Value& a : w["extra_axes"].items()) i.extra_axes.push_back(a["label"].string());
    return i;
}

bool parse_json(const u8* p, size_t n, json::Value& out) {
    try {
        out = json::Value::parse(std::string_view(reinterpret_cast<const char*>(p), n));
        return out.is_object();
    } catch (const std::exception&) {
        return false;
    }
}

float clamp_f(float v, float lo, float hi) { return std::isfinite(v) ? std::clamp(v, lo, hi) : 0.0f; }

} // namespace

// ------------------------------------------------------------------ pieces
std::string WheelInfo::title() const {
    if (vendor.empty()) return model.empty() ? "Wheel " + std::to_string(index) : model;
    return vendor + " " + model;
}

double Client::now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string Client::hex(ByteSpan b) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (u8 x : b) s += k[x >> 4], s += k[x & 15];
    return s;
}

Bytes Client::unhex(const std::string& s) {
    Bytes out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(u8(std::stoi(s.substr(i, 2), nullptr, 16)));
    return out;
}

std::array<u8, 32> Client::derive_key(const std::string& server_pw, const std::string& wheel_pw,
                                      const std::string& salt_hex, int iterations) {
    const std::string secret = server_pw + "\n" + wheel_pw;
    const Bytes salt = unhex(salt_hex);
    const Bytes k = pbkdf2_sha256(text_span(secret), salt, std::clamp(iterations, 1, 1000000), 32);
    std::array<u8, 32> out;
    std::copy(k.begin(), k.end(), out.begin());
    return out;
}

std::string Client::proof(const std::array<u8, 32>& key, const std::string& nonce, const std::string& client_nonce,
                          int wheel) {
    const std::string msg = "proof" + nonce + client_nonce + std::to_string(wheel);
    return hex(hmac_sha256(key, text_span(msg)));
}

std::array<u8, 32> Client::session_key(const std::array<u8, 32>& key, const std::string& nonce,
                                       const std::string& client_nonce) {
    return hmac_sha256(key, text_span("session" + nonce + client_nonce));
}

Bytes Client::packet(u8 type, ByteSpan body, u32 session, u32 seq, const u8* key) {
    Bytes b = {'N', 'X', 'W', '1'};
    put<u8>(b, type);
    put<u8>(b, key ? 1 : 0);
    put<u16>(b, 0);
    put<u32>(b, session);
    put<u32>(b, seq);
    b.insert(b.end(), body.begin(), body.end());
    if (key) {
        const std::array<u8, 32> sig = hmac_sha256(ByteSpan(key, 32), b);
        b.insert(b.end(), sig.begin(), sig.begin() + kSig);
    }
    return b;
}

// ------------------------------------------------------------------ the socket
Client::~Client() { close(); }

bool Client::open() {
    if (sock_ >= 0) return true;
    if (!sock_init()) return false;
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;
#if defined(_WIN32)
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof yes);
    sock_ = intptr_t(s);
    return true;
}

void Client::close() {
    if (sock_ < 0) return;
    disconnect();
    NXW_CLOSE(static_cast<sock_t>(sock_));
    sock_ = -1;
}

void Client::send_to(u32 ip, u16 port, const Bytes& data) {
    if (sock_ < 0 || !ip) return;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    sendto(static_cast<sock_t>(sock_), reinterpret_cast<const char*>(data.data()), int(data.size()), 0,
           reinterpret_cast<const sockaddr*>(&a), sizeof a);
}

void Client::send_setup(u8 type, const std::string& json) {
    send_to(target_.ip, target_.port, packet(type, text_span(json), 0, 0, nullptr));
    last_tx_ = now();
}

void Client::send_session(u8 type, ByteSpan body) {
    send_to(target_.ip, target_.port, packet(type, body, session_, ++seq_out_, session_key_.data()));
    last_tx_ = now();
}

std::string Client::server_address() const { return target_.ip ? ip_text(target_.ip) : std::string(); }

// ------------------------------------------------------------------ finding servers
void Client::discover(const std::vector<std::pair<u32, u16>>& extra) {
    if (!open()) return;
    const std::string body = "{\"proto\":1,\"client\":\"GT3 Revisited\"}";
    const Bytes p = packet(kDiscover, text_span(body), 0, 0, nullptr);
    send_to(0xFFFFFFFFu, kDefaultPort, p);
    for (const auto& [ip, port] : extra) send_to(ip, port, p);
    // Forget servers that stopped answering.
    const double t = now();
    servers_.erase(std::remove_if(servers_.begin(), servers_.end(), [t](const ServerInfo& s) { return t - s.seen > 6; }),
                   servers_.end());
}

// ------------------------------------------------------------------ connecting
void Client::connect(const Target& t) {
    if (!open()) {
        fail("no network");
        return;
    }
    if (status_ == Status::Connected) disconnect();
    target_ = t;
    attempts_ = 0;
    status_ = Status::Connecting;
    message_ = "connecting...";
    start_handshake();
}

void Client::start_handshake() {
    proving_ = false;
    session_ = 0;
    ++attempts_;
    if (!target_.ip && !target_.server_id.empty()) {          // find the server by its id first
        discover();
        handshake_at_ = retry_at_ = now();
        message_ = "looking for " + (target_.server_name.empty() ? std::string("the server") : target_.server_name) + "...";
        return;
    }
    if (attempts_ > 2 && !target_.server_id.empty()) discover(); // it may have a new address
    client_nonce_ = random_hex(16);
    send_setup(kConnect, "{\"proto\":1,\"wheel\":" + std::to_string(target_.wheel) + ",\"client\":" +
                             q("GT3 Revisited") + ",\"client_nonce\":" + q(client_nonce_) + "}");
    handshake_at_ = now();
    retry_at_ = handshake_at_ + kRetryEvery;
}

void Client::disconnect() {
    if (status_ == Status::Connected && session_) send_session(kBye);
    status_ = Status::Idle;
    session_ = 0;
    proving_ = false;
    message_.clear();
    have_state_ = false;
}

void Client::lost(const std::string& why, bool retry) {
    const bool was = status_ == Status::Connected;
    session_ = 0;
    proving_ = false;
    have_state_ = false;
    if (was) wheel_changed_ = true;
    status_ = retry ? Status::Reconnecting : Status::Failed;
    message_ = why;
    retry_at_ = now() + (retry ? kRetryEvery : 1e18);
}

void Client::fail(const std::string& why) { lost(why, false); }

// ------------------------------------------------------------------ receiving
void Client::handle(const u8* d, size_t n, u32 ip, u16 port) {
    if (n < kHeader || std::memcmp(d, "NXW1", 4) != 0) return;
    const u8 type = d[4], flags = d[5];
    const u32 session = get<u32>(d + 8), seq = get<u32>(d + 12);
    const bool signed_ = flags & 1;
    const size_t body_end = signed_ ? n - kSig : n;
    if (body_end < kHeader) return;
    const u8* body = d + kHeader;
    const size_t len = body_end - kHeader;
    auto signature_ok = [&](const std::array<u8, 32>& key) {
        if (!signed_) return false;
        const std::array<u8, 32> want = hmac_sha256(key, ByteSpan(d, body_end));
        u8 diff = 0;
        for (size_t i = 0; i < kSig; ++i) diff |= u8(want[i] ^ d[body_end + i]);
        return diff == 0;
    };

    if (type == kAnnounce && !signed_) {
        json::Value j;
        if (!parse_json(body, len, j) || j["proto"].integer() != 1) return;
        ServerInfo s;
        s.ip = ip, s.port = port, s.address = ip_text(ip);
        s.server_id = j["server_id"].string();
        s.name = j["name"].string();
        s.software = j["software"].string();
        s.password = j["password"].boolean();
        for (const json::Value& w : j["wheels"].items()) s.wheels.push_back(parse_wheel(w));
        s.seen = now();
        const std::string id = s.server_id;
        auto it = std::find_if(servers_.begin(), servers_.end(), [&](const ServerInfo& o) {
            return (!id.empty() && o.server_id == id) || (o.ip == ip && o.port == port);
        });
        if (it != servers_.end()) *it = std::move(s);
        else servers_.push_back(std::move(s));
        // The server we want, found again (maybe at a new address): connect there.
        const bool trying = status_ == Status::Connecting || status_ == Status::Reconnecting;
        if (trying && !id.empty() && id == target_.server_id && (target_.ip != ip || target_.port != port)) {
            target_.ip = ip, target_.port = port;
            start_handshake();
        }
        return;
    }
    if (ip != target_.ip || port != target_.port) return;     // everything else: only from our server

    if (type == kChallenge || type == kReject) {
        if (status_ == Status::Idle || status_ == Status::Connected) return;
        json::Value j;
        if (!parse_json(body, len, j) || j["client_nonce"].string() != client_nonce_) return; // not ours: ignored
        if (type == kReject) {
            const std::string reason = j["reason"].string(), msg = j["message"].string();
            if (reason == "password") fail(target_.server_password.empty() && target_.wheel_password.empty()
                                               ? "a password is needed" : "wrong password");
            else if (reason == "proto") fail("this server speaks another protocol version");
            else if (reason == "nonce") start_handshake();
            else if (reason == "busy") {
                lost(msg.empty() ? "the server is busy" : msg, true);
                retry_at_ = now() + (std::max)(1.0, j["retry_ms"].number(2000) / 1000.0);
            } else lost(msg.empty() ? reason : msg, true);    // in_use, no_wheel: try again later
            return;
        }
        if (proving_) return;
        const std::string nonce = j["nonce"].string(), salt = j["salt"].string();
        if (!is_hex(nonce, 32) || !is_hex(salt, 32)) return;
        const int iterations = std::clamp(j["iterations"].integer(20000), 1, 1000000);
        const std::string cache_id = salt + "\n" + std::to_string(iterations) + "\n" + target_.server_password + "\n" +
                                     target_.wheel_password;
        if (cache_id != key_cache_id_) {                       // (PBKDF2 takes a moment: once per password)
            key_cache_ = derive_key(target_.server_password, target_.wheel_password, salt, iterations);
            key_cache_id_ = cache_id;
        }
        key_ = key_cache_;
        server_nonce_ = nonce;
        pending_session_key_ = session_key(key_, nonce, client_nonce_);
        proving_ = true;
        send_setup(kConnect, "{\"proto\":1,\"wheel\":" + std::to_string(target_.wheel) + ",\"client\":" +
                                 q("GT3 Revisited") + ",\"client_nonce\":" + q(client_nonce_) + ",\"nonce\":" + q(nonce) +
                                 ",\"proof\":" + q(proof(key_, nonce, client_nonce_, target_.wheel)) +
                                 ",\"state_hz\":100}");
        retry_at_ = now() + kRetryEvery;
        return;
    }
    if (type == kAccept) {
        if (!proving_ || !signature_ok(pending_session_key_)) return; // a fake ACCEPT can't be signed
        json::Value j;
        if (!parse_json(body, len, j)) return;
        if (u32(j["session"].number()) != session || !session) return;
        session_ = session;
        session_key_ = pending_session_key_;
        seq_in_ = seq;
        seq_out_ = 0;
        proving_ = false;
        wheel_ = parse_wheel(j["wheel"]);
        status_ = Status::Connected;
        message_.clear();
        attempts_ = 0;
        last_rx_ = now();
        have_state_ = false;
        wheel_changed_ = true;
        forces_were_on_ = false;
        return;
    }
    // In the session: signed with its key, newer than what we had.
    if (status_ != Status::Connected || session != session_ || !signature_ok(session_key_) || seq <= seq_in_) return;
    seq_in_ = seq;
    last_rx_ = now();
    if (type == kState && len >= kStateSize) {
        WheelState& s = state_;
        s.steer = clamp_f(get<float>(body + 4), -1, 1);
        s.throttle = clamp_f(get<float>(body + 8), 0, 1);
        s.brake = clamp_f(get<float>(body + 12), 0, 1);
        s.clutch = clamp_f(get<float>(body + 16), 0, 1);
        s.handbrake = clamp_f(get<float>(body + 20), 0, 1);
        s.buttons = get<u64>(body + 24);
        s.dpad = body[32];
        s.gear = s8(body[33]);
        s.status = body[34];
        const size_t extra = body[35];
        s.extra.clear();
        for (size_t i = 0; i < extra && kStateSize + 4 * (i + 1) <= len; ++i)
            s.extra.push_back(get<float>(body + kStateSize + 4 * i));
        have_state_ = true;
    } else if (type == kNotice) {
        json::Value j;
        if (!parse_json(body, len, j)) return;
        const std::string ev = j["event"].string();
        if (ev == "changed" && j["wheel"].is_object()) wheel_ = parse_wheel(j["wheel"]);
        else if (ev == "unplugged") wheel_.connected = false;
        else if (ev == "plugged") wheel_.connected = true;
        else if (ev == "kicked") message_ = "kicked";
        else if (ev == "shutdown") message_ = "shutdown";
        wheel_changed_ = true;
    } else if (type == kBye) {
        if (message_ == "kicked") fail("the server's owner ended the session");
        else lost("the server stopped", true);
    }
}

// ------------------------------------------------------------------ every frame
void Client::poll() {
    if (sock_ < 0) return;
    for (int i = 0; i < 256; ++i) {
        u8 buf[2048];
        sockaddr_in from{};
        nxw_socklen fl = sizeof from;
        const auto n = recvfrom(static_cast<sock_t>(sock_), reinterpret_cast<char*>(buf), sizeof buf, 0,
                                reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) break;
        handle(buf, size_t(n), ntohl(from.sin_addr.s_addr), ntohs(from.sin_port));
    }
    const double t = now();
    switch (status_) {
    case Status::Connecting:
    case Status::Reconnecting:
        if (t >= retry_at_) {
            if (status_ == Status::Reconnecting) status_ = Status::Connecting;
            start_handshake();
            if (attempts_ > 1 && message_ == "connecting...") message_ = "no answer from the server, still trying...";
        }
        break;
    case Status::Connected: {
        if (t - last_rx_ > kLostAfter) {
            lost("no answer from the server, trying again...", true);
            break;
        }
        const bool game = game_forces_.on && t - game_forces_at_ < 0.2;
        const bool buzz = t < identify_until_;
        if ((game || buzz) && t - forces_sent_at_ >= kForcesEvery) {
            Forces f = game ? game_forces_ : Forces{};
            f.on = true;
            if (buzz) f.rumble = 1, f.rumble_hz = 0;
            Bytes b;
            put<u8>(b, 1);
            put<u8>(b, 0), put<u8>(b, 0), put<u8>(b, 0);
            for (float v : {clamp_f(f.constant, -1, 1), clamp_f(f.spring, 0, 1), clamp_f(f.spring_center, -1, 1),
                            clamp_f(f.damper, 0, 1), clamp_f(f.friction, 0, 1), clamp_f(f.rumble, 0, 1),
                            clamp_f(f.rumble_hz, 0, 250)})
                put<float>(b, v);
            send_session(kForces, b);
            forces_sent_at_ = t;
            forces_were_on_ = true;
        } else if (!game && !buzz && forces_were_on_) {        // forces off: say so once (the wheel goes limp)
            Bytes b(32, 0);
            send_session(kForces, b);
            forces_were_on_ = false;
        } else if (t - last_tx_ > kKeepEvery) {
            send_session(kKeepalive);
        }
        break;
    }
    default:
        break;
    }
}

void Client::send_forces(const Forces& f) {
    game_forces_ = f;
    game_forces_at_ = now();
}

void Client::identify(double seconds) { identify_until_ = now() + seconds; }

bool Client::take_state(WheelState& out) {
    if (!have_state_) return false;
    out = state_;
    have_state_ = false;
    return true;
}

} // namespace gt3r::netxwheel
