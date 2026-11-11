// Home > Network wheel: pick a NetXWheel server on the network, then one of its wheels, typing
// passwords where they are needed; the chosen wheel then works as if it were plugged in here
// (input/net_wheel.h), and is connected again at the next start. A connected wheel can be made to
// rumble for two seconds, to tell which one it is in a room with several.

#include "app/screen_netwheel.h"

#include "app/app.h"
#include "app/screen_registry.h"
#include "app/text_keyboard.h"
#include "base/config.h"
#include "input/input_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gt3r {

namespace {

using netxwheel::Client;
using netxwheel::ServerInfo;
using netxwheel::WheelInfo;

std::string join(const std::vector<std::string>& v, const char* sep = ", ") {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : sep) + x;
    return s;
}

std::string shifter_text(const std::string& s) {
    if (s == "h6r") return "H-shifter, 6 gears and reverse";
    if (s == "h7r") return "H-shifter, 7 gears and reverse";
    if (s == "h6" || s == "h7") return "H-shifter, " + s.substr(1) + " gears";
    if (s == "sequential") return "sequential";
    return "none";
}

class NetWheelScreen : public Screen {
public:
    void update(App& app, double dt) override {
        NetWheel* nw = app.input().netwheel();
        blink_ += dt;
        if (!nw) {
            if (app.menu().pressed(PadButton::Triangle) || app.menu().pressed(PadButton::Circle)) app.pop();
            return;
        }
        Client& c = nw->client();
        // Ask the network every two seconds while the page is open (the list keeps itself fresh).
        ask_in_ -= dt;
        if (ask_in_ <= 0) {
            ask_in_ = 2.0;
            c.discover(typed_);
        }
        if (editing_) {
            if (keyboard_.is_open())
                if (TextKeyboard::Result r = keyboard_.update(app.menu(), pointer_.take(), text_); r != TextKeyboard::Result::None)
                    end_edit(app, r == TextKeyboard::Result::Done);
            return;
        }
        pointer_.take();
        rebuild(app);
        const MenuInput& m = app.menu();
        const int n = int(rows_.size());
        if (n) sel_ = std::clamp(sel_ + m.vertical(), 0, n - 1);
        if (XrList::Hit h = xr_list_.take_hover(); h.row >= 0 && h.row < n) sel_ = h.row;
        if (XrList::Hit h = xr_list_.take_click(); h.row >= 0 && h.row < n) choose(app, sel_ = h.row);
        else if (m.pressed(PadButton::Cross) && n) choose(app, sel_);
        if (m.pressed(PadButton::Square) && page_ == Page::Wheels && this_server_connected(app)) rumble(app);
        if (m.pressed(PadButton::Triangle) || m.pressed(PadButton::Circle)) back(app);
        note_left_ -= dt;
    }

    void on_event(App& app, const SDL_Event& e) override {
        if (!editing_) {
            xr_list_.event(e);
            return;
        }
        pointer_.event(e);
        // Over the VR keyboard a real keyboard's letters only type; the arrows still move between its keys.
        if (keyboard_.is_open() && e.type == SDL_EVENT_KEY_DOWN && e.key.key != SDLK_UP && e.key.key != SDLK_DOWN &&
            e.key.key != SDLK_LEFT && e.key.key != SDLK_RIGHT)
            app.menu().suppress();
        if (const TextKeyboard::Result r = text_field_event(e, text_, !keyboard_.is_open()); r != TextKeyboard::Result::None)
            end_edit(app, r == TextKeyboard::Result::Done);
        if (text_.size() > 63) text_.resize(63);
    }

    bool clickable(float x, float y) const override { return editing_ && keyboard_.is_open() && keyboard_.clickable(x, y); }

