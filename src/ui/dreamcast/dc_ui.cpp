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
#include <functional>
#include <vector>
#include <list>
#include <filesystem>

#include <kos.h>
#include <dc/pvr.h>
#include <dc/biosfont.h>

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
};

static MenuState current_menu{};

// Colors (ARGB packed)
constexpr uint32_t COLOR_WHITE      = 0xFFFFFFFF;
constexpr uint32_t COLOR_YELLOW     = 0xFFFFFF00;
constexpr uint32_t COLOR_GRAY       = 0xFF808080;
constexpr uint32_t COLOR_BG         = 0xC0000020;
constexpr uint32_t COLOR_HIGHLIGHT  = 0x80004080;

// Font metrics (BIOS font)
constexpr int FONT_CHAR_W = 12;
constexpr int FONT_CHAR_H = 24;
constexpr int MENU_PADDING = 20;
constexpr int MENU_ITEM_SPACING = 4;

void draw_bios_text(int x, int y, uint32_t color, const char* text) {
    // KOS bios_font_draw_str renders directly to the framebuffer.
    // vram_s is a KOS-provided pointer to the 16-bit VRAM framebuffer
    // (defined in <dc/video.h> as: extern uint16 *vram_s).
    //
    // TODO: Implement proper PVR-based text rendering by:
    // 1. Pre-rendering the BIOS font glyphs to a PVR texture atlas
    // 2. Drawing textured quads for each character
    (void)color;
    bios_font_draw_str(vram_s + y * DC_SCREEN_WIDTH + x, DC_SCREEN_WIDTH, 0, text);
}

} // anonymous namespace

// ── recompui namespace stubs ────────────────────────────────────────
// These functions implement the recomp_ui.h interface with minimal
// Dreamcast-appropriate behavior.

namespace recompui {

void queue_event(const void* /*event*/) {
    // No SDL events on Dreamcast
}

bool try_deque_event(void* /*out*/) {
    return false;
}

void show_context(uint32_t /*context*/, std::string_view /*param*/) {
    current_menu.visible = true;
}

void hide_context(uint32_t /*context*/) {
    current_menu.visible = false;
}

void hide_all_contexts() {
    current_menu.visible = false;
}

bool is_context_shown(uint32_t /*context*/) {
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

bool get_cont_active() {
    return true; // Controller is always the active input device
}

void set_cont_active(bool /*active*/) {
    // Always active
}

void activate_mouse() {
    // No mouse
}

void message_box(const char* msg) {
    fprintf(stderr, "[DC UI] %s\n", msg);
}

void set_render_hooks() {
    // No render hooks needed for minimal UI
}

void update_supported_options() {
    // Dreamcast has fixed capabilities
}

void apply_color_hack() {
    // Not applicable
}

void init_styling(const std::filesystem::path& /*rcss_file*/) {
    // No CSS styling on Dreamcast
}

void init_prompt_context() {
    // Minimal init
}

void open_choice_prompt(
    const std::string& header_text,
    const std::string& /*content_text*/,
    const std::string& confirm_label_text,
    const std::string& cancel_label_text,
    std::function<void()> confirm_action,
    std::function<void()> cancel_action,
    int /*confirm_variant*/,
    int /*cancel_variant*/,
    bool /*focus_on_cancel*/,
    const std::string& /*return_element_id*/) {

    current_menu.title = header_text;
    current_menu.entries.clear();
    current_menu.entries.push_back({confirm_label_text, confirm_action, true});
    current_menu.entries.push_back({cancel_label_text, cancel_action, true});
    current_menu.selected_index = 0;
    current_menu.visible = true;
}

void close_prompt() {
    current_menu.visible = false;
}

bool is_prompt_open() {
    return current_menu.visible;
}

void update_mod_list(bool /*scan_mods*/) {
    // Mods not supported on Dreamcast
}

void process_game_started() {
    current_menu.visible = false;
}

void queue_image_from_bytes_rgba32(const std::string& /*src*/, const std::vector<char>& /*bytes*/, uint32_t /*width*/, uint32_t /*height*/) {
    // Not supported
}

void queue_image_from_bytes_file(const std::string& /*src*/, const std::vector<char>& /*bytes*/) {
    // Not supported
}

void release_image(const std::string& /*src*/) {
    // Not supported
}

void drop_files(const std::list<std::filesystem::path>& /*file_list*/) {
    // Not supported
}

// ── Menu rendering (called during PVR scene) ────────────────────────

void render_menu_overlay() {
    if (!current_menu.visible) return;

    // This would be called during the PVR scene to overlay menu graphics.
    // For the initial implementation, render using BIOS font.
    //
    // TODO: Implement proper PVR polygon-based menu rendering with:
    // - Background quad with semi-transparent dark overlay
    // - Text rendered via pre-baked font texture atlas
    // - Highlight bar for selected item
    // - Smooth scrolling for long lists

    int y = MENU_PADDING;

    // Title
    draw_bios_text(MENU_PADDING, y, COLOR_WHITE, current_menu.title.c_str());
    y += FONT_CHAR_H + MENU_ITEM_SPACING * 2;

    // Menu items
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

// Handle menu input (called from the input polling path)
void handle_menu_input(uint32_t buttons_pressed) {
    if (!current_menu.visible || current_menu.entries.empty()) return;

    // D-pad up/down to navigate
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

    // A button to confirm
    if (buttons_pressed & CONT_A) {
        auto& entry = current_menu.entries[current_menu.selected_index];
        if (entry.enabled && entry.action) {
            entry.action();
        }
    }

    // B button to go back / cancel
    if (buttons_pressed & CONT_B) {
        current_menu.visible = false;
    }
}

} // namespace recompui

#endif // DREAMCAST
