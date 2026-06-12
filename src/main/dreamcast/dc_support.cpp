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
#include <cstdlib>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
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

#include <cstddef>

#include "zelda_support.h"
#include "zelda_config.h"
#include "recomp_ui.h"
#include "dreamcast_platform.h"
#include "dc_ram.h"

extern "C" {
#include "xxHash/xxh3.h"
}

// ── Platform init / shutdown ────────────────────────────────────────

namespace {

// Older KOS headers expose this via maple_dev_status() but do not always ship
// a dedicated memcard.h in the cross-SDK include tree.
struct memcard_state_t {
    int port;
    int unit;
    int free_blocks;
};

} // anonymous namespace

namespace dreamcast {

void platform_init() {
    fprintf(stdout, "[DC] Platform initializing...\n");

    ram::init();
    ram::log_status(" boot");

    // Initialize the CD-ROM filesystem for reading the game ROM
    // The GD-ROM should already be accessible via /cd/ after KOS init
    fprintf(stdout, "[DC] GD-ROM filesystem available at /cd/\n");
    fprintf(stdout, "[DC] Fixed texture overrides: %s\n", DC_FIXED_TEXTURE_PATH);
    fprintf(stdout, "[DC] Working storage: %s (mirrored to VMU %s%s)\n",
            DC_RAM_STORAGE_PATH, DC_SAVE_PATH_PREFIX, DC_VMU_MIRROR_FILE);
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

    // Unpack VMS header (KOS API: vmu_pkg_parse(buf, pkg)).
    vmu_pkg_t pkg;
    if (vmu_pkg_parse(raw, &pkg) < 0) {
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

bool gdrom_read_file_at(const char* path, size_t offset, void* buffer, size_t size) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    bool ok = false;
    if (fseek(f, static_cast<long>(offset), SEEK_SET) == 0) {
        ok = fread(buffer, 1, size, f) == size;
    }
    fclose(f);
    return ok;
}

bool gdrom_file_xxh3_64(const char* path, size_t size, uint64_t* out_hash) {
    if (out_hash == nullptr) {
        return false;
    }

    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }

    XXH3_state_t* state = XXH3_createState();
    if (state == nullptr) {
        fclose(f);
        return false;
    }
    if (XXH3_64bits_reset(state) != XXH_OK) {
        XXH3_freeState(state);
        fclose(f);
        return false;
    }

    std::array<uint8_t, 64 * 1024> chunk{};
    size_t remaining = size;
    while (remaining > 0) {
        const size_t to_read = (remaining < chunk.size()) ? remaining : chunk.size();
        const size_t read = fread(chunk.data(), 1, to_read, f);
        if (read != to_read) {
            XXH3_freeState(state);
            fclose(f);
            return false;
        }
        if (XXH3_64bits_update(state, chunk.data(), read) != XXH_OK) {
            XXH3_freeState(state);
            fclose(f);
            return false;
        }
        remaining -= read;
    }

    *out_hash = XXH3_64bits_digest(state);
    XXH3_freeState(state);
    fclose(f);
    return true;
}

} // namespace dreamcast

// ── RAM-backed storage with VMU mirroring ────────────────────────────
// The shared save/config code (librecomp pi.cpp, src/game/config.cpp) writes
// regular files with subdirectories, long names, and ".swp" backup suffixes.
// None of that works on the flat, 12-character vmufs, so those writers target
// the KOS ramdisk (DC_RAM_STORAGE_PATH) instead and the functions below
// mirror the whole tree into a single VMU archive file.
//
// Archive layout (little-endian, SH-4 native):
//   ArchiveHeader
//   file_count * { ArchiveFileHeader, stored_size bytes of data }
// Each file is stored with its trailing run of identical bytes stripped
// (flashram saves are mostly 0x00/0xFF padding), which keeps a fresh 128 KB
// save well within the ~100 free blocks of a typical VMU.

namespace {

constexpr uint32_t ARCHIVE_MAGIC   = 0x5A363441; // 'Z64A'
constexpr uint32_t ARCHIVE_VERSION = 1;
constexpr size_t   ARCHIVE_NAME_MAX = 48;
// VI callbacks arrive at ~60 Hz; mirror at most every ~5 seconds.
constexpr uint32_t STORAGE_POLL_INTERVAL = 300;

struct ArchiveHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t file_count;
};

