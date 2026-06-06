#ifndef __DC_COMBINER_H__
#define __DC_COMBINER_H__

#ifdef DREAMCAST

#include <cstdint>

namespace dreamcast::combiner {

struct ColorSource {
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
    uint8_t a = 255;
};

struct Inputs {
    ColorSource shade;
    ColorSource prim;
    ColorSource env;
    ColorSource texel0;
    ColorSource texel1;
    uint8_t lod_fraction = 0;
    uint8_t prim_lod_frac = 0;
};

// Evaluate the 64-bit RDP combine word for one vertex/shade sample.
uint32_t evaluate(uint64_t combine_mode, const Inputs& in, bool cycle2 = false);

bool uses_texel0(uint64_t combine_mode);
bool uses_texel1(uint64_t combine_mode);

} // namespace dreamcast::combiner

#endif // DREAMCAST

#endif // __DC_COMBINER_H__
