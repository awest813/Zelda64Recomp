#ifndef __DC_FIXED_TEXTURE_H__
#define __DC_FIXED_TEXTURE_H__

#ifdef DREAMCAST

#include <cstdint>
#include <vector>

namespace dreamcast::tex {

// Load a hand-tuned PVR conversion from GD-ROM when present.
// Files live under DC_FIXED_TEXTURE_PATH as <hash>.dt (see tools/dreamcast/fixed_textures/).
// Returns true when pixels/pvr_format/stride were filled from disc.
bool try_load_fixed_texture(
    uint32_t content_hash,
    uint8_t fmt,
    uint8_t siz,
    uint32_t width,
    uint32_t height,
    std::vector<uint16_t>& pixels,
    uint32_t& stride,
    uint32_t& pvr_format);

} // namespace dreamcast::tex

#endif // DREAMCAST

#endif // __DC_FIXED_TEXTURE_H__
