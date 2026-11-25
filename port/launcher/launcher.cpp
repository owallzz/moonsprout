#include "mppm/launcher.hpp"
#include "p2_data_dir.hpp"
#include "mppm/frame_policy.hpp"

#include "branding.hpp"
#include "disc.hpp"
#include "menu_bar.hpp"
#include "integration.hpp"
#include "live_apply.hpp"
#include "prelaunch.hpp"
#include "restart.hpp"
#include "scene_bridge.hpp"
#include "settings.hpp"
#include "video.hpp"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/lib/window.hpp>
#include <aurora/rmlui.hpp>
#include <borealis/config.hpp>
#include <borealis/io.hpp>
#include <nlohmann/json.hpp>
#include <p2_config.h>
#include <p2_net.h>
#include "ui/settings_window.hpp"
#include "ui/ui.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string_view>
#include <vector>

namespace mppm::launcher {
namespace {

std::string sCommandLineDisc;
bool sPlayFlag     = false; // --play: restarted from "Play" with pending changes; boot directly
bool sLauncherFlag = false; // --launcher: restarted from "Start Menu"; show it whatever the settings
bool sInitialized  = false;
bool sAnyFrame     = false; // launcher::update ran at least once (a frame was built)
bool sWindowShown  = false; // Aurora leaves the window hidden (startHidden) until reveal_window
ui::Prelaunch* sPrelaunch = nullptr;
std::string sDataDir; // keeps AuroraConfig.userPath / cachePath alive until aurora_initialize copies them
std::vector<borealis::config::Connection> sConnections;

// Test-only switches stay raw environment checks (SETTINGS 1.3): same flag rule as the settings.
bool env_flag(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
}

std::filesystem::path data_dir() {
    const std::string& dir = mppm::data_dir_utf8();
    return dir.empty() ? std::filesystem::path(".") : borealis::io::fs_path_from_utf8(dir);
}

// The command line: `mppm [disc] [--cvar key=value]... [--play | --launcher]`. The disc is only
// recognised as the first argument; other words and unknown options are ignored (Aurora and test
// scripts pass their own, e.g. `--no-disc 1`). Returns the --cvar assignments.
std::vector<std::string> parse_args(int argc, char** argv) {
    std::vector<std::string> cvars;
    if (argc > 1 && argv[1] != nullptr && argv[1][0] != '-' && argv[1][0] != '\0') {
        sCommandLineDisc = argv[1];
    }
    for (int i = 1; i < argc; i++) {
        const std::string_view arg = argv[i] != nullptr ? argv[i] : "";
        if (arg == "--cvar") {
            if (i + 1 < argc && argv[i + 1] != nullptr) {
                cvars.emplace_back(argv[++i]);
            } else {
                std::fprintf(stderr, "mppm: --cvar needs key=value\n");
            }
        } else if (arg.starts_with("--cvar=")) {
            cvars.emplace_back(arg.substr(7));
        } else if (arg == "--play") {
            sPlayFlag = true;
        } else if (arg == "--launcher") {
            sLauncherFlag = true;
        }
    }
    if (sCommandLineDisc.empty()) {
        if (const char* env = std::getenv("MPPM_DISC"); env != nullptr && env[0] != '\0') {
            sCommandLineDisc = env;
        }
    }
    if (sPlayFlag && sLauncherFlag) {
        sPlayFlag = false; // the later restart target wins; both at once means the start menu
    }
    return cvars;
}

AuroraBackend backend_from_id(std::string_view id) {
    struct Entry {
        std::string_view id;
        AuroraBackend backend;
    };
    // Same ids as Dusklight (SETTINGS 2, Graphics Backend).
    static constexpr Entry kBackends[] = {
        {"vulkan", BACKEND_VULKAN}, {"d3d12", BACKEND_D3D12}, {"d3d11", BACKEND_D3D11},
        {"metal", BACKEND_METAL}, {"opengl", BACKEND_OPENGL}, {"opengles", BACKEND_OPENGLES},
        {"webgpu", BACKEND_WEBGPU},
    };
    for (const Entry& e : kBackends) {
        if (e.id == id) {
            return e.backend;
        }
    }
    return BACKEND_AUTO; // "auto", unknown or misspelled: fall back without rewriting the file
}

AuroraLogLevel aurora_log_level(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return LOG_DEBUG;
    case LogLevel::Info:
        return LOG_INFO;
    case LogLevel::Warning:
        return LOG_WARNING;
    case LogLevel::Error:
        return LOG_ERROR;
    }
    return LOG_INFO;
}

// Options Aurora reads once, at aurora_initialize.
void fill_aurora_config(AuroraConfig& config) {
    const auto& s = settings();
    // The window title is the product name; data stays in the "mppm" folder.
    config.appName   = branding::kProductName;
    sDataDir         = data_dir().string();
    config.userPath  = sDataDir.c_str();
    config.cachePath = sDataDir.c_str();
    config.desiredBackend                = backend_from_id(s.backend.graphicsBackend.get());
    config.msaa                          = static_cast<uint32_t>(s.video.msaa.get());
    config.maxTextureAnisotropy          = static_cast<uint16_t>(s.video.anisotropy.get());
    config.vsync                         = s.video.vsync.get();
    config.startFullscreen               = s.video.fullscreen.get();
    config.allowJoystickBackgroundEvents = s.input.backgroundInput.get();
    config.pauseOnFocusLost              = s.game.pauseInBackground.get() && !p2_net_active(); // plan 2.6: lock and code
    config.allowCpuAdapter               = s.video.allowCpuAdapter.get();
    config.windowWidth                   = static_cast<uint32_t>(s.video.windowWidth.get());
    config.windowHeight                  = static_cast<uint32_t>(s.video.windowHeight.get());
    // No saved position: let SDL centre the window (Aurora maps a negative position to
    // SDL_WINDOWPOS_UNDEFINED). Left at 0 it is an explicit position for the client area, and on
    // Windows that puts the title bar above the top of the screen. A relaunch restores the old
    // position itself (live_apply init).
    config.windowPosX                    = -1;
    config.windowPosY                    = -1;
    config.logLevel                      = aurora_log_level(s.tools.logLevel.get());
    // The window maps only once the start menu has a frame to present (present_first_frame), not
    // as an uncleared black surface while the fonts and the menu load.
    config.startHidden = true;
}

// Shows the window Aurora left hidden. Every path calls it before its first aurora_begin_frame
// (a hidden window pauses Aurora): the start menu's first frame, else the end of init.
void reveal_window() {
    if (!sWindowShown) {
        sWindowShown = true;
        aurora::window::show_window();
        // Wait for the map to land: a frame presented before the server has mapped the window is
        // dropped, and the next one only follows Aurora's slow first event pump.
        if (SDL_Window* window = aurora::window::get_sdl_window()) {
            SDL_SyncWindow(window);
        }
    }
}

// The window's close button and Quit leave through AURORA_EXIT, which port/main.cpp handles before
// the launcher sees the event, and exits without running destructors: a setting changed less than
// the autosave delay before (e.g. the first-run Check for Updates toggle) would be lost. An SDL
// event watch runs as the event is queued, on the thread pumping events (the main thread here).
bool flush_on_quit(void* /*userdata*/, SDL_Event* event) {
    if (event->type == SDL_EVENT_QUIT || event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        borealis::config::Registry::global().flush();
    }
    return true;
}

// Settings that apply at runtime through Aurora or the UI, hooked once the window exists.
void bind_runtime_settings() {
    auto& s = settings();
    sConnections.push_back(s.input.backgroundInput.on_change([](bool v) { aurora_set_background_input(v); }));
    // Plan 2.6: besides the settings lock, a harness run or online session never pauses on focus loss.
    sConnections.push_back(s.game.pauseInBackground.on_change([](bool v) { aurora_set_pause_on_focus_lost(v && !p2_net_active()); }));
    sConnections.push_back(s.tools.logLevel.on_change([](LogLevel v) { aurora_set_log_level(aurora_log_level(v)); }));
    sConnections.push_back(s.interface.uiScale.on_change([] { ui::apply_scale(); }));
}

// A disc image dragged onto the window while the start menu is up: the menu shows a drop target
// when it is the top document, and checks the file even under Settings or the first-run card
// (the menu behind says a disc can be dropped; Settings' Disc Image row follows the setting).
void handle_drop(const SDL_Event& event) {
    ui::Prelaunch* menu = sPrelaunch;
    const bool up    = menu != nullptr && menu->active();
    const bool onTop = up && ui::top_document() == menu;
    switch (event.type) {
    case SDL_EVENT_DROP_BEGIN:
    case SDL_EVENT_DROP_POSITION:
        if (onTop) {
            menu->drop_hover(true);
        }
        break;
    case SDL_EVENT_DROP_COMPLETE:
        if (menu != nullptr) {
            menu->drop_hover(false);
        }
        break;
    case SDL_EVENT_DROP_FILE:
        if (up && event.drop.data != nullptr) {
            menu->drop_file(event.drop.data);
        } else if (menu != nullptr) {
            menu->drop_hover(false);
        }
        break;
    default:
        break;
    }
}

// The very first frame, presented before anything pumps events: Aurora's first event pump starts
// its input layer (gamepad discovery), which takes a few hundred ms and would otherwise leave the
// freshly shown window black that long. The start menu's first frame (the backdrop) shows instead.
void present_first_frame() {
    if (sAnyFrame) {
        return;
    }
    // Everything the first frame needs is loaded: map the window now, then draw into it at once.
    reveal_window();
    if (aurora_begin_frame()) {
        update();
        aurora_end_frame();
    }
}

// Mirrors whether the start menu is up into the video state and gP2Config. Launcher windows open
// in game (the F1 bar, Settings, ...) take no aspect override: the UI covers the whole window
// whatever shape the game picture has (Aurora patch 0007), so opening the bar never changes the
// picture.
void sync_prelaunch_state() {
    const bool open = prelaunch_open();
    gP2Config.launcherOpen = open ? 1 : 0;
    video::set_start_menu_override(open);
}

} // namespace

