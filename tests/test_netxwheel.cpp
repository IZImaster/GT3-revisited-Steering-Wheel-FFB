// The network wheel's NetXWheel client (input/netxwheel.h): its keys match the NetXWheel server's
// (values from its Python implementation), and a whole session against a small server in this
// process: finding it, a password, connecting, the wheel's state, forces, fake answers ignored.

#include "test_framework.h"

#include "base/sha256.h"
#include "formats/json.h"
#include "input/netxwheel.h"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#define T_CLOSE closesocket
using t_socklen = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define T_CLOSE ::close
using t_socklen = socklen_t;
#endif

using namespace gt3r;
using netxwheel::Client;

namespace {

ByteSpan span_of(const std::string& s) { return ByteSpan(reinterpret_cast<const u8*>(s.data()), s.size()); }

// A NetXWheel server with one wheel, on a loopback port, answered by pump().
struct MiniServer {
    using sock_t = decltype(socket(0, 0, 0));
    sock_t s;
    u16 port = 0;
    std::string password = "secret";
    std::string salt = "00112233445566778899aabbccddeeff";
    std::string nonce, client_nonce;
    std::array<u8, 32> key{}, session_key{};
    u32 session = 0x01020304, seq = 0;
    sockaddr_in client{};
    bool connected = false;
    int forces = 0;
    float last_constant = 0, last_rumble = 0;
    bool fake_reject_sent = false;

    MiniServer() {
#if defined(_WIN32)
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
#endif
        s = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
        t_socklen l = sizeof a;
        getsockname(s, reinterpret_cast<sockaddr*>(&a), &l);
        port = ntohs(a.sin_port);
#if defined(_WIN32)
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
#else
        fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
        key = Client::derive_key(password, "", salt, 20000);
    }
    ~MiniServer() { T_CLOSE(s); }

