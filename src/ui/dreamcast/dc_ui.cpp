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
#include "dreamcast_platform.h"

namespace {

// ── Bitmap font rendering via KOS BIOS font ─────────────────────────
// The Dreamcast BIOS includes a built-in 12x24 font that we can use
// for basic text rendering without loading any external assets.

struct MenuEntry {
    std::string label;
    std::function<void()> action;
    bool enabled;
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

// ── Menu rendering (called during PVR scene) ─────────────────────────

void render_menu_overlay() {
    if (!current_menu.visible) return;

    // Render using the Dreamcast BIOS bitmap font with RGB565 colors.
    // Text is drawn directly to the framebuffer; a future improvement would
    // be to composite via a PVR semi-transparent background quad.

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
        snprintf(line, sizeof(line), "%c %s", prefix, current_menu.entries[i].label.c_str());
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

    // A / Start → confirm selection. Snapshot the action first: invoking it may
    // mutate (or replace) current_menu, which would invalidate the reference.
    if (buttons_pressed & (CONT_A | CONT_START)) {
        const int idx = current_menu.selected_index;
        if (idx >= 0 && idx < static_cast<int>(current_menu.entries.size())) {
            const MenuEntry& entry = current_menu.entries[idx];
            if (entry.enabled && entry.action) {
                auto action = entry.action;
                action();
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