void preinit(AuroraConfig& config, int argc, char** argv) {
    const auto cvars = parse_args(argc, argv);
    settings(); // register the vars before loading
    borealis::config::LoadOptions options;
    options.path = data_dir() / "config.json";
    const auto status = borealis::config::load(std::move(options));
    if (!status && status.code != borealis::config::ErrorCode::FileMissing) {
        // The file is kept (a bad one moves to config.json.bad); defaults apply for this run.
        std::fprintf(stderr, "mppm: settings: %s\n", status.message.c_str());
    }
    // Layers above the file (SETTINGS 1.3): --cvar, then the environment.
    apply_cvar_overrides(cvars);
    apply_env_overrides();
    // Harness runs and online sessions (SETTINGS 14): the netplay overlay locks every Forced and
    // Synced var above all other layers. A harness run locks the Synced ones to their defaults; M5.3b
    // locks them to the host's values at the start barrier.
    if (p2_net_active()) {
        if (net_lock_settings(nlohmann::json::object())) {
            p2_net_set_synced_hash(net_synced_hash(net_synced_settings()));
            std::fprintf(stderr, "mppm: net: settings locked (%d forced, %d synced)\n", net_forced_count(), net_synced_count());
        }
    }
    live::preinit();
    bind_game_config();
    fill_aurora_config(config);
    settings_take_boot_snapshot();
}

