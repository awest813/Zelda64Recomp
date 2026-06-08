// Dreamcast Maple Bus input backend
//
// Replaces SDL2 game controller input with direct KallistiOS maple bus
// device access. The Dreamcast controller has:
//   - Digital: A, B, X, Y, Start, D-pad (up/down/left/right)
//   - Analog:  One joystick (X/Y), two analog triggers (L/R)
//   - Accessories: Puru-puru vibration pack, VMU
//
// N64 controller mapping:
//   DC A     → N64 A
//   DC B     → N64 B
//   DC X     → N64 C-Down (item 2)
//   DC Y     → N64 C-Left (item 1)
//   DC Start → N64 Start
//   DC L     → N64 Z (target)
//   DC R     → N64 R (shield)
//   DC D-pad → N64 D-pad
//   DC Stick → N64 Analog stick

#ifdef DREAMCAST

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <atomic>
#include <span>

#include <kos.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/maple/purupuru.h>

#include "recomp_input.h"
#include "recomp_ui.h"
#include "ultramodern/input.hpp"
#include "dreamcast_platform.h"

namespace {

// ── Controller state ────────────────────────────────────────────────

struct DCControllerState {
    // Raw maple state
    uint32_t buttons;       // Bitmask from cont_state_t
    float joy_x;            // -1.0 to 1.0
    float joy_y;            // -1.0 to 1.0
    float trigger_l;        // 0.0 to 1.0
    float trigger_r;        // 0.0 to 1.0

    // Mapped N64 outputs
    uint16_t n64_buttons;
    float n64_x;
    float n64_y;

    bool connected;
    bool rumble_active;
};

static DCControllerState dc_controller{};
static std::atomic<bool> rumble_requested{false};

// Dreamcast button bitmasks (active LOW in hardware, KOS inverts for us)
// KOS cont_state_t uses CONT_* defines
constexpr uint32_t DC_BTN_A     = CONT_A;
constexpr uint32_t DC_BTN_B     = CONT_B;
constexpr uint32_t DC_BTN_X     = CONT_X;
constexpr uint32_t DC_BTN_Y     = CONT_Y;
constexpr uint32_t DC_BTN_START = CONT_START;
constexpr uint32_t DC_BTN_DPAD_UP    = CONT_DPAD_UP;
constexpr uint32_t DC_BTN_DPAD_DOWN  = CONT_DPAD_DOWN;
constexpr uint32_t DC_BTN_DPAD_LEFT  = CONT_DPAD_LEFT;
constexpr uint32_t DC_BTN_DPAD_RIGHT = CONT_DPAD_RIGHT;

// N64 button bitmasks (matching recomp_input.h)
constexpr uint16_t N64_BTN_A       = 0x8000;
constexpr uint16_t N64_BTN_B       = 0x4000;
constexpr uint16_t N64_BTN_Z       = 0x2000;
constexpr uint16_t N64_BTN_START   = 0x1000;
constexpr uint16_t N64_BTN_DU      = 0x0800;
constexpr uint16_t N64_BTN_DD      = 0x0400;
constexpr uint16_t N64_BTN_DL      = 0x0200;
constexpr uint16_t N64_BTN_DR      = 0x0100;
constexpr uint16_t N64_BTN_L       = 0x0020;
constexpr uint16_t N64_BTN_R       = 0x0010;
constexpr uint16_t N64_BTN_CU      = 0x0008;
constexpr uint16_t N64_BTN_CD      = 0x0004;
constexpr uint16_t N64_BTN_CL      = 0x0002;
constexpr uint16_t N64_BTN_CR      = 0x0001;

// Convert DC joystick raw value (-128..127) to float (-1.0..1.0)
// deadzone is a fraction in [0.0, 1.0]; values within the deadzone return 0.
float normalize_stick(int raw, float deadzone) {
    float val = static_cast<float>(raw) / 128.0f;
    if (val > 1.0f) val = 1.0f;
    if (val < -1.0f) val = -1.0f;

    // Apply deadzone
    if (std::fabs(val) < deadzone) {
        return 0.0f;
    }
    // Rescale outside deadzone so the usable range spans 0..1
    float sign = (val > 0.0f) ? 1.0f : -1.0f;
    return sign * (std::fabs(val) - deadzone) / (1.0f - deadzone);
}

// Map DC buttons to N64 buttons
uint16_t map_buttons(uint32_t dc_buttons, float trigger_l, float trigger_r) {
    uint16_t n64 = 0;

    // Face buttons
    if (dc_buttons & DC_BTN_A)     n64 |= N64_BTN_A;
    if (dc_buttons & DC_BTN_B)     n64 |= N64_BTN_B;
    if (dc_buttons & DC_BTN_START) n64 |= N64_BTN_START;

    // X → C-Down, Y → C-Left (most common item buttons in MM)
    if (dc_buttons & DC_BTN_X)     n64 |= N64_BTN_CD;
    if (dc_buttons & DC_BTN_Y)     n64 |= N64_BTN_CL;

    // D-pad
    if (dc_buttons & DC_BTN_DPAD_UP)    n64 |= N64_BTN_DU;
    if (dc_buttons & DC_BTN_DPAD_DOWN)  n64 |= N64_BTN_DD;
    if (dc_buttons & DC_BTN_DPAD_LEFT)  n64 |= N64_BTN_DL;
    if (dc_buttons & DC_BTN_DPAD_RIGHT) n64 |= N64_BTN_DR;

    // Analog triggers: L → N64 Z (targeting), R → N64 R (shield)
    if (trigger_l > 0.5f) n64 |= N64_BTN_Z;
    if (trigger_r > 0.5f) n64 |= N64_BTN_R;

    return n64;
}

} // anonymous namespace

