// Dreamcast minimal UI system
//
// Replaces the RmlUi-based UI with a simple PVR-based text/quad renderer
// for the Dreamcast. Provides basic menu navigation using the controller
// and renders text using a built-in bitmap font.
//
// The Dreamcast has no mouse, so all UI is navigated with the D-pad
// and face buttons.

#ifdef DREAMCAST

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <functional>
#include <vector>
#include <list>
#include <filesystem>

#include <kos.h>
#include <dc/pvr.h>
#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

#include "recomp_ui.h"
#include "recomp_input.h"
#include "zelda_config.h"
#include "zelda_sound.h"
#include "dreamcast_platform.h"

namespace {

// ── Bitmap font rendering via KOS BIOS font ─────────────────────────
// The Dreamcast BIOS includes a built-in 12x24 font that we can use
// for basic text rendering without loading any external assets.

struct MenuEntry {
    std::string label;
    std::function<void()> action;
    bool enabled;
    // Option entries: when value_fn is set the entry renders as
    // "Label: < value >" and D-pad left/right invoke adjust_fn(-1 / +1) to
    // change the underlying setting. Plain entries leave both empty and use
    // `action` (fired on A / Start) instead.
    std::function<std::string()> value_fn;
    std::function<void(int)> adjust_fn;
};

struct MenuState {
    std::string title;
    std::vector<MenuEntry> entries;
    int selected_index;
    bool visible;
    // Invoked when the user presses B (cancel) on a choice prompt; empty for
    // menus where B simply dismisses.
    std::function<void()> cancel_action;
};

static MenuState current_menu{};

static recomp::GameInput scanning_input = recomp::GameInput::COUNT;
// Also defined in dc_input.cpp, redefined here for setting bindings
constexpr uint32_t DC_INPUT_TYPE_BUTTON = 100;
constexpr uint32_t DC_INPUT_TYPE_TRIGGER = 101;
constexpr int32_t DC_L_TRIGGER = 0;
constexpr int32_t DC_R_TRIGGER = 1;

// Colors (ARGB packed)
constexpr uint32_t COLOR_WHITE      = 0xFFFFFFFF;
constexpr uint32_t COLOR_YELLOW     = 0xFFFFFF00;
constexpr uint32_t COLOR_GRAY       = 0xFF808080;
constexpr uint32_t MENU_PANEL_ARGB  = 0xC0000000; // 75% opaque black

// Font metrics (BIOS font)
constexpr int FONT_CHAR_H = 24;
constexpr int MENU_PADDING = 20;
constexpr int MENU_ITEM_SPACING = 4;

void draw_bios_text(int x, int y, uint32_t color, const char* text) {
    // Convert ARGB32 color to RGB565 for the Dreamcast framebuffer.
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    uint16_t fg565 = static_cast<uint16_t>(
        ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    // Opaque draw over a solid black cell keeps text legible over arbitrary
    // game frames. (KOS BIOS font API: colors are set separately from the draw.)
    bfont_set_foreground_color(fg565);
    bfont_set_background_color(0x0000u);
    bfont_draw_str(vram_s + y * DC_SCREEN_WIDTH + x, DC_SCREEN_WIDTH, 1 /*opaque*/,
                   const_cast<char*>(text));
}

// A fixed ContextId slot for the single Dreamcast menu context.
constexpr uint32_t DC_MENU_CONTEXT_SLOT = 1;
constexpr uint32_t DC_NULL_CONTEXT_SLOT = 0;

// ── Full-screen error display helpers ────────────────────────────────
// Drawn straight to the framebuffer with the BIOS font, so errors are
// visible even before the PVR renderer exists (e.g. missing ROM at boot).

// 12 px per BIOS-font glyph; leave a margin on both sides.
constexpr size_t ERROR_WRAP_COLUMNS = 48;
constexpr int ERROR_MAX_LINES = 12;

void fill_framebuffer(uint16_t color565) {
    uint16_t* fb = vram_s;
    for (int i = 0; i < DC_SCREEN_WIDTH * DC_SCREEN_HEIGHT; i++) {
        fb[i] = color565;
    }
}

// Raw button state of the first controller, bypassing the game input system
// (which may not be polling yet when a fatal error is shown).
uint32_t poll_raw_buttons() {
    maple_device_t* cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (cont == nullptr) {
        return 0;
    }
    cont_state_t* state = reinterpret_cast<cont_state_t*>(maple_dev_status(cont));
    return (state != nullptr) ? state->buttons : 0;
}

// Greedy word wrap into at most ERROR_MAX_LINES lines.
std::vector<std::string> wrap_text(const char* text, size_t max_columns) {
    std::vector<std::string> lines;
    std::string current;
    std::string word;

    auto flush_word = [&]() {
        if (word.empty()) {
            return;
        }
        if (!current.empty() && current.size() + 1 + word.size() > max_columns) {
            lines.push_back(current);
            current.clear();
        }
        if (!current.empty()) {
            current += ' ';
        }
        // Hard-split words longer than a full line.
        while (word.size() > max_columns) {
            lines.push_back(word.substr(0, max_columns));
            word.erase(0, max_columns);
        }
        current += word;
        word.clear();
    };

    for (const char* p = text; *p != '\0'; p++) {
        if (*p == '\n') {
            flush_word();
            lines.push_back(current);
            current.clear();
        } else if (*p == ' ' || *p == '\t') {
            flush_word();
        } else {
            word += *p;
        }
    }
    flush_word();
    if (!current.empty()) {
        lines.push_back(current);
    }
    if (lines.size() > static_cast<size_t>(ERROR_MAX_LINES)) {
        lines.resize(ERROR_MAX_LINES);
        lines.back() += " ...";
    }
    return lines;
}

// One-shot filter for librecomp's spurious "stored ROM" error (see
// dc_suppress_next_stored_rom_error() in dreamcast_platform.h).
std::atomic<bool> suppress_stored_rom_error{false};

// ── Notification toast state ─────────────────────────────────────────
// open_notification() messages are shown for a few seconds at the bottom of
// the screen instead of being dropped on stdout.
std::mutex toast_mutex;
std::string toast_text;
int toast_frames_remaining = 0;
constexpr int TOAST_DURATION_FRAMES = 150; // ~5 s at 30 Hz (matches PVR frame limiter)

} // anonymous namespace

// ── recompui namespace ───────────────────────────────────────────────
// Implements the recomp_ui.h interface with minimal Dreamcast-appropriate
// behavior. SDL events, RmlUi elements, and keyboard input are all absent.

namespace recompui {

// ── Context management ───────────────────────────────────────────────

void show_context(ContextId /*context*/, std::string_view /*param*/) {
    current_menu.visible = true;
}

void hide_context(ContextId /*context*/) {
    current_menu.visible = false;
}

void hide_all_contexts() {
    current_menu.visible = false;
}

bool is_context_shown(ContextId /*context*/) {
    return current_menu.visible;
}

bool is_context_capturing_input() {
    return current_menu.visible;
}

bool is_context_capturing_mouse() {
    return false; // No mouse on Dreamcast
}

bool is_any_context_shown() {
    return current_menu.visible;
}

ContextId try_close_current_context() {
    current_menu.visible = false;
    return ContextId{DC_NULL_CONTEXT_SLOT};
}

ContextId get_launcher_context_id() {
    return ContextId{DC_MENU_CONTEXT_SLOT};
}

ContextId get_config_context_id() {
    return ContextId{DC_MENU_CONTEXT_SLOT};
}

ContextId get_config_sub_menu_context_id() {
    return ContextId{DC_MENU_CONTEXT_SLOT};
}

// ── Config tab stubs ─────────────────────────────────────────────────

void set_config_tab(ConfigTab /*tab*/) {}

// ── In-game config menu ──────────────────────────────────────────────
// A minimal, controller-driven editor for the settings that already persist
// to the VMU. Built from the zelda64 / recomp config getters and setters so it
// stays in sync with whatever config.cpp serialises. Dismissing the menu saves
// the config so changes survive a power cycle.

namespace {

// Clamp an integer setting to [0, 100] adjusted in fixed steps.
int adjust_percent(int value, int delta, int step) {
    value += delta * step;
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    return value;
}

// Cycle an enum stored as an int through [0, count) with wraparound.
int cycle_enum(int value, int delta, int count) {
    value = (value + delta) % count;
    if (value < 0) value += count;
    return value;
}

const char* on_off(bool v) { return v ? "On" : "Off"; }

// Build one option entry from a getter/setter pair.
template <typename Get, typename Set>
MenuEntry make_enum_option(std::string label, Get get, Set set,
                           int count, const char* const* names) {
    MenuEntry e;
    e.label = std::move(label);
    e.enabled = true;
    e.value_fn = [get, names]() { return std::string(names[get()]); };
    e.adjust_fn = [get, set, count](int delta) {
        set(cycle_enum(get(), delta, count));
    };
    return e;
}

} // anonymous namespace

// Forward declarations
void open_main_menu(bool is_boot = false);
void open_config_menu(bool from_main_menu = false);

void open_controller_mapping_menu(bool from_main_menu) {
    using namespace recomp;
    current_menu.title = "Controller Mapping";
    current_menu.entries.clear();
    current_menu.selected_index = 0;

    auto add_mapping_entry = [&](GameInput gi, const char* label) {
        MenuEntry m;
        m.label = label;
        m.enabled = true;
        m.value_fn = [gi]() {
            if (scanning_input == gi) return std::string("Press Button...");
            return get_input_binding(gi, 0, InputDevice::Controller).to_string();
        };
        m.action = [gi]() {
            scanning_input = gi;
        };
        current_menu.entries.push_back(std::move(m));
    };

    add_mapping_entry(GameInput::A, "A Button");
    add_mapping_entry(GameInput::B, "B Button");
    add_mapping_entry(GameInput::C_UP, "C-Up");
    add_mapping_entry(GameInput::C_DOWN, "C-Down");
    add_mapping_entry(GameInput::C_LEFT, "C-Left");
    add_mapping_entry(GameInput::C_RIGHT, "C-Right");
    add_mapping_entry(GameInput::Z, "Z Target");
    add_mapping_entry(GameInput::R, "R Shield");
    add_mapping_entry(GameInput::START, "Start");
    add_mapping_entry(GameInput::DPAD_UP, "D-Pad Up");
    add_mapping_entry(GameInput::DPAD_DOWN, "D-Pad Down");
    add_mapping_entry(GameInput::DPAD_LEFT, "D-Pad Left");
    add_mapping_entry(GameInput::DPAD_RIGHT, "D-Pad Right");

    MenuEntry reset_m;
    reset_m.label = "Reset to Defaults";
    reset_m.enabled = true;
    reset_m.action = []() {
        zelda64::reset_cont_input_bindings();
    };
    current_menu.entries.push_back(std::move(reset_m));

    MenuEntry back_m;
    back_m.label = "Back";
    back_m.enabled = true;
    back_m.action = [from_main_menu]() {
        scanning_input = GameInput::COUNT;
        zelda64::save_config();
        if (from_main_menu) {
            open_main_menu(false);
        } else {
            open_config_menu(false);
        }
    };
    current_menu.entries.push_back(std::move(back_m));
    
    current_menu.cancel_action = back_m.action;
    current_menu.visible = true;
}

void open_config_menu(bool from_main_menu) {
    using namespace zelda64;

    current_menu.title = "Options";
    current_menu.entries.clear();
    current_menu.selected_index = 0;

    auto& e = current_menu.entries;

    // Volume (0–100 in steps of 10).
    {
        MenuEntry m;
        m.label = "Main Volume";
        m.enabled = true;
        m.value_fn = []() { return std::to_string(get_main_volume()); };
        m.adjust_fn = [](int d) { set_main_volume(adjust_percent(get_main_volume(), d, 10)); };
        e.push_back(std::move(m));
    }
    {
        MenuEntry m;
        m.label = "Music Volume";
        m.enabled = true;
        m.value_fn = []() { return std::to_string(get_bgm_volume()); };
        m.adjust_fn = [](int d) { set_bgm_volume(adjust_percent(get_bgm_volume(), d, 10)); };
        e.push_back(std::move(m));
    }
    {
        MenuEntry m;
        m.label = "Low-Health Beeps";
        m.enabled = true;
        m.value_fn = []() { return std::string(on_off(get_low_health_beeps_enabled())); };
        m.adjust_fn = [](int) { set_low_health_beeps_enabled(!get_low_health_beeps_enabled()); };
        e.push_back(std::move(m));
    }

    // Gameplay options backed by enums.
    static const char* targeting_names[] = {"Switch", "Hold"};
    e.push_back(make_enum_option(
        "Targeting",
        []() { return static_cast<int>(get_targeting_mode()); },
        [](int v) { set_targeting_mode(static_cast<TargetingMode>(v)); },
        static_cast<int>(TargetingMode::OptionCount), targeting_names));

    static const char* autosave_names[] = {"On", "Off"};
    e.push_back(make_enum_option(
        "Autosave",
        []() { return static_cast<int>(get_autosave_mode()); },
        [](int v) { set_autosave_mode(static_cast<AutosaveMode>(v)); },
        static_cast<int>(AutosaveMode::OptionCount), autosave_names));

    static const char* invert_names[] = {"None", "X", "Y", "Both"};
    e.push_back(make_enum_option(
        "Camera Invert",
        []() { return static_cast<int>(get_camera_invert_mode()); },
        [](int v) { set_camera_invert_mode(static_cast<CameraInvertMode>(v)); },
        static_cast<int>(CameraInvertMode::OptionCount), invert_names));

    // Controller options (recomp namespace getters/setters).
    {
        MenuEntry m;
        m.label = "Rumble Strength";
        m.enabled = true;
        m.value_fn = []() { return std::to_string(recomp::get_rumble_strength()); };
        m.adjust_fn = [](int d) {
            recomp::set_rumble_strength(adjust_percent(recomp::get_rumble_strength(), d, 10));
        };
        e.push_back(std::move(m));
    }
    {
        MenuEntry m;
        m.label = "Stick Deadzone";
        m.enabled = true;
        m.value_fn = []() { return std::to_string(recomp::get_joystick_deadzone()); };
        m.adjust_fn = [](int d) {
            recomp::set_joystick_deadzone(adjust_percent(recomp::get_joystick_deadzone(), d, 5));
        };
        e.push_back(std::move(m));
    }

    // Removing Controller Mapping to move it to the Main Menu

    // Closing the menu (Back or B) persists everything to the VMU.
    auto save_and_close = [from_main_menu]() {
        zelda64::save_config();
        if (from_main_menu) {
            open_main_menu(false);
        } else {
            current_menu.visible = false;
        }
    };
    // Quit Game is moved to the Main Menu
    {
        MenuEntry m;
        m.label = "Back (save)";
        m.enabled = true;
        m.action = save_and_close;
        e.push_back(std::move(m));
    }

    current_menu.cancel_action = save_and_close;
    current_menu.visible = true;
}

void open_main_menu(bool is_boot) {
    using namespace zelda64;

    current_menu.title = is_boot ? "Zelda64 Recompiled" : "Paused";
    current_menu.entries.clear();
    current_menu.selected_index = 0;

    auto& e = current_menu.entries;

    {
        MenuEntry m;
        m.label = is_boot ? "Start Game" : "Resume Game";
        m.enabled = true;
        m.action = []() {
            current_menu.visible = false;
        };
        e.push_back(std::move(m));
    }
    
    {
        MenuEntry m;
        m.label = "Options";
        m.enabled = true;
        m.action = [is_boot]() {
            open_config_menu(true);
        };
        e.push_back(std::move(m));
    }
    
    {
        MenuEntry m;
        m.label = "Controller Mapping";
        m.enabled = true;
        m.action = [is_boot]() {
            open_controller_mapping_menu(true);
        };
        e.push_back(std::move(m));
    }

    // Only show Quit if we're not in the boot launcher
    if (!is_boot) {
        MenuEntry m;
        m.label = "Quit Game";
        m.enabled = true;
        m.action = []() {
            zelda64::save_config();
            zelda64::open_quit_game_prompt();
        };
        e.push_back(std::move(m));
    }

    auto cancel_action = [is_boot]() {
        if (!is_boot) {
            current_menu.visible = false;
        }
    };
    
    current_menu.cancel_action = cancel_action;
    current_menu.visible = true;
}

int config_tab_to_index(ConfigTab tab) {
    return static_cast<int>(tab);
}

void set_config_tabset_mod_nav() {}
void focus_mod_configure_button() {}

// ── Display / cursor ─────────────────────────────────────────────────

void get_window_size(int& width, int& height) {
    width = DC_SCREEN_WIDTH;
    height = DC_SCREEN_HEIGHT;
}

void set_cursor_visible(bool /*visible*/) {
    // No cursor on Dreamcast
}

void toggle_fullscreen() {
    // Always fullscreen on Dreamcast
}

// ── Controller / mouse active flags ─────────────────────────────────

bool get_cont_active() {
    return true; // Controller is always the active input device
}

void set_cont_active(bool /*active*/) {}

void activate_mouse() {
    // No mouse on Dreamcast
}

// ── Error display ────────────────────────────────────────────────────

void show_error_screen(const char* title, const char* message) {
    fprintf(stderr, "[DC ERROR] %s: %s\n", title, message);

    const std::vector<std::string> lines = wrap_text(message, ERROR_WRAP_COLUMNS);

    // Dark blue background so the screen is clearly distinct from a hang.
    constexpr uint16_t ERROR_BG_565 = 0x000A;

    uint32_t prev_buttons = poll_raw_buttons();
    // Give up after ~60 seconds so a console without a controller attached
    // does not block forever (the error remains on the serial log).
    constexpr int TIMEOUT_ITERATIONS = 60 * 60;
    for (int i = 0; i < TIMEOUT_ITERATIONS; i++) {
        // Redraw every iteration: if a render thread is still presenting PVR
        // frames it will overwrite the framebuffer, so keep restoring it.
        fill_framebuffer(ERROR_BG_565);

        int y = 80;
        draw_bios_text(MENU_PADDING * 2, y, COLOR_YELLOW, title);
        y += FONT_CHAR_H * 2;
        for (const std::string& line : lines) {
            draw_bios_text(MENU_PADDING * 2, y, COLOR_WHITE, line.c_str());
            y += FONT_CHAR_H + 2;
        }
        draw_bios_text(MENU_PADDING * 2, DC_SCREEN_HEIGHT - FONT_CHAR_H * 2,
                       COLOR_GRAY, "Press A to continue");

        const uint32_t buttons = poll_raw_buttons();
        if ((buttons & ~prev_buttons) & (CONT_A | CONT_START)) {
            break;
        }
        prev_buttons = buttons;
        thd_sleep(16);
    }
}

void dc_suppress_next_stored_rom_error() {
    suppress_stored_rom_error.store(true);
}

void message_box(const char* msg) {
    // Legacy safety net: if librecomp still complains about a missing stored ROM
    // on Dreamcast (boot uses streamed PI reads via set_rom_stream), downgrade
    // that one expected message instead of blocking with a bogus error screen.
    if (suppress_stored_rom_error.exchange(false) && strstr(msg, "stored ROM") != nullptr) {
        fprintf(stdout, "[DC UI] suppressed expected message: %s\n", msg);
        return;
    }
    show_error_screen("Error", msg);
}

// ── Rendering hooks ──────────────────────────────────────────────────

void set_render_hooks() {}
void update_supported_options() {}
void apply_color_hack() {}

// ── Styling / prompt init ────────────────────────────────────────────

void init_styling(const std::filesystem::path& /*rcss_file*/) {}
void init_prompt_context() {}

// ── Choice / info / notification prompts ────────────────────────────

void open_choice_prompt(
    const std::string& header_text,
    const std::string& /*content_text*/,
    const std::string& confirm_label_text,
    const std::string& cancel_label_text,
    std::function<void()> confirm_action,
    std::function<void()> cancel_action,
    ButtonVariant /*confirm_variant*/,
    ButtonVariant /*cancel_variant*/,
    bool /*focus_on_cancel*/,
    const std::string& /*return_element_id*/) {

    current_menu.title = header_text;
    current_menu.entries.clear();
    current_menu.entries.push_back({confirm_label_text, confirm_action, true});
    current_menu.entries.push_back({cancel_label_text, cancel_action, true});
    current_menu.selected_index = 0;
    current_menu.cancel_action = cancel_action;
    current_menu.visible = true;
}

void open_info_prompt(
    const std::string& header_text,
    const std::string& /*content_text*/,
    const std::string& okay_label_text,
    std::function<void()> okay_action,
    ButtonVariant /*okay_variant*/,
    const std::string& /*return_element_id*/) {

    current_menu.title = header_text;
    current_menu.entries.clear();
    current_menu.entries.push_back({okay_label_text, okay_action, true});
    current_menu.selected_index = 0;
    current_menu.cancel_action = nullptr; // B just dismisses an info prompt
    current_menu.visible = true;
}

void open_notification(
    const std::string& header_text,
    const std::string& content_text,
    const std::string& /*return_element_id*/) {

    fprintf(stdout, "[DC UI] %s: %s\n", header_text.c_str(), content_text.c_str());

    std::lock_guard<std::mutex> lock(toast_mutex);
    toast_text = header_text.empty() ? content_text : (header_text + ": " + content_text);
    // bfont has no clipping; keep the line inside the framebuffer.
    if (toast_text.size() > ERROR_WRAP_COLUMNS) {
        toast_text.resize(ERROR_WRAP_COLUMNS - 3);
        toast_text += "...";
    }
    toast_frames_remaining = TOAST_DURATION_FRAMES;
}

void close_prompt() {
    current_menu.visible = false;
}

bool is_prompt_open() {
    return current_menu.visible;
}

// ── Mod list / game start ────────────────────────────────────────────

void update_mod_list(bool /*scan_mods*/) {
    // Mods not supported on Dreamcast
}

void process_game_started() {
    current_menu.visible = false;
}

// ── Image management (no-ops on Dreamcast) ───────────────────────────

void queue_image_from_bytes_rgba32(const std::string& /*src*/, const std::vector<char>& /*bytes*/, uint32_t /*width*/, uint32_t /*height*/) {}
void queue_image_from_bytes_file(const std::string& /*src*/, const std::vector<char>& /*bytes*/) {}
void release_image(const std::string& /*src*/) {}

void drop_files(const std::list<std::filesystem::path>& /*file_list*/) {}

// ── Menu rendering ───────────────────────────────────────────────────

int menu_panel_height() {
    if (!current_menu.visible) {
        return 0;
    }
    int height = MENU_PADDING * 2 + FONT_CHAR_H + MENU_ITEM_SPACING * 2;
    height += static_cast<int>(current_menu.entries.size()) * (FONT_CHAR_H + MENU_ITEM_SPACING);
    return height;
}

void render_menu_pvr_background() {
    if (!current_menu.visible) {
        return;
    }

    const int panel_h = menu_panel_height();
    if (panel_h <= 0) {
        return;
    }

    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.gen.shading = PVR_SHADE_FLAT;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    cxt.blend.src = PVR_BLEND_SRCALPHA;
    cxt.blend.dst = PVR_BLEND_INVSRCALPHA;

    pvr_poly_hdr_t hdr;
    pvr_poly_compile(&hdr, &cxt);

    pvr_list_begin(PVR_LIST_TR_POLY);
    pvr_prim(&hdr, sizeof(pvr_poly_hdr_t));

    const float x0 = static_cast<float>(MENU_PADDING);
    const float y0 = static_cast<float>(MENU_PADDING);
    const float x1 = static_cast<float>(DC_SCREEN_WIDTH - MENU_PADDING);
    const float y1 = static_cast<float>(MENU_PADDING + panel_h);

    pvr_vertex_t vert{};
    vert.z = 0.1f;
    vert.argb = MENU_PANEL_ARGB;
    vert.oargb = 0;

    vert.flags = PVR_CMD_VERTEX;
    vert.x = x0;
    vert.y = y0;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    vert.x = x1;
    vert.y = y0;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    vert.flags = PVR_CMD_VERTEX;
    vert.x = x0;
    vert.y = y1;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = x1;
    vert.y = y1;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    pvr_list_finish();
}

void render_menu_overlay() {
    // Notification toast (independent of the menu).
    {
        std::lock_guard<std::mutex> lock(toast_mutex);
        if (toast_frames_remaining > 0) {
            draw_bios_text(MENU_PADDING, DC_SCREEN_HEIGHT - FONT_CHAR_H - MENU_PADDING / 2,
                           COLOR_YELLOW, toast_text.c_str());
            toast_frames_remaining--;
        }
    }

    if (!current_menu.visible) return;

    // BIOS-font labels are drawn directly to the framebuffer after the PVR
    // frame is presented. The translucent panel is submitted via PVR first.

    int y = MENU_PADDING;

    draw_bios_text(MENU_PADDING, y, COLOR_WHITE, current_menu.title.c_str());
    y += FONT_CHAR_H + MENU_ITEM_SPACING * 2;

    for (size_t i = 0; i < current_menu.entries.size(); i++) {
        uint32_t color = COLOR_WHITE;
        if (!current_menu.entries[i].enabled) {
            color = COLOR_GRAY;
        } else if (static_cast<int>(i) == current_menu.selected_index) {
            color = COLOR_YELLOW;
        }

        char prefix = (static_cast<int>(i) == current_menu.selected_index) ? '>' : ' ';
        char line[128];
        const MenuEntry& entry = current_menu.entries[i];
        if (entry.value_fn) {
            // Option entry: show the adjustable value framed with arrows so the
            // player knows left/right changes it.
            snprintf(line, sizeof(line), "%c %s: < %s >", prefix,
                     entry.label.c_str(), entry.value_fn().c_str());
        } else {
            snprintf(line, sizeof(line), "%c %s", prefix, entry.label.c_str());
        }
        draw_bios_text(MENU_PADDING, y, color, line);
        y += FONT_CHAR_H + MENU_ITEM_SPACING;
    }
}

// ── Menu input handling ───────────────────────────────────────────────
// Called from the input polling path with raw Dreamcast button bitmask.

void handle_menu_input(uint32_t buttons_pressed, float trigger_l, float trigger_r) {
    if (!current_menu.visible || current_menu.entries.empty()) return;

    if (scanning_input != recomp::GameInput::COUNT) {
        recomp::InputField field{};
        
        if (buttons_pressed & CONT_A) field = {DC_INPUT_TYPE_BUTTON, CONT_A};
        else if (buttons_pressed & CONT_B) field = {DC_INPUT_TYPE_BUTTON, CONT_B};
        else if (buttons_pressed & CONT_X) field = {DC_INPUT_TYPE_BUTTON, CONT_X};
        else if (buttons_pressed & CONT_Y) field = {DC_INPUT_TYPE_BUTTON, CONT_Y};
        else if (buttons_pressed & CONT_START) field = {DC_INPUT_TYPE_BUTTON, CONT_START};
        else if (buttons_pressed & CONT_DPAD_UP) field = {DC_INPUT_TYPE_BUTTON, CONT_DPAD_UP};
        else if (buttons_pressed & CONT_DPAD_DOWN) field = {DC_INPUT_TYPE_BUTTON, CONT_DPAD_DOWN};
        else if (buttons_pressed & CONT_DPAD_LEFT) field = {DC_INPUT_TYPE_BUTTON, CONT_DPAD_LEFT};
        else if (buttons_pressed & CONT_DPAD_RIGHT) field = {DC_INPUT_TYPE_BUTTON, CONT_DPAD_RIGHT};
        else if (trigger_l > 0.5f) field = {DC_INPUT_TYPE_TRIGGER, DC_L_TRIGGER};
        else if (trigger_r > 0.5f) field = {DC_INPUT_TYPE_TRIGGER, DC_R_TRIGGER};

        if (field.input_type != 0) {
            recomp::set_input_binding(scanning_input, 0, recomp::InputDevice::Controller, field);
            scanning_input = recomp::GameInput::COUNT;
        }
        return;
    }

    if (buttons_pressed & CONT_DPAD_UP) {
        current_menu.selected_index--;
        if (current_menu.selected_index < 0) {
            current_menu.selected_index = static_cast<int>(current_menu.entries.size()) - 1;
        }
    }
    if (buttons_pressed & CONT_DPAD_DOWN) {
        current_menu.selected_index++;
        if (current_menu.selected_index >= static_cast<int>(current_menu.entries.size())) {
            current_menu.selected_index = 0;
        }
    }

    // Left / right adjust the value of an option entry (volume, toggles, …).
    if (buttons_pressed & (CONT_DPAD_LEFT | CONT_DPAD_RIGHT)) {
        const int idx = current_menu.selected_index;
        if (idx >= 0 && idx < static_cast<int>(current_menu.entries.size())) {
            const MenuEntry& entry = current_menu.entries[idx];
            if (entry.enabled && entry.adjust_fn) {
                entry.adjust_fn((buttons_pressed & CONT_DPAD_RIGHT) ? +1 : -1);
            }
        }
    }

    // A / Start → confirm selection. Snapshot the callbacks first: invoking
    // them may mutate (or replace) current_menu, invalidating the reference.
    if (buttons_pressed & (CONT_A | CONT_START)) {
        const int idx = current_menu.selected_index;
        if (idx >= 0 && idx < static_cast<int>(current_menu.entries.size())) {
            const MenuEntry& entry = current_menu.entries[idx];
            if (entry.enabled) {
                if (entry.action) {
                    auto action = entry.action;
                    action();
                } else if (entry.adjust_fn) {
                    // No discrete action: A cycles the option forward, matching
                    // the right-arrow behavior for one-handed adjustment.
                    entry.adjust_fn(+1);
                }
            }
        }
        return;
    }

    // B → cancel: hide first, then fire the cancel callback (which may open a
    // new menu of its own).
    if (buttons_pressed & CONT_B) {
        auto cancel = current_menu.cancel_action;
        current_menu.visible = false;
        if (cancel) {
            cancel();
        }
    }
}

} // namespace recompui

#endif // DREAMCAST
