// N64 texture import and PVR VRAM cache for the Dreamcast GBI renderer.

#ifdef DREAMCAST

#include "dc_texture_cache.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include <dc/pvr.h>

namespace dreamcast::tex {

namespace {

constexpr size_t MAX_ENTRIES = 128;
constexpr size_t VRAM_BUDGET = 3 * 1024 * 1024;

constexpr uint8_t G_IM_FMT_RGBA = 0;
constexpr uint8_t G_IM_FMT_CI = 2;
constexpr uint8_t G_IM_FMT_IA = 3;
constexpr uint8_t G_IM_FMT_I = 4;

constexpr uint8_t G_IM_SIZ_4b = 0;
constexpr uint8_t G_IM_SIZ_8b = 1;
constexpr uint8_t G_IM_SIZ_16b = 2;
constexpr uint8_t G_IM_SIZ_32b = 3;

uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1u) / alignment * alignment;
}

uint32_t fnv1a(const uint8_t* data, size_t size) {
    uint32_t hash = 0x811c9dc5u;
    for (size_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 0x01000193u;
    }
    return hash;
}

uint8_t scale_5_8(uint8_t v) {
    return static_cast<uint8_t>((v * 0xFF) / 0x1F);
}

uint8_t scale_4_8(uint8_t v) {
    return static_cast<uint8_t>(v * 0x11);
}

uint8_t scale_3_8(uint8_t v) {
    return static_cast<uint8_t>(v * 0x24);
}

uint16_t read_be16(const uint8_t* ptr) {
    return static_cast<uint16_t>((ptr[0] << 8) | ptr[1]);
}

uint32_t texture_width(const LoadedTexture& tex, const TileState& tile) {
    if (tile.line_size_bytes == 0) {
        return 1;
    }
    switch (tile.siz) {
    case G_IM_SIZ_4b:
        return tile.line_size_bytes * 2u;
    case G_IM_SIZ_8b:
        return tile.line_size_bytes;
    case G_IM_SIZ_16b:
        return tile.line_size_bytes / 2u;
    case G_IM_SIZ_32b:
        return tile.line_size_bytes / 4u;
    default:
        return tile.line_size_bytes;
    }
}

uint32_t texture_height(const LoadedTexture& tex, const TileState& tile) {
    const uint32_t width = std::max(texture_width(tex, tile), 1u);
    return tex.size_bytes / width;
}

} // anonymous namespace

struct Cache::Entry {
    const uint8_t* addr = nullptr;
    uint8_t fmt = 0;
    uint8_t siz = 0;
    uint32_t hash = 0;
    pvr_ptr_t vram = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t stride = 0;
    uint32_t pvr_format = 0;
    size_t bytes = 0;
    uint32_t last_used = 0;
};

Cache::Cache() = default;

Cache::~Cache() {
    flush();
}

void Cache::set_frame(uint32_t frame) {
    current_frame_ = frame;
}

void Cache::flush() {
    for (size_t i = 0; i < entry_count_; i++) {
        if (entries_[i].vram != 0) {
            pvr_mem_free(entries_[i].vram);
            entries_[i].vram = 0;
        }
    }
    entry_count_ = 0;
    vram_used_ = 0;
}

void Cache::evict_lru(size_t bytes_needed) {
    while (vram_used_ + bytes_needed > VRAM_BUDGET && entry_count_ > 0) {
        size_t victim = 0;
        for (size_t i = 1; i < entry_count_; i++) {
            if (entries_[i].last_used < entries_[victim].last_used) {
                victim = i;
            }
        }
        if (entries_[victim].vram != 0) {
            pvr_mem_free(entries_[victim].vram);
            vram_used_ -= entries_[victim].bytes;
        }
        entries_[victim] = entries_[entry_count_ - 1];
        entry_count_--;
    }
}