    void render(App& app, Ui& ui) override {
        xr_list_.clear();
        NetWheel* nw = app.input().netwheel();
        ui.header(page_ == Page::Servers ? "NETWORK WHEEL" : "NETWORK WHEEL - " + server_title(),
                  nw ? Ui::clip(nw->status_text(), 330) : "No network");
        if (!nw) {
            ui.text_center(150, "This device has no network for the network wheel.", colors::kBad);
            ui.footer("Triangle: back");
            return;
        }
        rebuild(app);
        const int n = int(rows_.size());
        sel_ = n ? std::clamp(sel_, 0, n - 1) : 0;
        constexpr int kRows = 11;
        constexpr float kTop = 30, kRowH = 14;
        const int first = xr_list_.first(0, sel_, kRows, ui.list_window(n, sel_, kRows));
        xr_list_.rows(0, first, std::min(kRows, n - first), 4, Ui::kWidth - 4, kTop - 2, kRowH);
        for (int i = 0; i < kRows && first + i < n; ++i) {
            const Row& r = rows_[size_t(first + i)];
            const float y = kTop + i * kRowH;
            if (first + i == sel_) ui.fill(4, y - 2, Ui::kWidth - 8, kRowH, colors::kHighlight);
            ui.text(12, y, Ui::clip(r.left, 300), r.color);
            if (!r.right.empty()) ui.text_right(Ui::kWidth - 12, y, Ui::clip(r.right, 300), colors::kDim);
        }
        if (n == 0) ui.text(12, kTop, "Looking for servers on your network...", colors::kDim);
        if (n > kRows) ui.scrollbar({Ui::kWidth - 6, kTop - 2, 3, kRows * kRowH}, n * kRowH, first * kRowH);
        // The selected entry's details.
        const float dy = kTop + kRows * kRowH + 8;
        ui.fill(4, dy - 4, Ui::kWidth - 8, Ui::kHeight - dy - 18, colors::kPanel);
        float y = dy;
        for (const std::string& line : details(app)) {
            if (y > Ui::kHeight - 34) break;
            ui.text(12, y, Ui::clip(line, Ui::kWidth - 24), colors::kText);
            y += 12;
        }
        if (note_left_ > 0) ui.text_center(Ui::kHeight - 30, note_, colors::kAccent);
        if (editing_) draw_edit(ui);
        else if (page_ == Page::Servers) ui.footer("Cross: choose   Triangle: back");
        else ui.footer(this_server_connected(app) ? "Cross: choose   Square: rumble the connected wheel   Triangle: servers"
                                                  : "Cross: connect   Triangle: servers");
    }

private:
    enum class Page : u8 { Servers, Wheels };
    enum class Kind : u8 { Server, TypeAddress, Wheel, Rumble, Disconnect };
    enum class Edit : u8 { Address, ServerPassword, WheelPassword };
    struct Row {
        Kind kind;
        int arg = 0; // server: index in servers(); wheel: its number
        std::string left, right;
        Color color = colors::kText;
    };

    // --- what the page shows -------------------------------------------------------------
    const ServerInfo* chosen(App& app) const {
        NetWheel* nw = app.input().netwheel();
        if (!nw) return nullptr;
        for (const ServerInfo& s : nw->client().servers())
            if ((!server_id_.empty() && s.server_id == server_id_) || (server_id_.empty() && s.ip == server_ip_))
                return &s;
        return nullptr;
    }
    std::string server_title() const { return server_name_.empty() ? "server" : server_name_; }

    // The game's session is with this server (connected, or trying).
    bool this_server(App& app) const {
        const NetWheel* nw = app.input().netwheel();
        if (!nw) return false;
        const Client& c = nw->client();
        const netxwheel::Target& t = c.target();
        return c.status() != Client::Status::Idle && !server_id_.empty() && t.server_id == server_id_;
    }
    bool this_server_connected(App& app) const {
        return this_server(app) && app.input().netwheel()->client().connected();
    }

