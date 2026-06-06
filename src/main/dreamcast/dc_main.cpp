// Dreamcast main entry point and platform initialization
//
// Replaces the SDL-based main() for the Dreamcast platform. Initializes
// KallistiOS hardware subsystems and wires up the ultramodern callbacks
// to the Dreamcast-specific implementations.

#ifdef DREAMCAST

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <memory>
#include <string>

#include <kos.h>
#include <dc/maple.h>
#include <dc/pvr.h>
#include <dc/cdrom.h>
#include <dc/vmu_pkg.h>

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "recomp_input.h"
#include "zelda_config.h"
#include "zelda_render.h"
#include "zelda_sound.h"
#include "zelda_game.h"
#include "zelda_support.h"
#include "ovl_patches.hpp"
#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/helpers.hpp"
#include "dreamcast_platform.h"

#include "../../patches/graphics.h"
#include "../../patches/input.h"
#include "../../patches/sound.h"
#include "../../patches/misc_funcs.h"

// KallistiOS romdisk (embedded assets, if any)
// extern uint8 romdisk[];
// KOS_INIT_ROMDISK(romdisk);

KOS_INIT_FLAGS(INIT_DEFAULT | INIT_MALLOCSTATS);

const std::string version_string = "1.2.2-dc";

extern RspUcodeFunc njpgdspMain;
extern RspUcodeFunc aspMain;

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    switch (task->t.type) {
    case M_AUDTASK:
        return aspMain;
    case M_NJPEGTASK:
        return njpgdspMain;
    default:
        fprintf(stderr, "Unknown RSP task: %u\n", task->t.type);
        return nullptr;
    }
}

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
gpr get_entrypoint_address();

// Supported game entry
std::vector<recomp::GameEntry> supported_games = {
    {
        .rom_hash = 0xEF18B4A9E2386169ULL,
        .internal_name = "ZELDA MAJORA'S MASK",
        .game_id = u8"mm.n64.us.1.0",
        .mod_game_id = "mm",
        .save_type = recomp::SaveType::Flashram,
        .is_enabled = false,
        .decompression_routine = zelda64::decompress_mm,
        .has_compressed_code = true,
        .entrypoint_address = get_entrypoint_address(),
        .entrypoint = recomp_entrypoint,
    },
};

// ── GFX callbacks ───────────────────────────────────────────────────

ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    // PVR is initialized by the render context
    fprintf(stdout, "[DC] Graphics subsystem ready\n");
    return {};
}

ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    // Dreamcast has no windowing system; the PVR renders directly to the
    // video output. Return an empty handle.
    ultramodern::renderer::WindowHandle handle{};
    fprintf(stdout, "[DC] Display output: 640x480 VGA/composite\n");
    return handle;
}

void update_gfx(void*) {
    recomp::handle_events();
}

// ── Audio callbacks ─────────────────────────────────────────────────

void queue_samples(int16_t* audio_data, size_t sample_count) {
    dreamcast::aica_queue_samples(audio_data, sample_count);
}

size_t get_frames_remaining() {
    return dreamcast::aica_get_frames_remaining();
}

void set_frequency(uint32_t freq) {
    dreamcast::aica_set_frequency(freq);
}

// ── Thread naming ───────────────────────────────────────────────────

namespace zelda64 {
    std::string get_game_thread_name(const OSThread* t) {
        std::string name = "[Game] ";
        switch (t->id) {
            case 0:  name += (t->priority == 150) ? "PIMGR" : (t->priority == 254) ? "VIMGR" : std::to_string(t->id); break;
            case 1:  name += "IDLE"; break;
            case 3:  name += "MAIN"; break;
            case 4:  name += "GRAPH"; break;
            case 5:  name += "SCHED"; break;
            case 7:  name += "PADMGR"; break;
            case 10: name += "AUDIOMGR"; break;
            case 13: name += "FLASHROM"; break;
            case 18: name += "DMAMGR"; break;
            case 19: name += "IRQMGR"; break;
            default: name += std::to_string(t->id); break;
        }
        return name;
    }
}

// ── Preload stubs (not needed on Dreamcast) ─────────────────────────