namespace recomp {
// Defined here, ahead of dreamcast::maple_poll() which reads them. The getters
// and setters that expose these to the config system live further down.
static int dc_rumble_strength  = 100; // 0-100
static int dc_joystick_deadzone = 15; // percent
} // namespace recomp

namespace dreamcast {

void maple_poll() {
    maple_device_t* cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (cont == nullptr) {
        dc_controller.connected = false;
        return;
    }

    cont_state_t* state = reinterpret_cast<cont_state_t*>(maple_dev_status(cont));
    if (state == nullptr) {
        dc_controller.connected = false;
        return;
    }

    dc_controller.connected = true;
    dc_controller.buttons = state->buttons;

    // Apply the user-configured deadzone (stored as an integer percentage).
    float deadzone = recomp::dc_joystick_deadzone / 100.0f;
    dc_controller.joy_x = normalize_stick(state->joyx, deadzone);
    dc_controller.joy_y = normalize_stick(-state->joyy, deadzone); // Invert Y for N64 convention
    dc_controller.trigger_l = static_cast<float>(state->ltrig) / 255.0f;
    dc_controller.trigger_r = static_cast<float>(state->rtrig) / 255.0f;

    // Map to N64
    dc_controller.n64_buttons = map_buttons(state->buttons, dc_controller.trigger_l, dc_controller.trigger_r);
    dc_controller.n64_x = dc_controller.joy_x;
    dc_controller.n64_y = dc_controller.joy_y;

    // Handle rumble
    bool should_rumble = rumble_requested.load(std::memory_order_relaxed);
    if (should_rumble != dc_controller.rumble_active) {
        maple_device_t* purupuru = maple_enum_type(0, MAPLE_FUNC_PURUPURU);
        if (purupuru != nullptr) {
            if (should_rumble) {
                // Map dc_rumble_strength (0–100) to puru-puru intensity (0x01–0x0F).
                // Puru-puru raw command layout: 0x00 DD II EE
                //   DD = duration (0x11 = continuous)
                //   II = intensity (motor power, 0x01 = minimum, 0x0F = maximum)
                //   EE = frequency/decay (0x11 = standard)
                uint8_t intensity = static_cast<uint8_t>(
                    1 + (recomp::dc_rumble_strength * 14 + 50) / 100);
                uint32_t cmd = (0x00u << 24) | (0x11u << 16) |
                               (static_cast<uint32_t>(intensity) << 8) | 0x11u;
                purupuru_rumble_raw(purupuru, cmd);
            } else {
                // Stop rumble: all zeros
                purupuru_rumble_raw(purupuru, 0x00000000);
            }
        }
        dc_controller.rumble_active = should_rumble;
    }
}

bool maple_get_buttons(uint16_t* buttons_out, float* x_out, float* y_out) {
    if (!dc_controller.connected) {
        *buttons_out = 0;
        *x_out = 0.0f;
        *y_out = 0.0f;
        return false;
    }

    *buttons_out = dc_controller.n64_buttons;
    *x_out = dc_controller.n64_x;
    *y_out = dc_controller.n64_y;
    return true;
}

float maple_get_trigger_l() {
    return dc_controller.trigger_l;
}

float maple_get_trigger_r() {
    return dc_controller.trigger_r;
}

void maple_set_rumble(bool active) {
    rumble_requested.store(active, std::memory_order_relaxed);
}

} // namespace dreamcast

// ── Integration with recomp input system ────────────────────────────
// These functions are called by the ultramodern callbacks registered in main.

namespace recomp {

void poll_inputs() {
    dreamcast::maple_poll();
}

bool get_n64_input(int controller_num, uint16_t* buttons_out, float* x_out, float* y_out) {
    if (controller_num != 0) {
        *buttons_out = 0;
        *x_out = 0.0f;
        *y_out = 0.0f;
        return false;
    }
    return dreamcast::maple_get_buttons(buttons_out, x_out, y_out);
}

void set_rumble(int controller_num, bool active) {
    if (controller_num == 0) {
        dreamcast::maple_set_rumble(active);
    }
}

void update_rumble() {
    // Rumble state is applied during maple_poll()
}

void handle_events() {
    dreamcast::maple_poll();
}

ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    ultramodern::input::connected_device_info_t info{};
    if (controller_num == 0 && dc_controller.connected) {
        info.is_connected = true;
        // Dreamcast controller has analog stick and rumble
        info.analog_stick_count = 1;
        info.has_rumble = true;
    }
    return info;
}