    void rebuild(App& app) {
        rows_.clear();
        NetWheel* nw = app.input().netwheel();
        if (!nw) return;
        const Client& c = nw->client();
        if (page_ == Page::Servers) {
            const std::vector<ServerInfo>& ss = c.servers();
            for (size_t i = 0; i < ss.size(); ++i) {
                const ServerInfo& s = ss[i];
                const bool ours = c.status() != Client::Status::Idle && c.target().server_id == s.server_id;
                std::string right = s.address + "   " + std::to_string(s.wheels.size()) +
                                    (s.wheels.size() == 1 ? " wheel" : " wheels") + (s.password ? "   password" : "");
                rows_.push_back({Kind::Server, int(i), (ours ? "> " : "") + (s.name.empty() ? s.address : s.name), right,
                                 ours ? colors::kGood : colors::kText});
            }
            rows_.push_back({Kind::TypeAddress, 0, "Type a server's address...", "if it isn't in the list", colors::kDim});
            if (c.status() != Client::Status::Idle) rows_.push_back({Kind::Disconnect, 0, "Disconnect the network wheel", "", colors::kText});
            return;
        }
        const ServerInfo* s = chosen(app);
        if (s)
            for (const WheelInfo& w : s->wheels) {
                const bool ours = this_server(app) && c.target().wheel == w.index;
                std::string state;
                if (ours) state = c.connected() ? "connected here" : "connecting...";
                else if (!w.connected) state = "unplugged";
                else if (w.in_use) state = "in use by " + w.in_use_by;
                else state = "free";
                if (w.password) state += "   password";
                Color col = ours ? colors::kGood : (w.in_use || !w.connected) ? colors::kDim : colors::kText;
                rows_.push_back({Kind::Wheel, w.index, (ours ? "> " : "") + std::string("Wheel ") + std::to_string(w.index) + ": " + w.title(),
                                 state, col});
            }
        if (this_server_connected(app)) rows_.push_back({Kind::Rumble, 0, "Rumble the connected wheel (2 seconds)", "Square", colors::kText});
        if (this_server(app)) rows_.push_back({Kind::Disconnect, 0, "Disconnect", "", colors::kText});
    }

