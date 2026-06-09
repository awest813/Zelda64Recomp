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

// Working storage lives on the KOS ramdisk because the VMU filesystem is
// flat (no subdirectories) and limits filenames to 12 characters, which the
// shared librecomp save/config paths (e.g. "saves/mm.n64.us.1.0.bin",
// "graphics.json.swp") violate. The whole tree is mirrored to a single VMU
// archive file by dreamcast::storage_poll()/storage_flush().
#define DC_RAM_STORAGE_PATH "/ram/zelda64"
#define DC_VMU_MIRROR_FILE  "ZELDA64.SAV"   // <= 12 chars (vmufs limit)

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

    // ── RAM-backed storage with VMU mirroring ───────────────────────
    // Saves and config JSON are written by the shared (librecomp / config.cpp)
    // code to DC_RAM_STORAGE_PATH on the KOS ramdisk. These functions mirror
    // that tree to/from a single VMU archive file (DC_VMU_MIRROR_FILE) so the
    // data survives a power cycle.
    //
    // storage_init():  create the ramdisk tree and restore it from the VMU.
    //                  Must run before load_config() / recomp::start().
    // storage_poll():  rate-limited change detection; mirrors to the VMU on a
    //                  background thread when the tree changed. Called from
    //                  the VI callback (~60 Hz; it self-limits internally).
    // storage_flush(): synchronous mirror, called once at shutdown.
    void storage_init();
    void storage_poll();
    void storage_flush();
}

// ── Dreamcast UI overlay (implemented in src/ui/dreamcast/dc_ui.cpp) ──
// The render context calls render_menu_pvr_background() before pvr_scene_finish()
// and render_menu_overlay() afterward for BIOS-font text. The input backend
// forwards freshly-pressed Maple buttons to handle_menu_input() while visible.
namespace recompui {
    // Translucent PVR panel drawn before pvr_scene_finish().
    void render_menu_pvr_background();
    // BIOS-font text drawn to the framebuffer after the PVR frame is presented.
    void render_menu_overlay();
    void handle_menu_input(uint32_t buttons_pressed);
    // Opens the in-game config menu (volume, autosave, targeting, etc.). The
    // input backend triggers this from a controller combo during gameplay.
    // Values are written through the zelda64 config setters and persisted to
    // the VMU when the menu is dismissed.
    void open_config_menu();

    // Blocking full-screen error display drawn with the BIOS font directly to
    // the framebuffer, so it works before PVR init and after fatal errors.
    // Returns when the player presses A/Start (or after a timeout, so a
    // headless/disconnected console does not hang forever).
    void show_error_screen(const char* title, const char* message);

    // The Dreamcast boot path loads the ROM itself (recomp::set_rom_contents)
    // because librecomp's "stored ROM" lives under the config path, which is
    // not writable storage big enough for a ROM on this platform. librecomp
    // still tries to load the stored ROM and reports a spurious error; this
    // arms a one-shot filter that downgrades that message box to a log line.
    void dc_suppress_next_stored_rom_error();
}

#endif // DREAMCAST

#endif // __DREAMCAST_PLATFORM_H__
