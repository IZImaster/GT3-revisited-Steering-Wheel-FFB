// VR headsets (docs/dev/VR.md): the race in stereo while driving. The headset's seated space
// (origin at the player's head, -z ahead) is placed in the world at the view's eye point: in
// the in-car views at the car's camera point 0 ([vr] seat_forward, seat_height from there),
// turning with the car (or with its heading only, [vr] level_horizon); in the chase views at
// the chase camera, level, turning with its lagged heading. Each eye is that placement times
// the eye's tracked pose. Replays, the pre-race shot, 2P split screen and the debug cameras
// stay on the virtual screen; replays there in 3D with [vr] replay_3d (render_stereo_screen).
// (Race screen code map: docs/dev/RACE.md.)

#include "app/screen_race_impl.h"

#if GT3R_XR

namespace gt3r::race_screen {

namespace {
constexpr float kPi = 3.14159265f;
}

bool RaceScreen::vr_stereo(App& app, bool split) const {
    const xr::Session* x = app.xr();
    if (!x || !x->settings().stereo || !x->eyes_valid() || !view_ || !course_scene_) return false;
    if (split || replaying_ || attract_ || demo_ || logger_mode_ || camera_.intro()) return false;
    const CameraMode m = camera_.mode();
    return m == CameraMode::Bumper || m == CameraMode::Roof || m == CameraMode::Chase || m == CameraMode::ChaseFar;
}

render::Mat4 RaceScreen::vr_base(const xr::Settings& st) const {
    const sim::Quat& q = render_state_.orientation;
    const render::Vec3 eye = camera_.eye();
    if (camera_.car_mounted()) { // the car's full attitude or its heading, the seat moved within the car
        const sim::Vec3 f = rotate(q, {0, 0, -1});
        const render::Mat4 car = st.level_horizon ? render::rotate_y(std::atan2(-f.x, -f.z))
                                                  : render::rotate_quat(q.w, q.x, q.y, q.z);
        const render::Mat4 seat = render::translate(eye) * car * render::translate({0, st.seat_height, -st.seat_forward});
        return camera_.look_back() ? seat * render::rotate_y(kPi) : seat;
    }
    // Chase: the rig's lagged heading (look-back included), level.
    const sim::Vec3 f = cam_forward(camera_);
    return render::translate(eye) * render::rotate_y(std::atan2(-f.x, -f.z));
}

void RaceScreen::render_vr_eyes(App& app, Ui& ui, const render::DrawOptions& o) {
    xr::Session& x = *app.xr();
    const xr::Settings& st = x.settings();
    SDL_Renderer* r = ui.renderer();
    SDL_Texture* canvas = SDL_GetRenderTarget(r);
    const render::Mat4 base = vr_base(st);

    // The items once for both eyes, from the head: the own car's body around the in-car view
    // unless [vr] car_body = off.
    const render::Mat4 head = base * x.head().matrix();
    const render::Vec3 head_pos = head.transform_point({0, 0, 0});
    const sim::Vec3 right{head.at(0, 0), head.at(1, 0), head.at(2, 0)};
    const xr::Eye& e0 = x.eye(0);
    TyreSmoke::View sv;
    sv.forward = render::normalize(render::Vec3{-head.at(0, 2), -head.at(1, 2), -head.at(2, 2)});
    sv.proj_x = 2.0f / std::max(e0.tan_right - e0.tan_left, 1e-3f);
    sv.near_m = camera_.near_plane, sv.far_m = camera_.gt3_far > 0 ? camera_.gt3_far : camera_.far_plane;
    const bool own_car = !(camera_.hides_car() || camera_.body_blocks_view()) || st.show_car;
    spark_pass_ = true, spark_view_ = render::inverse(head); // (the sparks' streaks from the head)
    collect_items(head_pos, right, own_car, true, sv);
    spark_pass_ = false;
    if (camera_.car_mounted()) // from inside the body: its bonnet, not its roof and windows
        for (size_t i = player_items_[0]; i < player_items_[1]; ++i) items_[i].single_sided = true;
    const float fovy = std::atan(e0.tan_up) - std::atan(e0.tan_down);
    const float sun_v = sun_visibility(head_pos, fovy, float(e0.height));
    // The glows of the cars as the flat view has them: on board, not the own car's (its body here is
    // seen from inside, so it hides nothing either).
    const GlowCars glow = has_glows() ? glow_cars(!camera_.hides_car(), true, -1) : GlowCars{};
    auto occl = [&](const render::Vec3& a, const render::Vec3& b) { return glow_blocked(glow, a, b); };
    // The sky domes centred on each eye, not on the head between them: they are small (Route
    // 11's is 33-51 m), so a shared centre put the sky at that distance in stereo, nearer than
    // the scenery drawn over it. Centred on the eye it has no parallax: at infinity.
    std::vector<std::pair<size_t, render::Mat4>> sky;
    for (size_t k = 0; k < items_.size(); ++k)
        if (items_[k].background) sky.emplace_back(k, items_[k].transform);

    for (int i = 0; i < 2; ++i) {
        const xr::Eye& e = x.eye(i);
        const render::Mat4 world = base * e.pose.matrix();
        const render::Mat4 view = render::inverse(world);
        const render::Vec3 eye = world.transform_point({0, 0, 0});
        const render::Mat4 proj = e.projection(camera_.near_plane, camera_.far_plane);
        const render::Frustum fr{e.tan_left, e.tan_right, e.tan_down, e.tan_up};
        for (const auto& [k, t] : sky) items_[k].transform = render::translate(eye - head_pos) * t;
        const SDL_FRect full{0, 0, float(e.width), float(e.height)};
        if (!vr_views_[i]) vr_views_[i] = std::make_unique<render::View3D>(*app.gpu());
        const size_t wet_items = wet_.active() ? add_wet_road(eye, view, proj, fr, o) : 0;
        // Straight into the eye's canvas (no composite pass); the glows go on top with the 2D
        // renderer, whose work runs after this.
        auto* tex = static_cast<SDL_GPUTexture*>(
            SDL_GetPointerProperty(SDL_GetTextureProperties(e.canvas), SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, nullptr));
        const bool direct = tex && vr_views_[i]->draw_to(tex, e.width, e.height, view, proj, eye, items_, o);
        SDL_SetRenderTarget(r, e.canvas);
        if (!direct) {
            SDL_SetRenderDrawColor(r, 0, 0, 0, 255); // opaque: the course passes keep the target's alpha
            SDL_RenderClear(r);
            vr_views_[i]->draw(r, full, view, proj, eye, items_, o);
        }
        remove_wet_road(wet_items); // (the wet road's frame items belong to this eye only)
        sun_.draw(r, full, view, fr, sun_v);
        lamps_.draw(r, full, view, eye, fr, occl);
        if (car_lamps_.active()) car_lamps_.draw(r, full, view, eye, fr, glow.lamps, occl);
        if (exit_transition_ > kExitHold) { // the race -> menu grey wash, over the whole view (presentation §C1)
            const SDL_FColor w = menu::flow::ScreenTransition::delay_fadeout_cover(exit_transition_);
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColorFloat(r, w.r, w.g, w.b, w.a);
            SDL_RenderFillRect(r, &full);
        }
    }
    for (const auto& [k, t] : sky) items_[k].transform = t;
    hitch::meshes = vr_views_[0]->stats().meshes_drawn;

    // Back on the canvas: transparent, for the HUD panel.
    SDL_SetRenderTarget(r, canvas);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
    SDL_RenderClear(r);
    // The rear-view mirror where GT3 draws it, at the top of the panel.
    if (camera_.shows_mirror())
        draw_mirror(app, ui, SDL_FRect{0, 0, ui.width(), ui.height()}, o, false, false);
    x.submit_stereo();
}

bool RaceScreen::stereo_screen(App& app) const {
    const xr::Session* x = app.xr();
    return x && x->settings().replay_3d && x->canvas_right() && replaying_ && !attract_ && !quick_ && !logger_mode_ &&
           app.gpu() && course_scene_;
}

// 3D replays on the virtual screen: as a 3D film, each eye sees its own picture of the screen,
// the replay drawn from two cameras side by side. The screen itself stays where it is, so cuts,
// pans and the TV cameras are as comfortable as flat; the headset places it for the head every
// frame, while new pictures come every replay_3d_rate-th frame (the same drawing work per
// second as the flat replay: two pictures every other frame).
//
// The depth: what is at the camera's aim (CameraRig::focus: the followed car) stays at the
// screen's depth, the distance behind it; the far distance's two pictures are replay_depth x the
// eyes' distance (6.3 cm) apart on the screen, so it never needs the eyes to look outwards.
void RaceScreen::render_stereo_screen(App& app, Ui& ui) {
    xr::Session& x = *app.xr();
    const xr::Settings& st = x.settings();
    const unsigned every = unsigned(std::clamp(st.replay_3d_rate, 2, 3));
    const bool fresh = !x.stereo_screen_ready() || stereo_frame_ % every == 0;
    ++stereo_frame_;
    if (!fresh) { // the last pictures stay on the screen
        x.submit_stereo_screen(false);
        return;
    }
    SDL_Renderer* r = ui.renderer();
    SDL_Texture* left = SDL_GetRenderTarget(r);
    int lw = 0, lh = 0;
    SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
    SDL_GetRenderLogicalPresentation(r, &lw, &lh, &mode);

    // The pictures' shape as render_drive lays them out (1P, the 2P halves, the original's 4:3 frame).
    const float W = ui.width(), H = ui.height();
    const bool split = two_player_ && field_ && field_->has_second() && !(replaying_ && !replay_settings_.split);
    float aspect = W / H, width_share = 1;
    if (split) {
        if (split_horizontal(app)) aspect = W / (H / 2);
        else aspect = (W / 2) / (8 * H / 11), width_share = 0.5f;
    } else if (original_res_) {
        aspect = 4.0f / 3.0f, width_share = H * 4 / 3 / W;
    }
    // The far distance's disparity across a picture (normalized device units: 2 = its width).
    const float disparity = std::clamp(st.replay_depth, 0.0f, 1.0f) * 2 * 0.063f / std::max(0.4f, st.screen_width * width_share);
    auto set_eye = [&](CameraRig& c, float side) { // side: +1 right eye, -1 left, 0 off
        if (side == 0 || disparity <= 0) return c.set_stereo(0, 0);
        const float focus = std::clamp(c.focus(), 2.0f, 300.0f);
        const float p00 = 1.0f / (std::tan(c.fovy() / 2) * aspect * CameraRig::kPixelAspect);
        const float sep = disparity * focus / p00; // metres between the two cameras
        c.set_stereo(side * sep / 2, side * disparity / 2);
    };

    // The right eye into its own canvas (with the screen canvas's layout), its own views (their
    // trails and dissolves keep their own pictures); then the left eye into the screen's canvas.
    const bool snapshot = dissolve_.snapshot;
    SDL_SetRenderTarget(r, x.canvas_right());
    SDL_SetRenderLogicalPresentation(r, lw, lh, mode);
    ui.clear();
    std::swap(view_, stereo_view_);
    std::swap(view2_, stereo_view2_);
    set_eye(camera_, 1), set_eye(camera2_, 1);
    render_drive(app, ui);
    std::swap(view_, stereo_view_);
    std::swap(view2_, stereo_view2_);
    dissolve_.snapshot = snapshot;
    SDL_SetRenderTarget(r, left);
    set_eye(camera_, -1), set_eye(camera2_, -1);
    render_drive(app, ui);
    set_eye(camera_, 0), set_eye(camera2_, 0);
    x.submit_stereo_screen(true);
}

} // namespace gt3r::race_screen

#endif // GT3R_XR