    std::vector<std::string> details(App& app) {
        std::vector<std::string> out;
        NetWheel* nw = app.input().netwheel();
        if (rows_.empty() || !nw) {
            out.push_back("The servers are computers with a steering wheel and NetXWheel");
            out.push_back("(github.com/IZImaster/NetXWheel), on the same network as this device.");
            return out;
        }
        const Row& r = rows_[size_t(std::clamp(sel_, 0, int(rows_.size()) - 1))];
        const Client& c = nw->client();
        if (r.kind == Kind::Server) {
            const ServerInfo& s = c.servers()[size_t(r.arg)];
            out.push_back(s.name + "  -  " + s.address + "  -  " + s.software);
            out.push_back(s.password ? "Needs the server's password." : "No server password.");
            for (const WheelInfo& w : s.wheels)
                out.push_back("Wheel " + std::to_string(w.index) + ": " + w.title() +
                              (w.in_use ? "  (in use)" : !w.connected ? "  (unplugged)" : ""));
        } else if (r.kind == Kind::TypeAddress) {
            out.push_back("A server on another part of the network may not answer the search:");
            out.push_back("type its address (the Pi shows it with: hostname -I), like 192.168.1.50.");
        } else if (r.kind == Kind::Wheel) {
            const ServerInfo* s = chosen(app);
            const WheelInfo* w = nullptr;
            if (s)
                for (const WheelInfo& x : s->wheels)
                    if (x.index == r.arg) w = &x;
            if (this_server_connected(app) && c.wheel().index == r.arg) w = &c.wheel(); // the full description
            if (!w) return out;
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s  -  turns %d degrees, full lock in the game at %d (%d each way)",
                          w->title().c_str(), w->rotation, w->steer_range, w->steer_range / 2);
            out.push_back(buf);
            std::string f = w->ffb ? "Force feedback: yes" : "Force feedback: no";
            if (!w->forces.empty()) f += " (" + join(w->forces) + ")";
            if (w->direct_drive) f += "   Direct drive";
            if (w->torque_nm > 0) {
                std::snprintf(buf, sizeof buf, "   %.1f Nm", w->torque_nm);
                f += buf;
            }
            out.push_back(f);
            out.push_back("Pedals: " + (w->pedals.empty() ? std::string("none") : join(w->pedals)) + "   Shifter: " +
                          shifter_text(w->shifter));
            if (!w->buttons.empty()) {
                std::vector<std::string> labels;
                for (const WheelInfo::Button& b : w->buttons) labels.push_back(b.label);
                out.push_back("Buttons: " + join(labels));
            }
            out.push_back(w->password ? "Needs this wheel's password too." : "No wheel password.");
            if (this_server(app) && c.target().wheel == r.arg && !c.message().empty()) out.push_back(c.message());
            else if (w->in_use && !(this_server(app) && c.target().wheel == r.arg))
                out.push_back("In use by " + w->in_use_by + ": it's free again when that game leaves.");
        } else if (r.kind == Kind::Rumble) {
            out.push_back("The wheel vibrates for two seconds, to tell which one it is.");
        } else if (r.kind == Kind::Disconnect) {
            out.push_back("Ends the session: the wheel is free for others, and this game won't connect at the");
            out.push_back("next start until you choose a wheel again.");
        }
        return out;
    }

    // --- actions ---------------------------------------------------------------------------
    void choose(App& app, int i) {
        NetWheel* nw = app.input().netwheel();
        if (!nw || i < 0 || i >= int(rows_.size())) return;
        const Row r = rows_[size_t(i)];
        Client& c = nw->client();
        switch (r.kind) {
        case Kind::Server: {
            const ServerInfo& s = c.servers()[size_t(r.arg)];
            server_id_ = s.server_id, server_ip_ = s.ip, server_port_ = s.port, server_name_ = s.name;
            const netxwheel::Target& saved = app.input().netwheel_target();
            server_password_ = saved.server_id == s.server_id ? saved.server_password : std::string();
            if (s.password && server_password_.empty()) begin_edit(app, Edit::ServerPassword);
            else open_wheels();
            break;
        }
        case Kind::TypeAddress: begin_edit(app, Edit::Address); break;
        case Kind::Wheel: {
            const ServerInfo* s = chosen(app);
            if (!s) break;
            const WheelInfo* w = nullptr;
            for (const WheelInfo& x : s->wheels)
                if (x.index == r.arg) w = &x;
            if (!w) break;
            if (this_server_connected(app) && c.target().wheel == w->index) {
                note("Already connected to this wheel");
                break;
            }
            wheel_ = w->index;
            const netxwheel::Target& saved = app.input().netwheel_target();
            const bool failed_password = this_server(app) && c.status() == Client::Status::Failed &&
                                         c.message().find("password") != std::string::npos;
            wheel_password_ = saved.server_id == server_id_ && saved.wheel == wheel_ && !failed_password ? saved.wheel_password
                                                                                                        : std::string();
            if (failed_password && s->password) {          // ask again: which one was wrong isn't known
                begin_edit(app, Edit::ServerPassword, true);
                break;
            }
            if (w->password && wheel_password_.empty()) begin_edit(app, Edit::WheelPassword);
            else connect(app);
            break;
        }
        case Kind::Rumble: rumble(app); break;
        case Kind::Disconnect:
            app.input().netwheel_disconnect();
            app.save_config();
            note("Disconnected");
            break;
        }
    }

    void open_wheels() {
        page_ = Page::Wheels;
        sel_ = 0;
    }

    void connect(App& app) {
        netxwheel::Target t;
        t.ip = server_ip_, t.port = server_port_, t.wheel = wheel_;
        t.server_id = server_id_, t.server_name = server_name_;
        t.server_password = server_password_, t.wheel_password = wheel_password_;
        app.input().netwheel_connect(t);
        app.save_config();
        note("Connecting to wheel " + std::to_string(wheel_) + "...");
    }

    void rumble(App& app) {
        NetWheel* nw = app.input().netwheel();
        if (!nw || !nw->client().connected()) return;
        nw->client().identify(2.0);
        note("Rumbling wheel " + std::to_string(nw->client().wheel().index) + " for 2 seconds");
    }

    void back(App& app) {
        if (page_ == Page::Wheels) {
            page_ = Page::Servers;
            sel_ = 0;
            return;
        }
        app.save_config();
        app.pop();
    }

    void note(const std::string& s) {
        note_ = s;
        note_left_ = 3;
    }

    // --- typing ----------------------------------------------------------------------------
    void begin_edit(App& app, Edit what, bool then_wheel = false) {
        edit_ = what;
        then_wheel_ = then_wheel;
        editing_ = true;
        text_.clear();
        blink_ = 0;
        if (SDL_Window* w = app.window()) SDL_StartTextInput(w);
        static const char* const kTitles[] = {"Server address", "Server password", "Wheel password"};
#if GT3R_XR
        if (app.xr()) keyboard_.open(kTitles[int(what)], "OK", false); // (no system keyboard in a headset)
#else
        (void)kTitles;
#endif
    }

    void end_edit(App& app, bool keep) {
        editing_ = false;
        keyboard_.close();
        if (SDL_Window* w = app.window()) SDL_StopTextInput(w);
        app.menu().suppress(); // the keys typed are no menu input
        if (!keep) return;
        const std::string t(trim(text_));
        if (edit_ == Edit::Address) {
            const u32 ip = NetWheel::parse_ip(t);
            if (!ip) {
                note("\"" + t + "\" is not an address like 192.168.1.50");
                return;
            }
            if (std::find(typed_.begin(), typed_.end(), std::make_pair(ip, netxwheel::kDefaultPort)) == typed_.end())
                typed_.push_back({ip, netxwheel::kDefaultPort});
            ask_in_ = 0; // ask it now
            note("Asking " + t + "...");
        } else if (edit_ == Edit::ServerPassword) {
            server_password_ = text_;
            if (then_wheel_) {
                const ServerInfo* s = chosen(app);
                bool wpw = false;
                if (s)
                    for (const WheelInfo& w : s->wheels)
                        if (w.index == wheel_) wpw = w.password;
                if (wpw) begin_edit(app, Edit::WheelPassword);
                else connect(app);
            } else open_wheels();
        } else {
            wheel_password_ = text_;
            connect(app);
        }
    }

    void draw_edit(Ui& ui) {
        const bool secret = edit_ != Edit::Address;
        const std::string shown = secret ? std::string(text_.size(), '*') : text_;
        if (keyboard_.is_open()) {
            keyboard_.draw(ui, shown);
            return;
        }
        static const char* const kTitles[] = {"Server address (like 192.168.1.50)", "Server password", "Wheel password"};
        const float w = 400, h = 70, x = (Ui::kWidth - w) / 2, y = 120;
        ui.fill(0, 0, Ui::kWidth, Ui::kHeight, Color{0, 0, 0, 120});
        ui.fill_round(x, y, w, h, 6, colors::kPanel);
        ui.frame_round(x, y, w, h, 6, 1, colors::kAccent);
        ui.text(x + 12, y + 10, kTitles[int(edit_)], colors::kAccent);
        ui.fill_round(x + 12, y + 26, w - 24, 20, 4, colors::kBackground);
        ui.text(x + 20, y + 32, Ui::clip(shown, w - 50) + (std::fmod(blink_, 1.0) < 0.6 ? "_" : ""), colors::kText);
        ui.footer("Type, Enter: OK, Esc: cancel (pad: up/down letter, right add, left delete, A: OK)");
    }

    Page page_ = Page::Servers;
    std::vector<Row> rows_;
    int sel_ = 0;
    XrList xr_list_;
    double ask_in_ = 0, blink_ = 0, note_left_ = 0;
    std::string note_;
    std::vector<std::pair<u32, u16>> typed_; // addresses typed: asked too
    // the server and wheel being chosen
    std::string server_id_, server_name_, server_password_, wheel_password_;
    u32 server_ip_ = 0;
    u16 server_port_ = netxwheel::kDefaultPort;
    int wheel_ = 1;
    // typing
    bool editing_ = false, then_wheel_ = false;
    Edit edit_ = Edit::Address;
    std::string text_;
    TextKeyboard keyboard_; // in VR
    PointerInput pointer_;
};

} // namespace

std::unique_ptr<Screen> make_netwheel_screen() { return std::make_unique<NetWheelScreen>(); }

GT3R_REGISTER_PLAYER_SCREEN_NO_DISC(netwheel, "netwheel", "Network wheel",
                                    "Use a steering wheel plugged into another computer: choose the server and the wheel.",
                                    12, [] { return make_netwheel_screen(); });

} // namespace gt3r