bool init(const AuroraInfo& info) {
    // Video state applies even without the UI (it is the only owner of the Aurora video calls).
    // The window is for the display's refresh rate (frame_policy).
    video::init(info.window);
    if (!aurora::rmlui::is_initialized() || !ui::initialize()) {
        std::fprintf(stderr, "mppm: launcher UI unavailable\n");
        reveal_window(); // the game boots without a start menu
        return false;
    }
    ui::apply_scale();
    bind_runtime_settings();
    live::init();
    SDL_AddEventWatch(flush_on_quit, nullptr);
    ui::install_in_game_ui(); // overlay, scrim and F1 bar (menu_bar.hpp), below any start menu
    integration::init();
    sInitialized = true;
    // A direct boot draws no start menu: the game's own first frame follows.
    const BootMode mode = boot_mode();
    if (mode != BootMode::StandaloneMenu && mode != BootMode::LiveMenu) {
        reveal_window();
    }
    return true;
}

void handle_event(const SDL_Event& event) {
    // The frame policy follows the refresh rate of the display the window is on (fps_design 4).
    if (event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED || event.type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED) {
        video::refresh_display();
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        // F11 and Alt+Enter toggle fullscreen anywhere (neither is bound by the keyboard layout).
        const bool altEnter = event.key.scancode == SDL_SCANCODE_RETURN && (event.key.mod & SDL_KMOD_ALT) != 0;
        if (event.key.scancode == SDL_SCANCODE_F11 || altEnter) {
            video::toggle_fullscreen();
            return;
        }
    }
    if (sInitialized) {
        if (event.type >= SDL_EVENT_DROP_FILE && event.type <= SDL_EVENT_DROP_POSITION) {
            handle_drop(event);
        }
        integration::handle_event(event);
        ui::handle_event(event);
    }
}