struct PreloadContext {};
bool preload_executable(PreloadContext& /*context*/) { return false; }
void release_preload(PreloadContext& /*context*/) {}

// ── Mod system stubs (disabled on Dreamcast) ────────────────────────
// Texture packs and runtime mods are not supported.

void enable_texture_pack(recomp::mods::ModContext& /*context*/, const recomp::mods::ModHandle& /*mod*/) {}
void disable_texture_pack(recomp::mods::ModContext&, const recomp::mods::ModHandle& /*mod*/) {}
void reorder_texture_pack(recomp::mods::ModContext&) {}

// ── Error handling ──────────────────────────────────────────────────
// Note: recompui::message_box() and recompui::update_supported_options()
// are implemented in dc_ui.cpp; no duplicate definitions here.

// ── Main ────────────────────────────────────────────────────────────

#define REGISTER_FUNC(name) recomp::overlays::register_base_export(#name, name)

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    fprintf(stdout, "Zelda 64: Recompiled v%s (Dreamcast)\n", version_string.c_str());

    recomp::Version project_version{};
    if (!recomp::Version::from_string(version_string, project_version)) {
        fprintf(stderr, "Invalid version string: %s\n", version_string.c_str());
        return EXIT_FAILURE;
    }

    // Initialize Dreamcast platform
    dreamcast::platform_init();

    // Initialize audio
    dreamcast::aica_init(48000);

    recomp::register_config_path(zelda64::get_app_folder_path());

    // Register supported games
    for (const auto& game : supported_games) {
        recomp::register_game(game);
    }

    // Note: embedded mods are not loaded on Dreamcast since the mod system
    // is disabled. Only the base game is supported.

    // Register game API functions
    REGISTER_FUNC(recomp_get_window_resolution);
    REGISTER_FUNC(recomp_get_target_aspect_ratio);
    REGISTER_FUNC(recomp_get_target_framerate);
    REGISTER_FUNC(recomp_get_autosave_enabled);
    REGISTER_FUNC(recomp_get_analog_cam_enabled);
    REGISTER_FUNC(recomp_get_camera_inputs);
    REGISTER_FUNC(recomp_get_targeting_mode);
    REGISTER_FUNC(recomp_get_bgm_volume);
    REGISTER_FUNC(recomp_get_low_health_beeps_enabled);
    REGISTER_FUNC(recomp_get_gyro_deltas);
    REGISTER_FUNC(recomp_get_mouse_deltas);
    REGISTER_FUNC(recomp_get_inverted_axes);
    REGISTER_FUNC(recomp_get_analog_inverted_axes);

    zelda64::register_overlays();
    zelda64::register_patches();
    zelda64::load_config();

    // Set up callbacks
    recomp::rsp::callbacks_t rsp_callbacks{
        .get_rsp_microcode = get_rsp_microcode,
    };

    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = zelda64::renderer::create_render_context,
    };

    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = create_gfx,
        .create_window = create_window,
        .update_gfx = update_gfx,
    };

    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples = queue_samples,
        .get_frames_remaining = get_frames_remaining,
        .set_frequency = set_frequency,
    };

    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = recomp::poll_inputs,
        .get_input = recomp::get_n64_input,
        .set_rumble = recomp::set_rumble,
        .get_connected_device_info = recomp::get_connected_device_info,
    };

    ultramodern::events::callbacks_t thread_callbacks{
        .vi_callback = recomp::update_rumble,
        .gfx_init_callback = recompui::update_supported_options,
    };

    ultramodern::error_handling::callbacks_t error_handling_callbacks{
        .message_box = recompui::message_box,
    };

    ultramodern::threads::callbacks_t threads_callbacks{
        .get_game_thread_name = zelda64::get_game_thread_name,
    };

    recomp::start(
        project_version,
        {},
        rsp_callbacks,
        renderer_callbacks,
        audio_callbacks,
        input_callbacks,
        gfx_callbacks,
        thread_callbacks,
        error_handling_callbacks,
        threads_callbacks
    );

    dreamcast::aica_shutdown();
    dreamcast::platform_shutdown();

    return EXIT_SUCCESS;
}

#endif // DREAMCAST
