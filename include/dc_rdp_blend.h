#ifndef __DC_RDP_BLEND_H__
#define __DC_RDP_BLEND_H__

#ifdef DREAMCAST

#include <cstdint>

#include <dc/pvr.h>

namespace dreamcast::rdp {

// PVR list + blend state derived from RDP other_mode_l/h (render mode).
struct BlendState {
    int list_type = PVR_LIST_OP_POLY;
    bool translucent = false;
    bool depth_write = true;
    bool blend_enable = false;
    int blend_src = PVR_BLEND_ONE;
    int blend_dst = PVR_BLEND_ZERO;
    int depth_compare = PVR_DEPTHCMP_GEQUAL;
    bool punch_through = false;
    uint8_t alpha_threshold = 0;
    bool zmode_decal = false;
    bool zmode_inter = false;
    bool zmode_xlu = false;
};

BlendState decode_blend(
    uint32_t other_mode_l,
    uint32_t other_mode_h,
    bool geometry_zbuffer,
    uint32_t blend_color_rgba);

} // namespace dreamcast::rdp

#endif // DREAMCAST

#endif // __DC_RDP_BLEND_H__