void handle_aurora_event(const AuroraEvent& event) {
    if (event.type == AURORA_WINDOW_RESIZED) {
        video::on_window_resized(event.windowSize);
    }
    if (sInitialized) {
        integration::handle_aurora_event(event);
    }
}

// Test hook (raw environment, like MPPM_UPDATE_MOCK): MPPM_TEST_DROP=<path> drops that file on the
// window 3 s after the start menu opened, through the same SDL events a real drag-and-drop sends.
void test_drop() {
    static const char* path = std::getenv("MPPM_TEST_DROP");
    static uint64_t openedAt = 0;
    if (path == nullptr || path[0] == '\0' || sPrelaunch == nullptr) {
        return;
    }
    if (openedAt == 0) {
        openedAt = SDL_GetTicks();
        return;
    }
    if (SDL_GetTicks() - openedAt < 3000) {
        return;
    }
    for (const SDL_EventType type : {SDL_EVENT_DROP_BEGIN, SDL_EVENT_DROP_FILE, SDL_EVENT_DROP_COMPLETE}) {
        SDL_Event event{};
        event.drop.type = type;
        event.drop.data = type == SDL_EVENT_DROP_FILE ? path : nullptr;
        handle_drop(event);
    }
    path = nullptr;
}

// Test hook (raw environment): MPPM_TEST_SET="<sec>:<key>=<value>;..." sets each setting's user
// value that many seconds after the first launcher frame, as a choice in Settings would
// (Meta::setUser, so on_change and the live appliers run). Each step logs `mppm: test: set ...`.
void test_set() {
    struct Step {
        uint64_t atMs;
        std::string key;
        std::string value;
    };
    static std::vector<Step> steps = [] {
        std::vector<Step> out;
        const char* env = std::getenv("MPPM_TEST_SET");
        std::string_view rest = env != nullptr ? env : "";
        while (!rest.empty()) {
            const size_t end = rest.find(';');
            const std::string_view item = rest.substr(0, end);
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
            const size_t colon = item.find(':');
            const size_t eq    = item.find('=');
            if (colon == std::string_view::npos || eq == std::string_view::npos || eq < colon) {
                if (!item.empty()) {
                    std::fprintf(stderr, "mppm: test: MPPM_TEST_SET item '%.*s' ignored\n",
                                 static_cast<int>(item.size()), item.data());
                }
                continue;
            }
            const double sec = std::atof(std::string(item.substr(0, colon)).c_str());
            out.push_back({static_cast<uint64_t>(sec * 1000.0), std::string(item.substr(colon + 1, eq - colon - 1)),
                           std::string(item.substr(eq + 1))});
        }
        return out;
    }();
    static uint64_t startMs = 0;
    if (steps.empty()) {
        return;
    }
    if (startMs == 0) {
        startMs = SDL_GetTicks();
    }
    const uint64_t elapsed = SDL_GetTicks() - startMs;
    for (auto it = steps.begin(); it != steps.end();) {
        if (elapsed < it->atMs) {
            ++it;
            continue;
        }
        const Meta* m = find_meta(it->key);
        if (m == nullptr) {
            std::fprintf(stderr, "mppm: test: set %s: unknown key\n", it->key.c_str());
        } else {
            try {
                m->setUser(*m->var, it->value);
                std::fprintf(stderr, "mppm: test: set %s=%s at %llu ms\n", it->key.c_str(), it->value.c_str(),
                             static_cast<unsigned long long>(elapsed));
            } catch (const std::exception& e) {
                std::fprintf(stderr, "mppm: test: set %s=%s: %s\n", it->key.c_str(), it->value.c_str(), e.what());
            }
        }
        it = steps.erase(it);
    }
}