struct ArchiveFileHeader {
    char name[ARCHIVE_NAME_MAX]; // NUL-terminated path relative to DC_RAM_STORAGE_PATH
    uint32_t full_size;          // size after restore (stored data + fill run)
    uint32_t stored_size;        // bytes of payload present in the archive
    uint8_t fill_byte;           // value of the stripped trailing run
    uint8_t pad[3];
};

std::mutex storage_mutex;            // guards last_mirror_ against poll/flush races
std::vector<uint8_t> last_mirror_;   // archive image as last written to (or read from) the VMU
std::atomic<bool> mirror_busy_{false};
std::atomic<bool> mirror_error_notified_{false};

void append_bytes(std::vector<uint8_t>& out, const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + size);
}

// Serialise every regular file under DC_RAM_STORAGE_PATH into an archive blob.
std::vector<uint8_t> build_archive() {
    std::vector<uint8_t> blob;
    uint32_t file_count = 0;

    ArchiveHeader header{ARCHIVE_MAGIC, ARCHIVE_VERSION, 0};
    append_bytes(blob, &header, sizeof(header));

    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(DC_RAM_STORAGE_PATH, ec);
    if (ec) {
        return blob;
    }

    const std::filesystem::path root(DC_RAM_STORAGE_PATH);
    for (const auto& entry : it) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }

        const std::string rel = entry.path().lexically_relative(root).generic_string();
        if (rel.empty() || rel.size() >= ARCHIVE_NAME_MAX) {
            fprintf(stderr, "[DC] storage: skipping '%s' (name too long)\n", rel.c_str());
            continue;
        }
        // Skip librecomp's in-flight backup files; they are transient.
        if (rel.size() > 4 && rel.compare(rel.size() - 4, 4, ".swp") == 0) {
            continue;
        }

        std::ifstream in(entry.path(), std::ios::binary);
        if (!in.good()) {
            continue;
        }
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());

        ArchiveFileHeader fh{};
        strncpy(fh.name, rel.c_str(), ARCHIVE_NAME_MAX - 1);
        fh.full_size = static_cast<uint32_t>(data.size());
        fh.fill_byte = data.empty() ? 0 : data.back();
        size_t stored = data.size();
        while (stored > 0 && data[stored - 1] == fh.fill_byte) {
            stored--;
        }
        fh.stored_size = static_cast<uint32_t>(stored);

        append_bytes(blob, &fh, sizeof(fh));
        append_bytes(blob, data.data(), stored);
        file_count++;
    }

    // Patch the final file count into the header.
    memcpy(blob.data() + offsetof(ArchiveHeader, file_count), &file_count, sizeof(file_count));
    return blob;
}

// Recreate the ramdisk tree from an archive blob read off the VMU.
bool restore_archive(const std::vector<uint8_t>& blob) {
    if (blob.size() < sizeof(ArchiveHeader)) {
        return false;
    }

    ArchiveHeader header;
    memcpy(&header, blob.data(), sizeof(header));
    if (header.magic != ARCHIVE_MAGIC || header.version != ARCHIVE_VERSION) {
        fprintf(stderr, "[DC] storage: VMU archive has bad magic/version\n");
        return false;
    }

    const std::filesystem::path root(DC_RAM_STORAGE_PATH);
    size_t offset = sizeof(ArchiveHeader);
    for (uint32_t i = 0; i < header.file_count; i++) {
        if (offset + sizeof(ArchiveFileHeader) > blob.size()) {
            fprintf(stderr, "[DC] storage: VMU archive truncated\n");
            return false;
        }
        ArchiveFileHeader fh;
        memcpy(&fh, blob.data() + offset, sizeof(fh));
        offset += sizeof(fh);
        fh.name[ARCHIVE_NAME_MAX - 1] = '\0';

        if (fh.stored_size > fh.full_size || offset + fh.stored_size > blob.size()) {
            fprintf(stderr, "[DC] storage: VMU archive entry out of bounds\n");
            return false;
        }
        // Reject path escapes; entries are written as plain relative paths.
        const std::string rel(fh.name);
        if (rel.empty() || rel.front() == '/' || rel.find("..") != std::string::npos) {
            offset += fh.stored_size;
            continue;
        }

        const std::filesystem::path dest = root / rel;
        std::error_code ec;
        std::filesystem::create_directories(dest.parent_path(), ec);

        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (out.good()) {
            out.write(reinterpret_cast<const char*>(blob.data() + offset), fh.stored_size);
            // Re-expand the stripped trailing run.
            std::vector<char> fill(4096, static_cast<char>(fh.fill_byte));
            size_t remaining = fh.full_size - fh.stored_size;
            while (remaining > 0 && out.good()) {
                const size_t chunk = (remaining < fill.size()) ? remaining : fill.size();
                out.write(fill.data(), chunk);
                remaining -= chunk;
            }
        }
        offset += fh.stored_size;
    }

    return true;
}