    void send(const sockaddr_in& to, const Bytes& b) {
        sendto(s, reinterpret_cast<const char*>(b.data()), int(b.size()), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    }
    void send_json(const sockaddr_in& to, u8 type, const std::string& j) { send(to, Client::packet(type, span_of(j), 0, 0, nullptr)); }

    void pump() {
        for (;;) {
            u8 buf[2048];
            sockaddr_in from{};
            t_socklen fl = sizeof from;
            const auto n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (n <= 0) return;
            if (n < 16 || std::memcmp(buf, "NXW1", 4) != 0) continue;
            const u8 type = buf[4];
            const bool sig = buf[5] & 1;
            const size_t end = sig ? size_t(n) - 16 : size_t(n);
            const std::string body(reinterpret_cast<const char*>(buf) + 16, end - 16);
            if (type == 1) { // DISCOVER
                send_json(from, 2, "{\"proto\":1,\"server_id\":\"abcdef0123456789\",\"name\":\"Test server\",\"software\":\"mini\","
                                   "\"password\":true,\"wheels\":[{\"index\":1,\"vendor\":\"Logitech\",\"model\":\"G920\","
                                   "\"rotation\":900,\"steer_range\":360,\"ffb\":true,\"direct_drive\":false,\"torque_nm\":2.3,"
                                   "\"pedals\":[\"throttle\",\"brake\"],\"shifter\":\"h6r\",\"password\":false,\"in_use\":false}]}");
            } else if (type == 3) { // CONNECT
                json::Value j = json::Value::parse(body);
                const std::string cn = j["client_nonce"].string();
                if (!j.has("proof")) {
                    client_nonce = cn;
                    nonce = "0123456789abcdef0123456789abcdef";
                    // first a fake rejection with another client_nonce: the client must ignore it
                    send_json(from, 6, "{\"reason\":\"password\",\"message\":\"fake\",\"client_nonce\":\"" + std::string(32, '0') + "\"}");
                    fake_reject_sent = true;
                    send_json(from, 4, "{\"nonce\":\"" + nonce + "\",\"salt\":\"" + salt +
                                           "\",\"iterations\":20000,\"server_password\":true,\"wheel_password\":false,"
                                           "\"client_nonce\":\"" + cn + "\"}");
                } else {
                    const std::string want = Client::proof(key, nonce, client_nonce, 1);
                    if (j["proof"].string() != want || cn != client_nonce) {
                        send_json(from, 6, "{\"reason\":\"password\",\"message\":\"wrong\",\"client_nonce\":\"" + cn + "\"}");
                        continue;
                    }
                    session_key = Client::session_key(key, nonce, client_nonce);
                    // a fake ACCEPT first (signed with another key): the client must ignore it
                    std::array<u8, 32> other{};
                    const std::string acc = "{\"session\":" + std::to_string(session) +
                                            ",\"wheel\":{\"index\":1,\"vendor\":\"Logitech\",\"model\":\"G920\",\"rotation\":900,"
                                            "\"steer_range\":360,\"ffb\":true,\"forces\":[\"constant\",\"spring\"],"
                                            "\"buttons\":[{\"bit\":0,\"role\":\"a\",\"label\":\"A\"}]}}";
                    send(from, Client::packet(5, span_of(acc), session, ++seq, other.data()));
                    send(from, Client::packet(5, span_of(acc), session, ++seq, session_key.data()));
                    client = from;
                    connected = true;
                }
            } else if (connected && (type == 17 || type == 18 || type == 20) && sig) {
                const std::array<u8, 32> h = hmac_sha256(session_key, ByteSpan(buf, end));
                if (std::memcmp(h.data(), buf + end, 16) != 0) continue;
                if (type == 17 && end - 16 >= 32) {
                    ++forces;
                    std::memcpy(&last_constant, buf + 16 + 4, 4);
                    std::memcpy(&last_rumble, buf + 16 + 24, 4);
                }
                if (type == 18) send(client, Client::packet(18, {}, session, ++seq, session_key.data()));
                if (type == 20) connected = false;
            }
        }
    }

    void send_state(float steer, u64 buttons, s8 gear) {
        Bytes b(36, 0);
        std::memcpy(b.data() + 4, &steer, 4);
        float throttle = 1;
        std::memcpy(b.data() + 8, &throttle, 4);
        std::memcpy(b.data() + 24, &buttons, 8);
        b[33] = u8(gear);
        send(client, Client::packet(16, b, session, ++seq, session_key.data()));
    }
};

void run(Client& c, MiniServer& s, double seconds) {
    const double end = Client::now() + seconds;
    while (Client::now() < end) {
        s.pump();
        c.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} // namespace

TEST(netxwheel_keys_match_the_server) {
    // From the NetXWheel server (netxwheel/protocol.py) with the same inputs.
    const auto key = Client::derive_key("srv pw", "wheel pw", "00112233445566778899aabbccddeeff", 20000);
    CHECK(Client::hex(key) == "7d8b048567d998406b2b9cb3428536b7bfb4c0e37f6f3f13123103ae46070f64");
    const std::string n = "0123456789abcdef0123456789abcdef", cn = "fedcba9876543210fedcba9876543210";
    CHECK(Client::proof(key, n, cn, 2) == "fdceb78f9446ec746c805ed437f83c678b80356c8d531ab6c2c8a9316f71fd7e");
    const auto sk = Client::session_key(key, n, cn);
    CHECK(Client::hex(sk) == "adad9e74521c4425273487c287e08f411b78b95c970dc369323ebac72bfde139");
    CHECK(Client::hex(Client::packet(18, {}, 7, 9, sk.data())) == "4e5857311201000007000000090000002df374adf345a77c6da97e90e79ae523");
    CHECK(Client::hex(Client::derive_key("", "", "00112233445566778899aabbccddeeff", 20000)) ==
          "b317b492cb5f37b8e84c539625093de8aa853ecc37e25c27c09a89b6b6cbc64d");
    // HMAC-SHA256 and PBKDF2 themselves (RFC 4231 case 2, RFC 7914 section 11).
    CHECK(Client::hex(hmac_sha256(span_of("Jefe"), span_of("what do ya want for nothing?"))) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    const Bytes p = pbkdf2_sha256(span_of("passwd"), span_of("salt"), 1, 64);
    CHECK(Client::hex(p) == "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
                            "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
}

TEST(netxwheel_session_with_a_server) {
    MiniServer srv;
    Client c;
    CHECK(c.open());
    c.discover({{0x7F000001u, srv.port}});
    run(c, srv, 0.3);
    bool found = false;
    for (const netxwheel::ServerInfo& s : c.servers())
        if (s.port == srv.port) {
            found = true;
            CHECK(s.name == "Test server");
            CHECK(s.password);
            CHECK(s.wheels.size() == 1 && s.wheels[0].title() == "Logitech G920" && s.wheels[0].rotation == 900 &&
                  s.wheels[0].torque_nm > 2.2f && s.wheels[0].shifter == "h6r");
        }
    CHECK(found);

    // A wrong password: refused, and it stops trying.
    netxwheel::Target t;
    t.ip = 0x7F000001u, t.port = srv.port, t.wheel = 1, t.server_password = "nope", t.server_id = "abcdef0123456789";
    c.connect(t);
    run(c, srv, 0.5);
    CHECK(c.status() == Client::Status::Failed);
    CHECK(srv.fake_reject_sent);

    // The right one: connected (the fake ACCEPT before the real one is ignored).
    t.server_password = "secret";
    c.connect(t);
    run(c, srv, 0.5);
    CHECK(c.connected());
    CHECK(c.wheel().title() == "Logitech G920" && c.wheel().buttons.size() == 1 && c.wheel().forces.size() == 2);

    srv.send_state(0.5f, 1, 3);
    run(c, srv, 0.1);
    netxwheel::WheelState st;
    CHECK(c.take_state(st));
    CHECK(st.steer > 0.49f && st.steer < 0.51f && st.throttle > 0.99f && st.buttons == 1 && st.gear == 3);

    // Forces reach the server, signed; the rumble to identify the wheel too.
    netxwheel::Forces f;
    f.on = true, f.constant = 0.25f;
    for (int i = 0; i < 10; ++i) {
        c.send_forces(f);
        run(c, srv, 0.02);
    }
    CHECK(srv.forces > 0 && srv.last_constant > 0.24f && srv.last_constant < 0.26f);
    run(c, srv, 0.3);
    c.identify(0.3);
    run(c, srv, 0.1);
    CHECK(srv.last_rumble > 0.99f);

    // Quiet (no forces, no state): the keep-alives keep the session.
    run(c, srv, 1.0);
    CHECK(c.connected());
    c.disconnect();
    run(c, srv, 0.1);
    CHECK(!srv.connected);
}
