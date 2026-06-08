#ifndef __DC_RDP_BLEND_H__
#define __DC_RDP_BLEND_H__

#ifdef DREAMCAST

#include <cstdint>

#include <dc/pvr.h>

namespace dreamcast::rdp {

// PVR list + blend state derived from RDP other_mode_l/h (render mode).
struct BlendState {
    bool translucent = false;
    bool depth_write = true;
    bool blend_enable = false;
    int blend_src = PVR_BLEND_ONE;
    int blend_dst = PVR_BLEND_ZERO;
    bool alpha_threshold = false;
};

BlendState decode_blend(uint32_t other_mode_l, uint32_t other_mode_h, bool geometry_zbuffer);

} // namespace dreamcast::rdp

#endif // DREAMCAST

#endif // __DC_RDP_BLEND_H__