Surface Cache::upload(const LoadedTexture& tex, const TileState& tile, const uint8_t* palette) {
    Surface surface{};
    if (tex.addr == nullptr || tex.size_bytes == 0) {
        return surface;
    }

    const uint32_t hash = fnv1a(tex.addr, tex.size_bytes);
    for (size_t i = 0; i < entry_count_; i++) {
        if (entries_[i].addr == tex.addr && entries_[i].fmt == tile.fmt && entries_[i].siz == tile.siz
            && entries_[i].hash == hash) {
            entries_[i].last_used = current_frame_;
            surface.vram = entries_[i].vram;
            surface.width = entries_[i].width;
            surface.height = entries_[i].height;
            surface.stride = entries_[i].stride;
            surface.pvr_format = entries_[i].pvr_format;
            surface.cms = tile.cms;
            surface.cmt = tile.cmt;
            surface.valid = entries_[i].vram != 0;
            return surface;
        }
    }

    uint32_t width = std::max(texture_width(tex, tile), 1u);
    uint32_t height = std::max(texture_height(tex, tile), 1u);

    // Crop to the active render tile when tile bounds are set.
    const uint32_t tile_w = std::max<uint32_t>((tile.lrs - tile.uls + 4) / 4, 1);
    const uint32_t tile_h = std::max<uint32_t>((tile.lrt - tile.ult + 4) / 4, 1);
    if (tile.lrs >= tile.uls && tile.lrt >= tile.ult) {
        width = std::min(width, tile_w);
        height = std::min(height, tile_h);
    }

    const uint32_t stride = align_up(width, 32);

    std::vector<uint16_t> pixels(static_cast<size_t>(stride) * height, 0);
    const size_t pixel_count = static_cast<size_t>(width) * height;

    switch (tile.fmt) {
    case G_IM_FMT_RGBA:
        if (tile.siz == G_IM_SIZ_16b) {
            for (size_t i = 0; i < pixel_count && (i * 2 + 1) < tex.size_bytes; i++) {
                const uint16_t col16 = read_be16(tex.addr + i * 2);
                const uint8_t a = col16 & 1;
                const uint8_t r = scale_5_8(static_cast<uint8_t>(col16 >> 11));
                const uint8_t g = scale_5_8(static_cast<uint8_t>((col16 >> 6) & 0x1F));
                const uint8_t b = scale_5_8(static_cast<uint8_t>((col16 >> 1) & 0x1F));
                pixels[i] = static_cast<uint16_t>((a ? 0x8000u : 0u) | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
            }
            surface.pvr_format = PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE;
        } else if (tile.siz == G_IM_SIZ_32b) {
            for (size_t i = 0; i < pixel_count && (i * 4 + 3) < tex.size_bytes; i++) {
                const uint8_t r = tex.addr[i * 4 + 0];
                const uint8_t g = tex.addr[i * 4 + 1];
                const uint8_t b = tex.addr[i * 4 + 2];
                const uint8_t a = tex.addr[i * 4 + 3];
                pixels[i] = static_cast<uint16_t>((a ? 0xF000u : 0u) | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4));
            }
            surface.pvr_format = PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE;
        }
        break;
    case G_IM_FMT_IA:
        if (tile.siz == G_IM_SIZ_4b) {
            for (size_t i = 0; i < pixel_count; i++) {
                const uint8_t byte = tex.addr[i / 2];
                const uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xF;
                const uint8_t intensity = scale_3_8(static_cast<uint8_t>(part >> 1));
                const uint8_t alpha = (part & 1) ? 0xF : 0;
                pixels[i] = static_cast<uint16_t>((alpha << 12) | (intensity >> 4) << 8 | (intensity >> 4) << 4 | (intensity >> 4));
            }
            surface.pvr_format = PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE;
        } else if (tile.siz == G_IM_SIZ_8b) {
            for (size_t i = 0; i < pixel_count && i < tex.size_bytes; i++) {
                const uint8_t intensity = scale_4_8(static_cast<uint8_t>(tex.addr[i] >> 4));
                const uint8_t alpha = scale_4_8(static_cast<uint8_t>(tex.addr[i] & 0xF));
                pixels[i] = static_cast<uint16_t>((alpha >> 4) << 12 | (intensity >> 4) << 8 | (intensity >> 4) << 4 | (intensity >> 4));
            }
            surface.pvr_format = PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE;
        } else if (tile.siz == G_IM_SIZ_16b) {
            for (size_t i = 0; i < pixel_count && (i * 2 + 1) < tex.size_bytes; i++) {
                const uint8_t intensity = tex.addr[i * 2];
                const uint8_t alpha = tex.addr[i * 2 + 1];
                pixels[i] = static_cast<uint16_t>((alpha >> 4) << 12 | (intensity >> 4) << 8 | (intensity >> 4) << 4 | (intensity >> 4));
            }
            surface.pvr_format = PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE;
        }
        break;
    case G_IM_FMT_I:
        if (tile.siz == G_IM_SIZ_4b) {
            for (size_t i = 0; i < pixel_count; i++) {
                const uint8_t byte = tex.addr[i / 2];
                const uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xF;
                const uint8_t intensity = scale_4_8(part);
                pixels[i] = static_cast<uint16_t>(0xF000u | (intensity >> 4) << 8 | (intensity >> 4) << 4 | (intensity >> 4));
            }
            surface.pvr_format = PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE;
        } else if (tile.siz == G_IM_SIZ_8b) {
            for (size_t i = 0; i < pixel_count && i < tex.size_bytes; i++) {
                const uint8_t intensity = tex.addr[i];
                pixels[i] = static_cast<uint16_t>(0xF000u | (intensity >> 4) << 8 | (intensity >> 4) << 4 | (intensity >> 4));
            }
            surface.pvr_format = PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE;
        }
        break;
    case G_IM_FMT_CI:
        if (palette == nullptr) {
            return surface;
        }
        if (tile.siz == G_IM_SIZ_4b) {
            for (size_t i = 0; i < pixel_count; i++) {
                const uint8_t byte = tex.addr[i / 2];
                const uint8_t idx = (byte >> (4 - (i % 2) * 4)) & 0xF;
                const uint16_t col16 = read_be16(palette + idx * 2);
                const uint8_t a = col16 & 1;
                const uint8_t r = scale_5_8(static_cast<uint8_t>(col16 >> 11));
                const uint8_t g = scale_5_8(static_cast<uint8_t>((col16 >> 6) & 0x1F));
                const uint8_t b = scale_5_8(static_cast<uint8_t>((col16 >> 1) & 0x1F));
                pixels[i] = static_cast<uint16_t>((a ? 0x8000u : 0u) | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
            }
            surface.pvr_format = PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE;
        } else if (tile.siz == G_IM_SIZ_8b) {
            for (size_t i = 0; i < pixel_count && i < tex.size_bytes; i++) {
                const uint16_t col16 = read_be16(palette + tex.addr[i] * 2);
                const uint8_t a = col16 & 1;
                const uint8_t r = scale_5_8(static_cast<uint8_t>(col16 >> 11));
                const uint8_t g = scale_5_8(static_cast<uint8_t>((col16 >> 6) & 0x1F));
                const uint8_t b = scale_5_8(static_cast<uint8_t>((col16 >> 1) & 0x1F));
                pixels[i] = static_cast<uint16_t>((a ? 0x8000u : 0u) | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
            }
            surface.pvr_format = PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE;
        }
        break;
    default:
        return surface;
    }

    if (surface.pvr_format == 0) {
        return surface;
    }

    if (stride != width) {
        for (int y = static_cast<int>(height) - 1; y >= 0; y--) {
            const size_t src = static_cast<size_t>(y) * width;
            const size_t dst = static_cast<size_t>(y) * stride;
            std::memmove(pixels.data() + dst, pixels.data() + src, width * sizeof(uint16_t));
        }
    }

    const size_t upload_bytes = static_cast<size_t>(stride) * height * sizeof(uint16_t);
    const size_t padded_bytes = align_up(static_cast<uint32_t>(upload_bytes), 32);

    evict_lru(padded_bytes);

    pvr_ptr_t vram = pvr_mem_malloc(padded_bytes);
    if (vram == 0) {
        return surface;
    }

    pvr_txr_set_stride(stride);
    pvr_txr_load(pixels.data(), vram, padded_bytes);

    if (entry_count_ < MAX_ENTRIES) {
        Entry& entry = entries_[entry_count_++];
        entry.addr = tex.addr;
        entry.fmt = tile.fmt;
        entry.siz = tile.siz;
        entry.hash = hash;
        entry.vram = vram;
        entry.width = static_cast<uint16_t>(width);
        entry.height = static_cast<uint16_t>(height);
        entry.stride = static_cast<uint16_t>(stride);
        entry.pvr_format = surface.pvr_format;
        entry.bytes = padded_bytes;
        entry.last_used = current_frame_;
        vram_used_ += padded_bytes;
    }

    surface.vram = vram;
    surface.width = static_cast<uint16_t>(width);
    surface.height = static_cast<uint16_t>(height);
    surface.stride = static_cast<uint16_t>(stride);
    surface.cms = tile.cms;
    surface.cmt = tile.cmt;
    surface.valid = true;
    return surface;
}

} // namespace dreamcast::tex

#endif // DREAMCAST
