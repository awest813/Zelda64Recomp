// Dreamcast platform support functions
//
// Implements zelda_support.h functions for the Dreamcast platform:
// program path, asset paths, file dialogs (stubbed), and error display.
//
// Also implements the dreamcast::platform_init/shutdown entry points
// and VMU/GD-ROM storage functions.

#ifdef DREAMCAST

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/reent.h>
#include <unistd.h>

#include <kos.h>
#include <dc/cdrom.h>
#include <dc/vmu_pkg.h>
#include <dc/maple.h>
#include <dc/maple/vmu.h>
#include <dc/vmufs.h>

#include "zelda_support.h"
#include "zelda_config.h"
#include "dreamcast_platform.h"

// ── Platform init / shutdown ────────────────────────────────────────

namespace dreamcast {

void platform_init() {
    fprintf(stdout, "[DC] Platform initializing...\n");

    // Initialize the CD-ROM filesystem for reading the game ROM
    // The GD-ROM should already be accessible via /cd/ after KOS init
    fprintf(stdout, "[DC] GD-ROM filesystem available at /cd/\n");
    fprintf(stdout, "[DC] VMU save path: %s\n", DC_SAVE_PATH_PREFIX);
    fprintf(stdout, "[DC] Platform initialized\n");
}

void platform_shutdown() {
    fprintf(stdout, "[DC] Platform shutting down\n");
}

// ── VMU storage ─────────────────────────────────────────────────────

bool vmu_save(const char* filename, const void* data, size_t size) {
    // Find first VMU
    maple_device_t* vmu = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
    if (vmu == nullptr) {
        fprintf(stderr, "[DC] No VMU found for saving\n");
        return false;
    }

    // Package the data with a VMS header
    vmu_pkg_t pkg;
    memset(&pkg, 0, sizeof(pkg));
    strncpy(pkg.desc_short, "MM Recomp", 16);
    strncpy(pkg.desc_long, "Zelda64 Recompiled Save", 32);
    strncpy(pkg.app_id, "Z64RECOMP", 16);
    pkg.icon_cnt = 0;
    pkg.icon_anim_speed = 0;
    pkg.eyecatch_type = VMUPKG_EC_NONE;
    pkg.data_len = size;
    pkg.data = const_cast<uint8_t*>(static_cast<const uint8_t*>(data));

    uint8_t* pkg_out = nullptr;
    int pkg_size = 0;
    if (vmu_pkg_build(&pkg, &pkg_out, &pkg_size) < 0) {
        fprintf(stderr, "[DC] Failed to build VMU package\n");
        return false;
    }

    // Write to VMU
    // Construct the full path: /vmu/a1/<filename>
    char path[64];
    snprintf(path, sizeof(path), "%s%s", DC_SAVE_PATH_PREFIX, filename);

    FILE* f = fopen(path, "wb");
    if (f == nullptr) {
        fprintf(stderr, "[DC] Failed to open VMU file for writing: %s\n", path);
        free(pkg_out);
        return false;
    }

    size_t written = fwrite(pkg_out, 1, pkg_size, f);
    fclose(f);
    free(pkg_out);

    if (written != static_cast<size_t>(pkg_size)) {
        fprintf(stderr, "[DC] VMU write incomplete: %zu / %d bytes\n", written, pkg_size);
        return false;
    }

    fprintf(stdout, "[DC] Saved %zu bytes to VMU: %s\n", size, path);
    return true;
}

bool vmu_load(const char* filename, void* data, size_t max_size, size_t* out_size) {
    char path[64];
    snprintf(path, sizeof(path), "%s%s", DC_SAVE_PATH_PREFIX, filename);

    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }

    // Read entire file
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(f);
        return false;
    }

    uint8_t* raw = static_cast<uint8_t*>(malloc(file_size));
    if (raw == nullptr) {
        fclose(f);
        return false;
    }

    fread(raw, 1, file_size, f);
    fclose(f);

    // Unpack VMS header
    vmu_pkg_t pkg;
    if (vmu_pkg_parse(raw, static_cast<size_t>(file_size), &pkg) < 0) {
        fprintf(stderr, "[DC] Failed to parse VMU package: %s\n", path);
        free(raw);
        return false;
    }

    size_t copy_size = (pkg.data_len < max_size) ? pkg.data_len : max_size;
    memcpy(data, pkg.data, copy_size);
    if (out_size) *out_size = copy_size;

    free(raw);
    fprintf(stdout, "[DC] Loaded %zu bytes from VMU: %s\n", copy_size, path);
    return true;
}

bool vmu_delete(const char* filename) {
    char path[64];
    snprintf(path, sizeof(path), "%s%s", DC_SAVE_PATH_PREFIX, filename);
    return (remove(path) == 0);
}

size_t vmu_free_blocks() {
    maple_device_t* vmu = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
    if (vmu == nullptr) {
        return 0;
    }

    int blocks = vmufs_free_blocks(vmu);
    return (blocks < 0) ? 0 : static_cast<size_t>(blocks);
}

// ── GD-ROM access ───────────────────────────────────────────────────

bool gdrom_file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

size_t gdrom_file_size(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    return (size > 0) ? static_cast<size_t>(size) : 0;
}

bool gdrom_read_file(const char* path, void* buffer, size_t size) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    size_t read = fread(buffer, 1, size, f);
    fclose(f);
    return (read == size);
}

} // namespace dreamcast

// ── zelda_support.h implementation ──────────────────────────────────

namespace zelda64 {

std::filesystem::path get_program_path() {
    // On Dreamcast, the program runs from the GD-ROM
    return std::filesystem::path("/cd");
}

std::filesystem::path get_asset_path(const char* asset) {
    return std::filesystem::path(DC_ASSET_BASE_PATH) / asset;
}

void open_file_dialog(std::function<void(bool success, const std::filesystem::path& path)> callback) {
    // No file dialog on Dreamcast; the ROM is expected at a fixed path.
    // Directly pass the known ROM path.
    if (dreamcast::gdrom_file_exists(DC_ROM_PATH)) {
        callback(true, std::filesystem::path(DC_ROM_PATH));
    } else {
        fprintf(stderr, "[DC] ROM not found at %s\n", DC_ROM_PATH);
        callback(false, {});
    }
}

void open_file_dialog_multiple(std::function<void(bool success, const std::list<std::filesystem::path>& paths)> callback) {
    // Not supported on Dreamcast
    callback(false, {});
}

void show_error_message_box(const char* title, const char* message) {
    fprintf(stderr, "[DC ERROR] %s: %s\n", title, message);
}

std::filesystem::path get_app_folder_path() {
    // Config/save data goes to VMU, but the path API expects a filesystem
    // path. Use the VMU mount point.
    return std::filesystem::path(DC_SAVE_PATH_PREFIX);
}

} // namespace zelda64

extern "C" {

struct recomp_context;

void recomp_run_ui_callbacks(uint8_t* rdram, struct recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
}

unsigned int sleep(unsigned int seconds) {
    thd_sleep(seconds * 1000);
    return 0;
}

int usleep(unsigned int usec) {
    thd_sleep(usec / 1000);
    return 0;
}

int mkdir(const char *pathname, mode_t mode) {
    (void)mode;
    return fs_mkdir(pathname);
}

int fchmod(int fd, mode_t mode) {
    (void)fd;
    (void)mode;
    return 0;
}

int _stat_r(struct _reent *reent, const char *path, struct stat *buf) {
    (void)reent;
    return fs_stat(path, buf, 0);
}

} // extern "C"

#endif // DREAMCAST
