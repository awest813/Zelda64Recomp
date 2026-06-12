// Optional hand-tuned PVR texture overrides (SM64 DC "fixed textures" workflow).

#ifdef DREAMCAST

#include "dc_fixed_texture.h"
#include "dreamcast_platform.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <kos.h>

namespace dreamcast::tex {

namespace {

constexpr uint32_t kMagic = 0x58464344u; // 'DCFX'

struct FixedTextureHeader {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pvr_format;
    uint8_t fmt;
    uint8_t siz;
    uint8_t reserved[2];
};

bool read_file(const char* path, std::vector<uint8_t>& out) {
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }

    const long file_size = ftell(file);
    if (file_size < static_cast<long>(sizeof(FixedTextureHeader))) {
        fclose(file);
        return false;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    out.resize(static_cast<size_t>(file_size));
    const size_t read_bytes = fread(out.data(), 1, out.size(), file);
    fclose(file);
    return read_bytes == out.size();
}

} // anonymous namespace

bool try_load_fixed_texture(
    uint32_t content_hash,
    uint8_t fmt,
    uint8_t siz,
    uint32_t width,
    uint32_t height,
    std::vector<uint16_t>& pixels,
    uint32_t& stride,
    uint32_t& pvr_format) {
    char path[96];
    std::snprintf(path, sizeof(path), "%s%08x.dt", DC_FIXED_TEXTURE_PATH, content_hash);

    std::vector<uint8_t> file_data;
    if (!read_file(path, file_data)) {
        return false;
    }

    if (file_data.size() < sizeof(FixedTextureHeader)) {
        return false;
    }

    FixedTextureHeader header{};
    std::memcpy(&header, file_data.data(), sizeof(header));
    if (header.magic != kMagic || header.fmt != fmt || header.siz != siz) {
        return false;
    }
    if (header.width != width || header.height != height || header.stride == 0 || header.pvr_format == 0) {
        return false;
    }

    const size_t pixel_bytes = static_cast<size_t>(header.stride) * header.height * sizeof(uint16_t);
    const size_t expected_size = sizeof(FixedTextureHeader) + pixel_bytes;
    if (file_data.size() < expected_size) {
        return false;
    }

    pixels.resize(static_cast<size_t>(header.stride) * header.height);
    std::memcpy(pixels.data(), file_data.data() + sizeof(FixedTextureHeader), pixel_bytes);
    stride = header.stride;
    pvr_format = header.pvr_format;

    fprintf(stdout, "[DC] Fixed texture override: %s (%ux%u)\n", path, width, height);
    return true;
}

} // namespace dreamcast::tex

#endif // DREAMCAST