// Test hook (raw environment): MPPM_TEST_PAD="<sec>:<input>[:<ms>];..." drives a virtual SDL
// gamepad, so a run can be played with no window focus and no system-wide input (a hidden desktop,
// a headless machine). Each step presses <input> that many seconds after the first launcher frame
// and holds it <ms> (default 100). <input> is an SDL gamepad button name (a, b, x, y, start, back,
// dpup, dpdown, dpleft, dpright, leftshoulder, rightshoulder, ...) or an axis pushed all the way:
// leftx+, leftx-, lefty+, lefty-, rightx+, ..., lefttrigger, righttrigger. The pad is the only
// controller when physical ones are kept out with SDL's hints (SDL_XINPUT_ENABLED=0 and so on);
// pair it with --cvar input.backgroundInput=true when the window can't have focus.
void test_pad() {
    struct Step {
        uint64_t atMs;
        uint64_t holdMs;
        std::string input;
        int button = -1; // SDL_GamepadButton, or -1 for an axis
        int axis   = -1; // SDL_GamepadAxis
        Sint16 value       = 0;
        bool down          = false;
        uint64_t pressedAt = 0;
    };
    static std::vector<Step> steps = [] {
        std::vector<Step> out;
        const char* env = std::getenv("MPPM_TEST_PAD");
        std::string_view rest = env != nullptr ? env : "";
        while (!rest.empty()) {
            const size_t end = rest.find(';');
            const std::string item(rest.substr(0, end));
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
            const size_t c1 = item.find(':');
            if (c1 == std::string::npos) {
                if (!item.empty()) {
                    std::fprintf(stderr, "mppm: test: MPPM_TEST_PAD item '%s' ignored\n", item.c_str());
                }
                continue;
            }
            const size_t c2 = item.find(':', c1 + 1);
            Step s;
            s.atMs   = static_cast<uint64_t>(std::atof(item.substr(0, c1).c_str()) * 1000.0);
            s.input  = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
            s.holdMs = c2 == std::string::npos ? 100 : std::strtoull(item.c_str() + c2 + 1, nullptr, 10);
            std::string name = s.input;
            char sign        = 0;
            if (!name.empty() && (name.back() == '+' || name.back() == '-')) {
                sign = name.back();
                name.pop_back();
            }
            const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(name.c_str());
            const SDL_GamepadAxis axis     = SDL_GetGamepadAxisFromString(name.c_str());
            if (sign == 0 && button != SDL_GAMEPAD_BUTTON_INVALID) {
                s.button = button;
            } else if (axis != SDL_GAMEPAD_AXIS_INVALID) {
                s.axis  = axis;
                s.value = sign == '-' ? SDL_JOYSTICK_AXIS_MIN : SDL_JOYSTICK_AXIS_MAX;
            } else {
                std::fprintf(stderr, "mppm: test: MPPM_TEST_PAD input '%s' unknown\n", s.input.c_str());
                continue;
            }
            out.push_back(std::move(s));
        }
        return out;
    }();
    static SDL_Joystick* pad = nullptr;
    static uint64_t startMs  = 0;
    if (steps.empty()) {
        return;
    }
    if (pad == nullptr) {
        SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD);
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type        = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes       = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons    = SDL_GAMEPAD_BUTTON_COUNT;
        desc.axis_mask   = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
        desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1u;
        desc.name        = "mppm test pad";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        pad = id != 0 ? SDL_OpenJoystick(id) : nullptr;
        if (pad == nullptr) {
            std::fprintf(stderr, "mppm: test: virtual pad unavailable: %s\n", SDL_GetError());
            steps.clear();
            return;
        }
        startMs = SDL_GetTicks();
        std::fprintf(stderr, "mppm: test: virtual pad attached (%zu steps)\n", steps.size());
        return;
    }
    const uint64_t elapsed = SDL_GetTicks() - startMs;
    for (auto it = steps.begin(); it != steps.end();) {
        Step& s = *it;
        if (!s.down) {
            if (elapsed >= s.atMs) { // held from the frame it is due, for at least one frame
                s.down      = true;
                s.pressedAt = elapsed;
                if (s.button >= 0) {
                    SDL_SetJoystickVirtualButton(pad, s.button, true);
                } else {
                    SDL_SetJoystickVirtualAxis(pad, s.axis, s.value);
                }
                std::fprintf(stderr, "mppm: test: pad %s at %llu ms\n", s.input.c_str(),
                             static_cast<unsigned long long>(elapsed));
            }
            ++it;
            continue;
        }
        if (elapsed < s.pressedAt + s.holdMs) {
            ++it;
            continue;
        }
        if (s.button >= 0) {
            SDL_SetJoystickVirtualButton(pad, s.button, false);
        } else {
            SDL_SetJoystickVirtualAxis(pad, s.axis, 0);
        }
        it = steps.erase(it);
    }
}

