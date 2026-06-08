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
#include <string>
#include <string_view>
#include <functional>
#include <vector>
#include <list>
#include <filesystem>

#include <kos.h>
#include <dc/pvr.h>
#include <dc/biosfont.h>

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
    bfont_draw_str(vram_s + y * DC_SCREEN_WIDTH + x, DC_SCREEN_WIDTH, 1 /*opaque*/, text);
}

// A fixed ContextId slot for the single Dreamcast menu context.
constexpr uint32_t DC_MENU_CONTEXT_SLOT = 1;
constexpr uint32_t DC_NULL_CONTEXT_SLOT = 0;

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

void open_config_menu() {
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

    // Closing the menu (Back or B) persists everything to the VMU.
    auto save_and_close = []() {
        zelda64::save_config();
        current_menu.visible = false;
    };
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

void message_box(const char* msg) {
    fprintf(stderr, "[DC UI] %s\n", msg);
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

void handle_menu_input(uint32_t buttons_pressed) {
    if (!current_menu.visible || current_menu.entries.empty()) return;

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
