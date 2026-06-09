// Dreamcast-specific game option and sound configuration
//
// Provides the zelda64:: config getter/setter functions that are normally
// defined in src/ui/ui_config.cpp (which is not compiled on Dreamcast).
//
// Config persistence (load_config/save_config) is handled by
// src/game/config.cpp which is compiled on all platforms. Sound and game
// option values are stored here as simple atomics; config.cpp serialises
// them to/from JSON files on the VMU filesystem.

#ifdef DREAMCAST

#include <atomic>
#include <cstdio>

#include "zelda_config.h"
#include "zelda_sound.h"
#include "recomp_ui.h"
#include "ultramodern/ultramodern.hpp"
#include "dreamcast_platform.h"

// ── Sound settings ───────────────────────────────────────────────────
// Stored as 0–100 integer values matching the PC convention.

static std::atomic<int> dc_main_volume{100};
static std::atomic<int> dc_bgm_volume{100};
static std::atomic<int> dc_low_health_beeps{1}; // enabled by default

namespace zelda64 {

void set_main_volume(int volume) {
    dc_main_volume.store(volume);
}

int get_main_volume() {
    return dc_main_volume.load();
}

void set_bgm_volume(int volume) {
    dc_bgm_volume.store(volume);
}

int get_bgm_volume() {
    return dc_bgm_volume.load();
}

void set_low_health_beeps_enabled(bool enabled) {
    dc_low_health_beeps.store(static_cast<int>(enabled));
}

bool get_low_health_beeps_enabled() {
    return dc_low_health_beeps.load() != 0;
}

void reset_sound_settings() {
    dc_main_volume.store(100);
    dc_bgm_volume.store(100);
    dc_low_health_beeps.store(1);
}

// ── Game options ─────────────────────────────────────────────────────
// Fixed-type atomic storage for each option. Default values reflect the
// most typical play experience for Dreamcast (fixed resolution, single
// controller, no mods).

static std::atomic<int> dc_targeting_mode{static_cast<int>(TargetingMode::Switch)};
static std::atomic<int> dc_autosave_mode{static_cast<int>(AutosaveMode::On)};
static std::atomic<int> dc_camera_invert{static_cast<int>(CameraInvertMode::InvertNone)};
static std::atomic<int> dc_analog_camera_invert{static_cast<int>(CameraInvertMode::InvertNone)};
static std::atomic<int> dc_analog_cam_mode{static_cast<int>(AnalogCamMode::On)};
static std::atomic<int> dc_debug_mode{0};

TargetingMode get_targeting_mode() {
    return static_cast<TargetingMode>(dc_targeting_mode.load());
}

void set_targeting_mode(TargetingMode mode) {
    dc_targeting_mode.store(static_cast<int>(mode));
}

AutosaveMode get_autosave_mode() {
    return static_cast<AutosaveMode>(dc_autosave_mode.load());
}

void set_autosave_mode(AutosaveMode mode) {
    dc_autosave_mode.store(static_cast<int>(mode));
}

CameraInvertMode get_camera_invert_mode() {
    return static_cast<CameraInvertMode>(dc_camera_invert.load());
}

void set_camera_invert_mode(CameraInvertMode mode) {
    dc_camera_invert.store(static_cast<int>(mode));
}

CameraInvertMode get_analog_camera_invert_mode() {
    return static_cast<CameraInvertMode>(dc_analog_camera_invert.load());
}

void set_analog_camera_invert_mode(CameraInvertMode mode) {
    dc_analog_camera_invert.store(static_cast<int>(mode));
}

AnalogCamMode get_analog_cam_mode() {
    return static_cast<AnalogCamMode>(dc_analog_cam_mode.load());
}

void set_analog_cam_mode(AnalogCamMode mode) {
    dc_analog_cam_mode.store(static_cast<int>(mode));
}

bool get_debug_mode_enabled() {
    return dc_debug_mode.load() != 0;
}

void set_debug_mode_enabled(bool enabled) {
    dc_debug_mode.store(static_cast<int>(enabled));
}

// ── Quit prompt ──────────────────────────────────────────────────────
// Confirmation via the minimal DC prompt system. Confirming tells
// ultramodern to exit its main loop; dc_main then flushes saves to the VMU
// and shuts the platform down.
void open_quit_game_prompt() {
    recompui::open_choice_prompt(
        "Quit Game?",
        "Unsaved progress will be lost.",
        "Quit",
        "Cancel",
        []() {
            recompui::close_prompt();
            fprintf(stdout, "[DC] Quit confirmed - shutting down\n");
            ultramodern::quit();
        },
        []() {
            recompui::close_prompt();
        },
        recompui::ButtonVariant::Error,
        recompui::ButtonVariant::Secondary,
        true,  // focus_on_cancel
        "");
}

} // namespace zelda64

#endif // DREAMCAST