void update() {
    if (!sInitialized) {
        return;
    }
    sAnyFrame = true;
    reveal_window(); // safety net: never stay hidden past a frame
    test_drop();
    test_set();
    test_pad();
    ui::update();
    integration::update();
    scene::update();
    live::update();
    borealis::config::Registry::global().update();
    (void)frame_policy(); // follows live edits now, and logs the new policy under tools.logFps
    if (sPrelaunch != nullptr && (sPrelaunch->closed() || sPrelaunch->pending_close())) {
        sPrelaunch = nullptr;
    }
    sync_prelaunch_state();
}

std::string startup_disc() {
    if (!sCommandLineDisc.empty()) {
        return sCommandLineDisc;
    }
    const std::string saved = settings().backend.discPath.get();
    if (!saved.empty() && disc::inspect(saved).state == disc::State::Ready) {
        return saved;
    }
    return {};
}

BootMode boot_mode() {
    if (!sInitialized) {
        return BootMode::Direct; // no UI: the disc must come from the command line
    }
    const bool liveScene = settings().interface.liveBackground.get() && scene::available();
    const bool forced    = env_flag("MPPM_FORCE_LAUNCHER") || sLauncherFlag;
    const bool haveDisc  = !startup_disc().empty();
    // The start menu always comes first; only Skip Start Menu (or the --play restart) boots the
    // game directly. A disc on the command line is just the disc to use, not a skip. Headless test
    // scripts that want the old direct boot set MPPM_SKIP_LAUNCHER=1.
    if (!forced && (sPlayFlag || settings().backend.skipLauncher.get()) && haveDisc) {
        return BootMode::Direct;
    }
    if (!sCommandLineDisc.empty()) {
        // Without the live scene the start menu is shown over the booting game, as before the
        // scene existed.
        return BootMode::LiveMenu;
    }
    return haveDisc && liveScene ? BootMode::LiveMenu : BootMode::StandaloneMenu;
}

void show_prelaunch() {
    if (!sInitialized) {
        return;
    }
    note_game_booted();
    scene::arm();
    if (sPrelaunch != nullptr) {
        // Booted in place behind the standalone menu (live_options 3.6): the same menu, Settings
        // and all, carries on over the game.
        sPrelaunch->game_started();
        sync_prelaunch_state();
        return;
    }
    auto doc   = std::make_unique<ui::Prelaunch>(true);
    sPrelaunch = doc.get();
    ui::push_document(std::move(doc));
    sync_prelaunch_state();
    live::restore_ui();
    present_first_frame();
}

