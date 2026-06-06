#ifndef __DREAMCAST_PLATFORM_H__
#define __DREAMCAST_PLATFORM_H__

// Dreamcast platform abstraction header.
// This header is included when DREAMCAST is defined and provides compile-time
// feature flags and forward declarations for Dreamcast-specific subsystems.

#ifdef DREAMCAST

// ── Feature flags ───────────────────────────────────────────────────
// These flags control which subsystems are compiled in or stubbed out.

// RT64 is not available on Dreamcast; use the PVR renderer instead.
#define RECOMP_RENDERER_PVR 1
#define RECOMP_RENDERER_RT64 0

// RmlUi is too heavy for SH-4; use the minimal DC UI system.
#define RECOMP_UI_RMLUI 0
#define RECOMP_UI_MINIMAL 1

// SDL2 is not used on Dreamcast; KOS maple/input APIs are used directly.
#define RECOMP_INPUT_SDL 0
#define RECOMP_INPUT_MAPLE 1

// Audio uses KOS AICA/snd_stream instead of SDL audio.
#define RECOMP_AUDIO_SDL 0
#define RECOMP_AUDIO_AICA 1

// The mod system requires runtime code patching which is not supported.
#define RECOMP_MODS_ENABLED 0

// NFD (native file dialog) is not available; ROM is loaded from GD-ROM.
#define RECOMP_FILE_DIALOG 0

// Dreamcast hardware constants
#define DC_MAIN_RAM_SIZE    (16 * 1024 * 1024)  // 16 MB main RAM
#define DC_VRAM_SIZE        (8 * 1024 * 1024)   // 8 MB video RAM
#define DC_SOUND_RAM_SIZE   (2 * 1024 * 1024)   // 2 MB sound RAM (AICA)

// Display defaults
#define DC_SCREEN_WIDTH     640
#define DC_SCREEN_HEIGHT    480

// VMU save constants
#define DC_VMU_BLOCK_SIZE   512
#define DC_VMU_MAX_BLOCKS   200  // Typical VMU capacity

// GD-ROM paths
#define DC_ROM_PATH         "/cd/rom.z64"
#define DC_ASSET_BASE_PATH  "/cd/assets/"
#define DC_SAVE_PATH_PREFIX "/vmu/a1/"

#include <cstdint>
#include <cstddef>

namespace dreamcast {
    // ── Initialization ──────────────────────────────────────────────
    // Called once at startup to initialize all DC hardware subsystems.
    void platform_init();
    void platform_shutdown();

    // ── Input ───────────────────────────────────────────────────────
    // Poll maple bus devices and update controller state.
    void maple_poll();
    bool maple_get_buttons(uint16_t* buttons_out, float* x_out, float* y_out);
    float maple_get_trigger_l();
    float maple_get_trigger_r();
    void maple_set_rumble(bool active);

    // ── Audio ───────────────────────────────────────────────────────
    // Initialize AICA sound hardware for PCM streaming.
    void aica_init(uint32_t sample_rate);
    void aica_shutdown();
    void aica_queue_samples(const int16_t* samples, size_t sample_count);
    size_t aica_get_frames_remaining();
    void aica_set_frequency(uint32_t freq);

    // ── Storage ─────────────────────────────────────────────────────
    // VMU (Visual Memory Unit) save/load.
    bool vmu_save(const char* filename, const void* data, size_t size);
    bool vmu_load(const char* filename, void* data, size_t max_size, size_t* out_size);
    bool vmu_delete(const char* filename);
    size_t vmu_free_blocks();

    // GD-ROM file access.
    bool gdrom_file_exists(const char* path);
    size_t gdrom_file_size(const char* path);
    bool gdrom_read_file(const char* path, void* buffer, size_t size);
}

#endif // DREAMCAST

#endif // __DREAMCAST_PLATFORM_H__
