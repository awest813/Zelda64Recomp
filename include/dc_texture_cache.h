#ifndef __DC_TEXTURE_CACHE_H__
#define __DC_TEXTURE_CACHE_H__

#ifdef DREAMCAST

#include <array>
#include <cstdint>

#include <dc/pvr.h>

namespace dreamcast::tex {

struct LoadedTexture {
    const uint8_t* addr = nullptr;
    uint32_t size_bytes = 0;
};

struct TileState {
    uint8_t fmt = 0;
    uint8_t siz = 0;
    uint8_t cms = 0;
    uint8_t cmt = 0;
    uint16_t uls = 0;
    uint16_t ult = 0;
    uint16_t lrs = 0;
    uint16_t lrt = 0;
    uint32_t line_size_bytes = 0;
};

struct Surface {
    pvr_ptr_t vram = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t stride = 0;
    uint32_t pvr_format = 0;
    uint8_t cms = 0;
    uint8_t cmt = 0;
    bool valid = false;
};

// LRU-ish texture cache: imports N64 RDRAM tiles into PVR VRAM (ARGB1555/4444).
class Cache {
public:
    Cache();
    ~Cache();

    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;

    void set_frame(uint32_t frame);
    void flush();

    Surface upload(const LoadedTexture& tex, const TileState& tile, const uint8_t* palette);

private:
    struct Entry;

    std::array<Entry, 128> entries_{};
    size_t entry_count_ = 0;
    size_t vram_used_ = 0;
    uint32_t current_frame_ = 0;

    void evict_lru(size_t bytes_needed);
};

} // namespace dreamcast::tex

#endif // DREAMCAST

#endif // __DC_TEXTURE_CACHE_H__
