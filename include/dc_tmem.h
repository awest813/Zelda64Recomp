#ifndef __DC_TMEM_H__
#define __DC_TMEM_H__

#ifdef DREAMCAST

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace dreamcast::tmem {

constexpr size_t TMEM_SIZE = 4096;

// 4 KB RDP texture memory staging buffer.
class Buffer {
public:
    void clear() {
        data_.fill(0);
    }

    const uint8_t* data() const {
        return data_.data();
    }

    uint8_t* data() {
        return data_.data();
    }

    // Stage a contiguous block of texture data from RDRAM into TMEM.
    //
    // RDRAM is stored as host-native 32-bit words (big-endian N64 words
    // byte-swapped to little-endian), so byte access is XOR-3 swizzled. We
    // de-swizzle here so TMEM holds the texture in logical (big-endian) byte
    // order, which is what the texture decoders expect.
    void load_block(const uint8_t* rdram, uint32_t src_offset, uint32_t dest_offset, uint32_t size_bytes) {
        if (rdram == nullptr || size_bytes == 0) {
            return;
        }
        const uint32_t offset = dest_offset % TMEM_SIZE;
        const uint32_t copy_size = std::min<uint32_t>(size_bytes, static_cast<uint32_t>(TMEM_SIZE) - offset);
        for (uint32_t j = 0; j < copy_size; j++) {
            data_[offset + j] = rdram[(src_offset + j) ^ 3u];
        }
    }

    void load_tile(
        const uint8_t* rdram,
        uint32_t src_offset,
        uint32_t src_stride_bytes,
        uint32_t dest_offset,
        uint32_t width_bytes,
        uint32_t height) {

        if (rdram == nullptr || width_bytes == 0 || height == 0) {
            return;
        }

        uint32_t dst = dest_offset % TMEM_SIZE;
        for (uint32_t y = 0; y < height; y++) {
            if (dst + width_bytes > TMEM_SIZE) {
                break;
            }
            const uint32_t row_src = src_offset + static_cast<uint32_t>(y) * src_stride_bytes;
            for (uint32_t x = 0; x < width_bytes; x++) {
                data_[dst + x] = rdram[(row_src + x) ^ 3u];
            }
            dst += width_bytes;
        }
    }

private:
    std::array<uint8_t, TMEM_SIZE> data_{};
};

} // namespace dreamcast::tmem

#endif // DREAMCAST

#endif // __DC_TMEM_H__