// ── Analog / digital input queries ──────────────────────────────────
// The Dreamcast has no keyboard or generic binding system; these functions
// return zero / false for all binding queries. Actual input is returned by
// get_n64_input() above, which reads directly from the Maple bus.

float get_input_analog(const InputField& /*field*/) {
    return 0.0f;
}

float get_input_analog(const std::span<const InputField> /*fields*/) {
    return 0.0f;
}

bool get_input_digital(const InputField& /*field*/) {
    return false;
}

bool get_input_digital(const std::span<const InputField> /*fields*/) {
    return false;
}

// Gyro, mouse, and right-stick are not present on the standard Dreamcast
// controller. Return zero deltas/values.
void get_gyro_deltas(float* x, float* y) {
    *x = 0.0f;
    *y = 0.0f;
}

void get_mouse_deltas(float* x, float* y) {
    *x = 0.0f;
    *y = 0.0f;
}

void get_right_analog(float* x, float* y) {
    // The Dreamcast controller has only one analog stick.
    *x = 0.0f;
    *y = 0.0f;
}

// ── Rumble strength ─────────────────────────────────────────────────
// Puru-puru rumble is either on or off; expose as 0–100 scale.
// (dc_rumble_strength is defined near the top of this file so maple_poll can
//  read it.)

int get_rumble_strength() {
    return dc_rumble_strength;
}

void set_rumble_strength(int strength) {
    dc_rumble_strength = strength;
}

// ── Sensitivity / deadzone stubs (no keyboard/mouse on Dreamcast) ───
static int dc_gyro_sensitivity    = 50;
static int dc_mouse_sensitivity   = 50;
// dc_joystick_deadzone is defined near the top of this file (read by maple_poll).

int  get_gyro_sensitivity()   { return dc_gyro_sensitivity; }
void set_gyro_sensitivity(int v) { dc_gyro_sensitivity = v; }
int  get_mouse_sensitivity()  { return dc_mouse_sensitivity; }
void set_mouse_sensitivity(int v) { dc_mouse_sensitivity = v; }
int  get_joystick_deadzone()  { return dc_joystick_deadzone; }
void set_joystick_deadzone(int v) { dc_joystick_deadzone = v; }

void apply_joystick_deadzone(float x_in, float y_in, float* x_out, float* y_out) {
    float deadzone = dc_joystick_deadzone / 100.0f;
    float magnitude = std::sqrt(x_in * x_in + y_in * y_in);
    if (magnitude < deadzone) {
        *x_out = 0.0f;
        *y_out = 0.0f;
    } else {
        float scale = (magnitude - deadzone) / (1.0f - deadzone) / magnitude;
        *x_out = x_in * scale;
        *y_out = y_in * scale;
    }
}

void set_right_analog_suppressed(bool /*suppressed*/) {
    // No right analog stick on Dreamcast.
}

// ── Input scanning (no-op on Dreamcast) ─────────────────────────────
static InputField dc_scanned_input{};
static int        dc_scanned_input_index = -1;

void start_scanning_input(InputDevice /*device*/) {}
void stop_scanning_input() {}

void finish_scanning_input(InputField scanned_field) {
    dc_scanned_input = scanned_field;
}

void cancel_scanning_input() {
    dc_scanned_input = {};
}

InputField get_scanned_input() {
    InputField ret = dc_scanned_input;
    dc_scanned_input = {};
    return ret;
}

int get_scanned_input_index() {
    return dc_scanned_input_index;
}

void config_menu_set_cont_or_kb(bool /*cont_interacted*/) {
    // No menu on Dreamcast.
}

// ── Input enable flags ───────────────────────────────────────────────
// Game input is disabled while a menu context is visible so that joystick
// and button events are consumed by the UI and not forwarded to the game.
bool game_input_disabled() {
    return recompui::is_any_context_shown();
}

bool all_input_disabled() {
    return false;
}

// ── Background input mode ────────────────────────────────────────────
// Dreamcast has no window focus; background input mode is always On.
static BackgroundInputMode dc_background_input_mode = BackgroundInputMode::On;

BackgroundInputMode get_background_input_mode() {
    return dc_background_input_mode;
}

void set_background_input_mode(BackgroundInputMode mode) {
    dc_background_input_mode = mode;
}

} // namespace recomp

// ── Default input mappings ───────────────────────────────────────────
// On Dreamcast, input comes directly from the Maple bus controller via
// get_n64_input() above, so the binding tables are intentionally empty.
// config.cpp uses these to initialise the binding arrays on startup.

namespace recomp {
const DefaultN64Mappings default_n64_keyboard_mappings = {};
const DefaultN64Mappings default_n64_controller_mappings = {};
} // namespace recomp

#endif // DREAMCAST
