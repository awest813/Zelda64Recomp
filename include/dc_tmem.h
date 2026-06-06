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

    void load_block(const uint8_t* src, uint32_t dest_offset, uint32_t size_bytes) {
        if (src == nullptr || size_bytes == 0) {
            return;
        }
        const uint32_t offset = dest_offset % TMEM_SIZE;
        const uint32_t copy_size = std::min(size_bytes, TMEM_SIZE - offset);
        std::memcpy(data_.data() + offset, src, copy_size);
    }

    void load_tile(
        const uint8_t* src,
        uint32_t src_stride_bytes,
        uint32_t dest_offset,
        uint32_t width_bytes,
        uint32_t height) {

        if (src == nullptr || width_bytes == 0 || height == 0) {
            return;
        }

        uint32_t dst = dest_offset % TMEM_SIZE;
        for (uint32_t y = 0; y < height; y++) {
            if (dst + width_bytes > TMEM_SIZE) {
                break;
            }
            std::memcpy(data_.data() + dst, src + static_cast<size_t>(y) * src_stride_bytes, width_bytes);
            dst += width_bytes;
        }
    }

private:
    std::array<uint8_t, TMEM_SIZE> data_{};
};

} // namespace dreamcast::tmem

#endif // DREAMCAST

#endif // __DC_TMEM_H__