// Write an archive blob to the VMU. Returns false when no VMU is present or
// the write fails (e.g. out of blocks).
bool write_mirror_to_vmu(const std::vector<uint8_t>& blob) {
    const bool ok = dreamcast::vmu_save(DC_VMU_MIRROR_FILE, blob.data(), blob.size());
    if (!ok && !mirror_error_notified_.exchange(true)) {
        // Surface the failure once on screen; repeating it every poll would
        // make the game unplayable when no VMU is inserted.
        recompui::open_notification(
            "Save warning",
            "Could not write save data to the VMU. Check that a VMU with free blocks is inserted in slot A1.",
            "");
    }
    if (ok) {
        mirror_error_notified_.store(false);
    }
    return ok;
}

} // anonymous namespace

namespace dreamcast {

void storage_init() {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(DC_RAM_STORAGE_PATH) / "saves", ec);
    if (ec) {
        fprintf(stderr, "[DC] storage: failed to create %s\n", DC_RAM_STORAGE_PATH);
    }

    // Restore the previous session's tree from the VMU, if present.
    // The archive is at most a little over the 128 KB flashram save.
    constexpr size_t MAX_ARCHIVE_SIZE = 192 * 1024;
    std::vector<uint8_t> blob(MAX_ARCHIVE_SIZE);
    size_t loaded = 0;
    if (vmu_load(DC_VMU_MIRROR_FILE, blob.data(), blob.size(), &loaded) && loaded > 0) {
        blob.resize(loaded);
        if (restore_archive(blob)) {
            fprintf(stdout, "[DC] storage: restored %zu bytes from VMU\n", loaded);
            std::lock_guard<std::mutex> lock(storage_mutex);
            last_mirror_ = std::move(blob);
        }
    } else {
        fprintf(stdout, "[DC] storage: no VMU mirror found (fresh start)\n");
    }
}

void storage_poll() {
    static uint32_t counter = 0;
    if (++counter < STORAGE_POLL_INTERVAL) {
        return;
    }
    counter = 0;

    if (mirror_busy_.load(std::memory_order_acquire)) {
        return; // Previous VMU write still in flight.
    }

    std::vector<uint8_t> blob = build_archive();
    {
        std::lock_guard<std::mutex> lock(storage_mutex);
        if (blob == last_mirror_) {
            return; // Nothing changed since the last mirror.
        }
        last_mirror_ = blob;
    }

    // VMU writes take on the order of seconds for ~100 KB; do them off the
    // VI callback thread so the game does not hitch.
    mirror_busy_.store(true, std::memory_order_release);
    std::thread([moved_blob = std::move(blob)]() {
        write_mirror_to_vmu(moved_blob);
        mirror_busy_.store(false, std::memory_order_release);
    }).detach();
}

void storage_flush() {
    // Wait for any in-flight background write to finish.
    while (mirror_busy_.load(std::memory_order_acquire)) {
        thd_sleep(50);
    }

    std::vector<uint8_t> blob = build_archive();
    {
        std::lock_guard<std::mutex> lock(storage_mutex);
        if (blob == last_mirror_) {
            return;
        }
        last_mirror_ = blob;
    }
    write_mirror_to_vmu(blob);
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
    recompui::show_error_screen(title, message);
}

std::filesystem::path get_app_folder_path() {
    // Config and save files are written here by the shared code, then
    // mirrored to the VMU by dreamcast::storage_poll()/storage_flush().
    // (The vmufs is flat with 12-character names, so the shared writers
    // cannot target it directly.)
    return std::filesystem::path(DC_RAM_STORAGE_PATH);
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
