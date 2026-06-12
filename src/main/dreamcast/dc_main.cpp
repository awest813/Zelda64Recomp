// Dreamcast main entry point and platform initialization
//
// Replaces the SDL-based main() for the Dreamcast platform. Initializes
// KallistiOS hardware subsystems and wires up the ultramodern callbacks
// to the Dreamcast-specific implementations.

#ifdef DREAMCAST

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>
#include <vector>
#include <memory>
#include <string>
#include <filesystem>
#include <array>

#include <kos.h>
#include <dc/maple.h>
#include <dc/pvr.h>
#include <dc/cdrom.h>
#include <dc/vmu_pkg.h>

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "recomp_input.h"
#include "recomp_ui.h"
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
#include "dc_ram.h"
#include "recomp_ui.h"

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
        fprintf(stderr, "Unknown RSP task: %lu\n", static_cast<unsigned long>(task->t.type));
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

// ── Boot-time ROM loading ───────────────────────────────────────────
// There is no launcher UI on Dreamcast, so nothing would ever call
// recomp::select_rom() / recomp::start_game(). Instead, validate the ROM on
// GD-ROM, register it for streamed PI reads, and start the game before
// entering recomp::start() (whose game thread then proceeds immediately
// instead of waiting for a launcher).

namespace {

constexpr size_t ROM_HEADER_BYTES = 0x1000;

// In-place de-byteswap for .v64 (16-bit swapped) and .n64 (32-bit swapped)
// dumps so users do not have to convert their ROM to .z64 by hand.
void byteswap_rom(std::vector<uint8_t>& data, size_t stride) {
    for (size_t i = 0; i + stride <= data.size(); i += stride) {
        for (size_t j = 0; j < stride / 2; j++) {
            std::swap(data[i + j], data[i + stride - 1 - j]);
        }
    }
}

bool dc_boot_load_rom() {
    const size_t rom_size = dreamcast::gdrom_file_size(DC_ROM_PATH);
    // MM US 1.0 is 32 MB; require at least the 4 KB header region so an
    // empty or placeholder file is rejected up front.
    if (rom_size < ROM_HEADER_BYTES) {
        recompui::show_error_screen(
            "ROM Not Found",
            "Could not read rom.z64 from the disc.\n\n"
            "Burn your Majora's Mask (US) ROM to the disc root as rom.z64 "
            "(see tools/dreamcast/make_disc.sh).");
        return false;
    }

    std::array<uint8_t, ROM_HEADER_BYTES> header{};
    if (!dreamcast::gdrom_read_file_at(DC_ROM_PATH, 0, header.data(), header.size())) {
        recompui::show_error_screen("Disc Read Error",
                                    "Failed to read rom.z64 from the disc.");
        return false;
    }

    // Identify byte order from the first word (big-endian z64 is 0x80371240).
    const uint32_t first_word = (uint32_t(header[0]) << 24) | (uint32_t(header[1]) << 16) |
                                (uint32_t(header[2]) << 8) | uint32_t(header[3]);
    switch (first_word) {
        case 0x80371240u:
            break;
        case 0x37804012u:
            byteswap_rom(header, 2);
            break;
        case 0x40123780u:
            byteswap_rom(header, 4);
            break;
        default:
            recompui::show_error_screen("Invalid ROM",
                                        "rom.z64 is not an N64 ROM image.");
            return false;
    }

    // Match the registered game by internal name (offset 0x20).
    const auto& game = supported_games[0];
    if (memcmp(header.data() + 0x20, game.internal_name.c_str(), game.internal_name.size()) != 0) {
        recompui::show_error_screen(
            "Wrong ROM",
            "The ROM on the disc is not Majora's Mask (US).\n\n"
            "This build only supports the US 1.0 release.");
        return false;
    }

    // Hash the full ROM from GD-ROM without keeping it in RAM.
    if (first_word != 0x80371240u) {
        recompui::show_error_screen(
            "Unsupported ROM Format",
            "Streamed ROM access requires a big-endian .z64 image on disc.\n\n"
            "Convert your ROM to .z64 before burning the disc.");
        return false;
    }

    uint64_t rom_hash = 0;
    if (!dreamcast::gdrom_file_xxh3_64(DC_ROM_PATH, rom_size, &rom_hash) ||
        rom_hash != game.rom_hash) {
        recompui::show_error_screen(
            "ROM Hash Mismatch",
            "The ROM on the disc does not match Majora's Mask (US 1.0).\n\n"
            "Use the correct ROM image when building the disc.");
        return false;
    }

    fprintf(stdout, "[DC] Validated ROM at %s (%zu bytes, streamed PI reads)\n",
            DC_ROM_PATH, rom_size);
    recomp::set_rom_stream(std::filesystem::path(DC_ROM_PATH), rom_size);
    recomp::start_game(game.game_id);
    return true;
}

// VI callback: shared rumble bookkeeping plus the VMU save mirror.
void dc_vi_callback() {
    recomp::update_rumble();
    dreamcast::storage_poll();

    static uint64_t last_ram_log_ms = 0;
    const uint64_t now = timer_ms_gettime64();
    if (now - last_ram_log_ms >= 5000) {
        dreamcast::ram::log_status(" periodic");
        last_ram_log_ms = now;
    }
}

} // anonymous namespace

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

    // Set up the ramdisk working tree and restore saves/config from the VMU
    // before anything reads or writes the config path.
    dreamcast::storage_init();

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
        .vi_callback = dc_vi_callback,
        .gfx_init_callback = recompui::update_supported_options,
    };

    ultramodern::error_handling::callbacks_t error_handling_callbacks{
        .message_box = recompui::message_box,
    };

    ultramodern::threads::callbacks_t threads_callbacks{
        .get_game_thread_name = zelda64::get_game_thread_name,
    };

    // Validate the ROM on GD-ROM, register streamed PI reads, and mark the
    // game as started; recomp::start() below then boots straight into gameplay.
    if (!dc_boot_load_rom()) {
        dreamcast::aica_shutdown();
        dreamcast::platform_shutdown();
        return EXIT_FAILURE;
    }

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

    // Final mirror of saves/config to the VMU before powering down.
    dreamcast::storage_flush();

    dreamcast::aica_shutdown();
    dreamcast::platform_shutdown();

    return EXIT_SUCCESS;
}

#endif // DREAMCAST
