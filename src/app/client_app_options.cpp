// ClientApp: the options the Options window (ui/<pack>/options.lua) shows and changes, by name, and the window /
// frame settings (window mode, resolution, vertical sync, frame caps, field of view, UI scale).

#include "app/client_app_internal.h"

namespace nmsc::app {

namespace client_internal {

namespace {

constexpr const char* kWindowModes[] = {"windowed", "borderless", "fullscreen"};
constexpr const char* kVsyncModes[] = {"off", "on", "adaptive"};
// game::camera_fov: below an aspect of 2 the horizontal field of view is fov_units x 0.703125 degrees.
constexpr float kDegreesPerFovUnit = 360.0f / 512.0f;

int index_of(const char* const* names, int count, const std::string& name, int fallback) {
    for (int i = 0; i < count; ++i)
        if (name == names[i]) return i;
    return fallback;
}

// The gamepad's numbers as options (gamepad_<setting>, game/gamepad_bindings.h GamepadSettings; a client extension):
// whole steps, so the thresholds are in percent and the camera speed in tenths of a radian a second; booleans 0 / 1
// and the cursor speed (pixels a second) as they are. 0 for a name that is not a gamepad setting.
float gamepad_option_scale(std::string_view setting) {
    if (setting == "deadzone" || setting == "trigger_threshold" || setting == "move_threshold" ||
        setting == "strafe_threshold")
        return 100.0f;
    if (setting == "camera_speed") return 10.0f;
    auto names = game::GamepadSettings::names();
    return std::find(names.begin(), names.end(), setting) != names.end() ? 1.0f : 0.0f;
}

// display.resolution: "WxH" (none: 0 x 0).
platform::DisplayMode parse_resolution(const std::string& s) {
    platform::DisplayMode m;
    if (std::sscanf(s.c_str(), "%dx%d", &m.width, &m.height) != 2) m = {};
    return m;
}

} // namespace

std::optional<int> ClientApp::video_option(std::string_view name) const {
    if (name == "window_mode") return static_cast<int>(window_.mode());
    if (name == "resolution") {
        // The display mode the window is at: the fullscreen mode, or the windowed size when it is one of them
        // (-1: a size of its own, or borderless at the desktop's).
        auto modes = window_.display_modes();
        platform::DisplayMode at = parse_resolution(config_.get_string("display.resolution", ""));
        if (window_.mode() == platform::WindowMode::Windowed) {
            platform::WindowGeometry g = window_.geometry();
            at = {g.width, g.height, 0.0f};
        } else if (window_.mode() == platform::WindowMode::Borderless) {
            at = window_.desktop_mode();
        }
        for (std::size_t i = 0; i < modes.size(); ++i)
            if (modes[i].width == at.width && modes[i].height == at.height) return static_cast<int>(i);
        return -1;
    }
    if (name == "vsync") return static_cast<int>(vk_.vsync());
    if (name == "max_fps") return opts_.max_fps;
    if (name == "background_fps") return background_fps_;
    if (name == "fov") return static_cast<int>(std::lround(camera_.fov_units * kDegreesPerFovUnit));
    if (name == "ui_scale") return std::clamp(config_.get_int("ui.scale", 0), 0, 300);
    if (name == "ui_scale_now") return static_cast<int>(std::lround(ui::ui_scale() * 100.0f));
    // The user interface: its index in option_choices("ui_name") (the server's, at the next login), and the running
    // one's.
    auto ui_index = [&](std::string_view id) {
        for (std::size_t i = 0; i < ui_packs_.size(); ++i)
            if (ui_packs_[i].id == id) return static_cast<int>(i);
        return -1;
    };
    if (name == "ui_name") return ui_index(wanted_ui_pack());
    if (name == "ui_name_now") return ui_index(ui_name_);
    return std::nullopt;
}

bool ClientApp::set_video_option(std::string_view name, int value) {
    if (name == "window_mode") {
        int m = std::clamp(value, 0, 2);
        config_.set("display.window_mode", kWindowModes[m]);
        platform::DisplayMode size = parse_resolution(config_.get_string("display.resolution", ""));
        // Back to a window: the size it had before (the window keeps it), not the fullscreen resolution.
        window_.set_mode(static_cast<platform::WindowMode>(m), m == 2 ? size : platform::DisplayMode{});
    } else if (name == "resolution") {
        auto modes = window_.display_modes();
        if (value < 0 || value >= static_cast<int>(modes.size())) return true;
        const platform::DisplayMode& m = modes[static_cast<std::size_t>(value)];
        config_.set("display.resolution", std::format("{}x{}", m.width, m.height));
        if (window_.mode() != platform::WindowMode::Borderless) window_.set_mode(window_.mode(), m);
    } else if (name == "vsync") {
        int v = std::clamp(value, 0, 2);
        config_.set("display.vsync", kVsyncModes[v]);
        vk_.set_vsync(static_cast<render::VkContext::Vsync>(v));
    } else if (name == "max_fps" || name == "background_fps") {
        int fps = value <= 0 ? 0 : std::clamp(value, 10, 1000);
        (name == "max_fps" ? opts_.max_fps : background_fps_) = fps;
        config_.set_int(name == "max_fps" ? "display.max_fps" : "display.background_fps", fps);
    } else if (name == "fov") {
        // Degrees across (below an aspect of 2); below 0 the original's 90 units (63.3 degrees).
        float units = value < 0 ? game::kStockFovUnits : std::clamp(static_cast<float>(value), 45.0f, 120.0f) / kDegreesPerFovUnit;
        camera_.fov_units = units;
        config_.set("display.fov", value < 0 ? std::string() : std::format("{:.2f}", units));
    } else if (name == "ui_name") {
        if (value < 0 || value >= static_cast<int>(ui_packs_.size())) return true;
        const std::string& id = ui_packs_[static_cast<std::size_t>(value)].id;
        // The saved server's own (its server.ini, as the login screen picks it); a test login's, ui.name.
        std::optional<ServerProfile> saved;
        if (!server_profile_.id.empty()) saved = load_profile(server_profile_.id, login_view_.servers_dir);
        if (saved) {
            saved->ui = id;
            if (!save_profile(*saved, login_view_.servers_dir)) return true;
            server_profile_.ui = id;
            for (auto& p : login_view_.profiles)
                if (p.id == saved->id) p.ui = id;
            if (ui::selected_profile_id(login_view_) == saved->id) login_view_.ui = id;
            return true;
        }
        config_.set("ui.name", id);
    } else if (name == "ui_scale") {
        config_.set_int("ui.scale", value <= 0 ? 0 : std::clamp(value, 50, 300)); // ui_scale_setting, at the next start
    } else {
        return false;
    }
    config_.save(config_path_);
    return true;
}

void ClientApp::apply_video_options() {
    vk_.set_vsync(static_cast<render::VkContext::Vsync>(
        index_of(kVsyncModes, 3, config_.get_string("display.vsync", "on"), 1)));
    if (opts_.max_fps <= 0) opts_.max_fps = std::max(0, config_.get_int("display.max_fps", 0)); // --max-fps first
    background_fps_ = std::max(0, config_.get_int("display.background_fps", 0));
    float fov = config_float("display.fov", 0.0f);
    camera_.fov_units = fov > 0.0f ? fov : game::kStockFovUnits;
    int mode = index_of(kWindowModes, 3, config_.get_string("display.window_mode", "windowed"), 0);
    // Screenshot runs stay in a window of the size asked for.
    if (mode != 0 && opts_.screenshot.empty())
        window_.set_mode(static_cast<platform::WindowMode>(mode),
                         mode == 2 ? parse_resolution(config_.get_string("display.resolution", "")) : platform::DisplayMode{});
}

bool ClientApp::choose_asset_folder() {
    AssetFolderResolution r = resolve_asset_folder(opts_.game_dir, config_, {}, platform::data_dir());
    std::optional<std::filesystem::path> folder;
    if (r.source != AssetFolderSource::Picker) {
        folder = r.path;
        AssetFolderCheck check = validate_asset_folder(r.path);
        if (!check.ok) log::warn("asset folder {}: {}", r.path.string(), describe(check));
        // --game-dir / EQ_GAME_DIR is used as asked; the player is told it holds Hailstone's own files.
        if (std::string why = data_folder_conflict(r.path, platform::data_dir()); !why.empty())
            log::error("asset folder {}: {}", r.path.string(), why);
    } else {
        if (r.reason.empty()) log::info("asset folder: none chosen yet, asking");
        else log::info("asset folder: {} {}", r.reason, r.check.is_directory ? describe(r.check) : "");
        ui::AssetPickerView view;
        view.reason = r.reason;
        view.data_folder = platform::data_dir();
        view.recent = recent_asset_folders(config_);
        if (!r.path.empty()) ui::set_picker_folder(view, r.path.string());
        else if (!view.recent.empty()) ui::set_picker_folder(view, view.recent.front());
        folder = ui::run_asset_picker(window_, vk_, ui_, view, opts_.frames, opts_.screenshot);
        if (view.recent != recent_asset_folders(config_)) config_.set("assets.recent", join_recent_folders(view.recent));
        if (folder) remember_asset_folder(config_, *folder);
        config_.save(config_path_);
        if (!folder) return false;
    }
    try {
        game_.set_base(*folder);
    } catch (const std::exception& e) {
        log::error("asset folder: {}", e.what());
        return false;
    }
    asset_source_ = r.source;
    log::info("asset folder {}{}", folder->string(), r.source == AssetFolderSource::Override ? " (--game-dir / EQ_GAME_DIR)" : "");
    return true;
}

void ClientApp::change_asset_folder_at_login() {
    // Only on the idle login screen: no login, connection or zone holds what the old folder's files gave them.
    if (screen_ != Screen::Login || login_ || conn_ || asset_source_ == AssetFolderSource::Override ||
        !asset_folder_pick_requested(config_))
        return;
    log::info("asset folder: changing it ({} now)", game_.root().string());
    ui::AssetPickerView view;
    view.reason = "Choose the asset folder to play with.";
    view.data_folder = platform::data_dir();
    view.recent = recent_asset_folders(config_);
    view.cancel = true;
    ui::set_picker_folder(view, game_.root().string());
    std::optional<std::filesystem::path> folder = ui::run_asset_picker(window_, vk_, ui_, view);
    if (view.recent != recent_asset_folders(config_)) config_.set("assets.recent", join_recent_folders(view.recent));
    if (view.window_closed) {
        config_.save(config_path_); // assets.pick stays: the picker shows at the next start
        quit_ = true;
        return;
    }
    request_asset_folder_pick(config_, false);
    if (!folder || same_folder(std::filesystem::absolute(*folder), game_.root())) {
        if (folder) remember_asset_folder(config_, *folder);
        config_.save(config_path_);
        refresh_game_folders();
        return;
    }
    switch_asset_folder(*folder);
}

bool ClientApp::switch_asset_folder(const std::filesystem::path& folder) {
    // The folder is remembered only once the game runs with it: one that went away after it was checked (removed,
    // unmounted) leaves assets.path naming the folder in use.
    bool ok = true;
    try {
        game_.set_base(folder);
    } catch (const std::exception& e) {
        log::error("asset folder: {}", e.what());
        ok = false;
    }
    if (ok) remember_asset_folder(config_, folder);
    config_.save(config_path_);
    if (ok) {
        log::info("asset folder {}", game_.root().string());
        apply_classic_folder(); // the classic folder's preferred files depend on the base being live
        reload_server_files();
    }
    refresh_game_folders();
    return ok;
}

void ClientApp::refresh_game_folders() {
    auto& folders = login_view_.game_folders;
    folders.clear();
    login_view_.game_folder_fixed = asset_source_ == AssetFolderSource::Override;
    if (!game_.has_base()) return;
    folders.push_back({game_.root().string(), is_live_asset_folder(game_), true});
    if (login_view_.game_folder_fixed) return;
    for (const std::string& recent : recent_asset_folders(config_)) {
        if (same_folder(recent, game_.root())) continue;
        AssetFolderCheck check = validate_asset_folder(std::filesystem::path(recent));
        folders.push_back({recent, check.live, check.ok});
    }
}

void ClientApp::apply_classic_folder() {
    std::string folder = opts_.classic_dir.empty() ? config_.get_string("assets.classic", "") : opts_.classic_dir;
    std::optional<std::filesystem::path> classic;
    if ((asset_source_ != AssetFolderSource::Override || !opts_.classic_dir.empty()) && !folder.empty()) {
        if (check_classic_folder(folder).ok) classic = std::filesystem::path(folder);
        else log::warn("classic folder {}: no classic zone files there; not used", folder);
    }
    if (classic == game_.classic()) return;
    game_.set_classic(classic, is_live_asset_folder(game_) ? GameDir::PreferRule(classic_preferred_file)
                                                           : GameDir::PreferRule());
    if (classic) log::info("classic folder {}", classic->string());
}

const std::vector<ui::FontFace>& ClientApp::font_faces() const {
    if (!font_faces_) {
        auto dirs = ui::system_font_dirs();
        font_faces_ = ui::scan_font_faces(dirs);
        log::info("fonts: {} faces in {} folders", font_faces_->size(), dirs.size());
    }
    return *font_faces_;
}

void ClientApp::apply_font_face() {
    std::string face = config_.get_string("ui.font_face", "");
    if (face.empty()) {
        ui_.set_font_face({}, {});
        return;
    }
    ui::FontFiles files = ui::find_font_family(font_faces(), face);
    if (files.regular.empty()) files.regular = files.bold; // a face with only a bold file
    if (files.regular.empty()) log::warn("fonts: no face \"{}\"; the default font stays", face);
    else {
        // As UTF-8: path::string() throws on Windows for a file name the ANSI code page cannot show.
        auto text = [](const std::filesystem::path& p) {
            const std::u8string u = p.u8string();
            return std::string(u.begin(), u.end());
        };
        log::info("fonts: {} ({}{})", face, text(files.regular), files.bold.empty() ? "" : ", " + text(files.bold));
    }
    ui_.set_font_face(files.regular, files.bold);
}

std::vector<std::string> ClientApp::option_choices(std::string_view name) const {
    std::vector<std::string> out;
    // The Options window's font list (EnumerateSystemFonts): "Default" (the Arial-class font), then every family.
    if (name == "font_face") {
        out.push_back("Default");
        for (auto& f : ui::font_families(font_faces())) out.push_back(f);
    }
    // The asset folder the game runs with (one entry; "asset_folder_pick" changes it on the login screen).
    if (name == "asset_folder") out.push_back(game_.root().string());
    if (name == "ui_name")
        for (const auto& p : ui_packs_) out.push_back(p.name);
    if (name == "ui_name_info") // each one's description and input, as "description\ninput"
        for (const auto& p : ui_packs_) out.push_back(p.description + "\n" + p.input);
    if (name == "resolution")
        for (const auto& m : window_.display_modes())
            out.push_back(m.refresh > 0.0f ? std::format("{} x {} ({:.0f} Hz)", m.width, m.height, m.refresh)
                                           : std::format("{} x {}", m.width, m.height));
    return out;
}

std::optional<int> ClientApp::option_value(std::string_view o) const {
    // The game options (game/client_options.h): the character's, so only with a connection.
    if (auto id = game::find_client_option(o)) return conn_ ? std::optional<int>(conn_->client_options().get(*id)) : std::nullopt;
    // The boxes of the character's own state (auto split, auto consent, show helm): once the profile is in.
    if (conn_)
        if (auto v = conn_->character_setting(o)) return v;
    // The pet window's PopupOnPet (COptionsWnd +0x250 shows CPetInfoWnd +0x765): the window keeps it in the UI ini.
    if (o == "pet_window_popup")
        return conn_ && eq_api_ ? eq_api_->window_setting("PetInfoWindow", "popup") : std::nullopt;
    if (auto d = display_option(o)) return d;
    if (auto v = video_option(o)) return v;
    if (o == "pc_names") return pc_names_ ? 1 : 0;
    if (o == "font_face") {
        // The face's place in option_choices("font_face"): 0 the default, -1 one not in the list.
        std::string face = config_.get_string("ui.font_face", "");
        if (face.empty()) return 0;
        auto choices = option_choices("font_face");
        for (std::size_t i = 1; i < choices.size(); ++i)
            if (game::same_no_case(choices[i], face)) return static_cast<int>(i);
        return -1;
    }
    // The story window's eqclient.ini [News] LastRead (a file time) and Automatic, here hailstone.ini news.*.
    if (o == "news_last_read") return config_.get_int("news.last_read", 0);
    if (o == "news_automatic") return config_.get_bool("news.automatic", false) ? 1 : 0;
    if (o == "npc_names") return npc_names_ ? 1 : 0;
    // Hailstone: the hot button states the original leaves out (game/hot_button_state.h), on by default.
    if (o == "hotbutton_extras") return config_.get_bool("ui.hotbutton_extras", true) ? 1 : 0;
    // Hailstone: the layout copy window offered at a character's first entry (app/lua_eq_layout.cpp), on by default.
    if (o == "offer_layout_copy") return config_.get_bool("ui.offer_layout_copy", true) ? 1 : 0;
    // Hailstone: Tab's NPC cycle leaves out players' pets and mercenaries (game/targeting.h), on by default.
    if (o == "tab_skip_pets") return config_.get_bool("target.skip_pets", true) ? 1 : 0;
    // Hailstone: auto attack stays on past the target's death while the extended target list holds a spawn
    // (net::GameConnection::set_attack_until_xtarget_clear), off by default.
    if (o == "attack_until_xtarget_clear") return config_.get_bool("combat.attack_until_xtarget_clear", false) ? 1 : 0;
    // Hailstone: with no target, an NPC on the extended target list that attacks the player becomes the target
    // (net::GameConnection::set_auto_target_xtarget), on by default.
    if (o == "auto_target_xtarget") return config_.get_bool("combat.auto_target_xtarget", true) ? 1 : 0;
    // Hailstone: a camp logs out at once (net::GameConnection::set_fast_camp), off by default.
    if (o == "fast_camp") return config_.get_bool("game.fast_camp", false) ? 1 : 0;
    // Hailstone fork: /stick may move the player (Navigator::set_stick_allowed), off by default.
    if (o == "allow_stick") return config_.get_bool("nav.allow_stick", false) ? 1 : 0;
    // Hailstone fork: the Auto Combat add-on (the "autocombat" script) may run (mq::ScriptHost::set_blocked), off by
    // default.
    if (o == "face_on_attack") return config_.get_bool("combat.face_on_attack", false) ? 1 : 0;
    if (o == "allow_autocombat") return config_.get_bool("scripts.allow_autocombat", false) ? 1 : 0;
    if (o == "clip_plane") return opts_.clip_plane;
    if (o == "max_camera_distance") return static_cast<int>(std::lround(max_camera_distance_));
    if (o == "mouse_invert_y") return mouse_invert_y_ ? 1 : 0;
    if (o == "mouse_wheel_zoom") return mouse_wheel_zoom_ ? 1 : 0;
    if (o == "mouse_sensitivity") return mouse_sensitivity_;
    if (o.starts_with("gamepad_") && gamepad_)
        if (float scale = gamepad_option_scale(o.substr(8)); scale > 0.0f)
            if (auto v = gamepad_->setting(o.substr(8))) return static_cast<int>(std::lround(*v * scale));
    // 1: the asset-folder picker shows on the login screen, or at the next start (assets.pick); 1: this start's
    // folder came from --game-dir or EQ_GAME_DIR, which the picker does not change.
    if (o == "asset_folder_pick") return asset_folder_pick_requested(config_) ? 1 : 0;
    if (o == "asset_folder_override") return asset_source_ == AssetFolderSource::Override ? 1 : 0;
    audio::AudioSettings s = audio_ ? audio_->settings() : audio::load_audio_settings(config_);
    if (o == "master") return s.master;
    if (o == "music") return s.music;
    if (o == "effects") return s.effects;
    if (o == "realism") return s.realism;
    if (o == "environment") return s.environment ? 1 : 0;
    if (o == "combat_music") return s.combat_music ? 1 : 0;
    if (o == "mute") return s.mute ? 1 : 0;
    if (o == "reverb") return s.reverb ? 1 : 0;
    if (o == "reverse_sound") return s.reverse_sound ? 1 : 0;
    return std::nullopt;
}

void ClientApp::set_option_value(std::string_view o, int v) {
    if (auto id = game::find_client_option(o)) {
        if (conn_) conn_->change_client_option(*id, v);
        return;
    }
    if (conn_ && conn_->set_character_setting(o, v)) return;
    if (o == "pet_window_popup") {
        // GeneralPageWndNotification's +0x250: CPetInfoWnd::SetShowOnSummon (only with a pet window).
        if (eq_api_) eq_api_->set_window_setting("PetInfoWindow", "popup", v);
        return;
    }
    if (set_display_option(o, v) || set_video_option(o, v)) return;
    if (o == "font_face") {
        // GeneralPageWndNotification's font button: the face set and saved (Fonts / Font.<set>.0).
        auto choices = option_choices("font_face");
        if (v < 0 || v >= static_cast<int>(choices.size())) return;
        config_.set("ui.font_face", v == 0 ? std::string() : choices[static_cast<std::size_t>(v)]);
        config_.save(config_path_);
        apply_font_face();
        return;
    }
    if (o == "offer_layout_copy") {
        config_.set_bool("ui.offer_layout_copy", v != 0);
        config_.save(config_path_);
        return;
    }
    if (o == "hotbutton_extras") {
        config_.set_bool("ui.hotbutton_extras", v != 0);
        config_.save(config_path_);
        return;
    }
    if (o == "tab_skip_pets") {
        config_.set_bool("target.skip_pets", v != 0);
        config_.save(config_path_);
        return;
    }
    if (o == "attack_until_xtarget_clear") {
        config_.set_bool("combat.attack_until_xtarget_clear", v != 0);
        config_.save(config_path_);
        if (conn_) conn_->set_attack_until_xtarget_clear(v != 0);
        return;
    }
    if (o == "auto_target_xtarget") {
        config_.set_bool("combat.auto_target_xtarget", v != 0);
        config_.save(config_path_);
        if (conn_) conn_->set_auto_target_xtarget(v != 0);
        return;
    }
    if (o == "fast_camp") {
        config_.set_bool("game.fast_camp", v != 0);
        config_.save(config_path_);
        if (conn_) conn_->set_fast_camp(v != 0);
        return;
    }
    if (o == "allow_stick") {
        config_.set_bool("nav.allow_stick", v != 0);
        config_.save(config_path_);
        if (nav_) nav_->set_stick_allowed(v != 0);
        return;
    }
    if (o == "face_on_attack") {
        config_.set_bool("combat.face_on_attack", v != 0);
        config_.save(config_path_);
        return;
    }
    if (o == "allow_autocombat") {
        config_.set_bool("scripts.allow_autocombat", v != 0);
        config_.save(config_path_);
        if (automation_) automation_->scripts().set_blocked("autocombat", v == 0);
        return;
    }
    if (o == "news_last_read" || o == "news_automatic") {
        if (o == "news_last_read") config_.set_int("news.last_read", v);
        else config_.set_bool("news.automatic", v != 0);
        config_.save(config_path_);
        return;
    }
    if (o == "pc_names" || o == "npc_names") {
        (o == "pc_names" ? pc_names_ : npc_names_) = v != 0;
        config_.set_bool(o == "pc_names" ? "display.pc_names" : "display.npc_names", v != 0);
        config_.save(config_path_);
        return;
    }
    if (o == "mouse_invert_y" || o == "mouse_wheel_zoom") {
        (o == "mouse_invert_y" ? mouse_invert_y_ : mouse_wheel_zoom_) = v != 0;
        config_.set_bool(o == "mouse_invert_y" ? "mouse.invert_y" : "mouse.wheel_zoom", v != 0);
        config_.save(config_path_);
        return;
    }
    if (o == "mouse_sensitivity") {
        mouse_sensitivity_ = std::clamp(v, 0, 20);
        config_.set("mouse.sensitivity", std::to_string(mouse_sensitivity_));
        config_.save(config_path_);
        return;
    }
    if (o.starts_with("gamepad_") && gamepad_)
        if (float scale = gamepad_option_scale(o.substr(8)); scale > 0.0f) {
            gamepad_->set_setting(o.substr(8), static_cast<float>(v) / scale); // saved by Gamepad
            return;
        }
    if (o == "asset_folder_pick") {
        request_asset_folder_pick(config_, v != 0);
        config_.save(config_path_);
        return;
    }
    if (o == "clip_plane") {
        // ClipPlane and CDisplay::SetYon: the view distance follows at once.
        opts_.clip_plane = std::clamp(v, 0, 20);
        view_ = game::compute_view_distance(zone_fog_, opts_.clip_plane);
        config_.set("display.clip_plane", std::to_string(opts_.clip_plane));
        config_.save(config_path_);
        return;
    }
    if (o == "max_camera_distance") {
        // Hailstone: how far the mouse wheel zooms out; a camera already farther comes in to it.
        int units = std::clamp(v, kMinCameraZoomSetting, kMaxCameraZoomSetting);
        max_camera_distance_ = static_cast<float>(units);
        camera_distance_ = std::min(camera_distance_, max_camera_distance_);
        config_.set_int("display.max_camera_distance", units);
        config_.save(config_path_);
        return;
    }
    audio::AudioSettings s = audio_ ? audio_->settings() : audio::load_audio_settings(config_);
    int percent = std::clamp(v, 0, 100);
    if (o == "master") s.master = percent;
    else if (o == "music") s.music = percent;
    else if (o == "effects") s.effects = percent;
    else if (o == "realism") s.realism = percent;
    else if (o == "environment") s.environment = v != 0;
    else if (o == "combat_music") s.combat_music = v != 0;
    else if (o == "mute") s.mute = v != 0;
    else if (o == "reverb") s.reverb = v != 0;
    else if (o == "reverse_sound") s.reverse_sound = v != 0;
    else return;
    if (audio_) audio_->set_settings(s);
    audio::store_audio_settings(s, config_);
    config_.save(config_path_);
}

} // namespace client_internal

} // namespace nmsc::app