StandaloneResult run_standalone_menu(bool (*frame)()) {
    if (!sInitialized) {
        return {};
    }
    auto doc        = std::make_unique<ui::Prelaunch>(false);
    auto* prelaunch = doc.get();
    sPrelaunch      = prelaunch;
    ui::push_document(std::move(doc));
    sync_prelaunch_state();
    live::restore_ui();
    present_first_frame();
    StandaloneResult result;
    // The disc's inspection, cached by path: the loop asks every frame.
    std::string inspectedPath;
    bool inspectedReady = false;
    int liveFrames      = 0; // frames the in-place boot condition has held
    while (frame()) {
        if (prelaunch->quit_requested()) {
            break;
        }
        // With the live scene, a usable disc boots the game under this same menu at once (DESIGN
        // 6.9, live_options 3.6), whether a disc was just picked or Live Start Menu turned on. The
        // menu is only standalone without both, so the condition holds only after a change.
        const std::string& picked = settings().backend.discPath.get();
        if (picked != inspectedPath) {
            inspectedPath  = picked;
            inspectedReady = !picked.empty() && disc::inspect(picked).state == disc::State::Ready;
        }
        ui::Document* top = ui::top_document();
        const bool menuIdle = prelaunch->idle() && prelaunch->active() &&
                              (top == prelaunch || dynamic_cast<ui::SettingsWindow*>(top) != nullptr);
        const bool wantLive = scene::available() && settings().interface.liveBackground.get() && inspectedReady;
        if (wantLive && menuIdle && !live::relaunching() && !live::pending_relaunch()) {
            // Two frames after it first holds, so the "Applying..." row is on screen before the
            // synchronous part of the boot.
            if (++liveFrames >= 3) {
                std::fprintf(stderr, "mppm: start menu: booting the game in place\n");
                result = {picked, true};
                break;
            }
        } else {
            liveFrames = 0;
        }
        if (prelaunch->launch_requested()) {
            // A command-line disc (MPPM_FORCE_LAUNCHER / --launcher) wins over the remembered one.
            result.disc = sCommandLineDisc.empty() ? picked : sCommandLineDisc;
            break;
        }
    }
    // The disc picked here is the one the game boots: not a pending restart afterwards.
    if (!result.disc.empty() && result.disc == settings().backend.discPath.get()) {
        settings_rebase_boot_snapshot(settings().backend.discPath);
    }
    if (result.keepMenu) {
        // The menu stays up, over the game that boots behind it (show_prelaunch reuses it).
        borealis::config::Registry::global().flush();
        sync_prelaunch_state();
        return result;
    }
    // Everything opened over the menu goes too: a disc picked in Settings with Live Start Menu on
    // boots the game behind a new start menu (show_prelaunch, which starts with `intro-skip`), and
    // the settings window, file dialog result or any modal of the old one must not stay on top.
    // The settings window saves on hide; nothing restart-only can be pending before the boot.
    ui::close_all_documents();
    prelaunch->force_hide(true);
    sPrelaunch = nullptr;
    borealis::config::Registry::global().flush();
    sync_prelaunch_state();
    return result;
}

bool prelaunch_open() {
    return sPrelaunch != nullptr && sPrelaunch->active();
}

bool hold_game() {
    // Harness runs (M5.1a): each return from the hold loop into the game runs a full update, an
    // extra sim tick, so the game is never held there.
    if (p2_net_active()) {
        integration::set_game_held(false);
        return false;
    }
    // Never behind the start menu: the live scene keeps animating there (SETTINGS 6).
    const bool hold = sInitialized && settings().game.pauseInMenu.get() && game_booted() && !prelaunch_open() &&
                      !ui::is_prelaunch_open() && ui::any_document_visible();
    integration::set_game_held(hold);
    return hold;
}

bool log_fps() {
    return settings().tools.logFps.get();
}

void shutdown() {
    borealis::config::Registry::global().flush();
    if (sInitialized) {
        ui::shutdown();
        sInitialized = false;
    }
}

} // namespace mppm::launcher
