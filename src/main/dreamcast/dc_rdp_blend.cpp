// Decode N64 RDP render-mode / blender state into PVR polygon context fields.
//
// Follows the gfx_pc heuristic: cycle blender B-mux != G_BL_A_MEM enables framebuffer
// blending. For 2-cycle modes the cycle-2 equation determines the final blend.

#ifdef DREAMCAST

#include "dc_rdp_blend.h"

namespace dreamcast::rdp {

namespace {

constexpr uint32_t G_MDSFT_ALPHACOMPARE = 0;
constexpr uint32_t G_MDSFT_CYCLETYPE = 20;

constexpr uint32_t G_CYC_2CYCLE = 1u << G_MDSFT_CYCLETYPE;
constexpr uint32_t G_CYC_COPY = 2u << G_MDSFT_CYCLETYPE;
constexpr uint32_t G_CYC_FILL = 3u << G_MDSFT_CYCLETYPE;

constexpr uint32_t G_AC_THRESHOLD = 1u << G_MDSFT_ALPHACOMPARE;

constexpr uint32_t Z_CMP = 0x0010;
constexpr uint32_t Z_UPD = 0x0020;
constexpr uint32_t FORCE_BL = 0x4000;

constexpr uint32_t ZMODE_OPA = 0x0000;
constexpr uint32_t ZMODE_INTER = 0x0400;
constexpr uint32_t ZMODE_XLU = 0x0800;
constexpr uint32_t ZMODE_DEC = 0x0C00;

constexpr uint32_t G_BL_CLR_IN = 0;
constexpr uint32_t G_BL_CLR_MEM = 1;
constexpr uint32_t G_BL_CLR_BL = 2;
constexpr uint32_t G_BL_CLR_FOG = 3;
constexpr uint32_t G_BL_1MA = 0;
constexpr uint32_t G_BL_A_MEM = 1;
constexpr uint32_t G_BL_A_IN = 0;
constexpr uint32_t G_BL_A_FOG = 1;
constexpr uint32_t G_BL_1 = 2;
constexpr uint32_t G_BL_0 = 3;

struct CycleBlend {
    uint32_t m1a = 0;
    uint32_t m1b = 0;
    uint32_t m2a = 0;
    uint32_t m2b = 0;
};

CycleBlend extract_cycle1(uint32_t other_mode_l) {
    return {
        (other_mode_l >> 30) & 3u,
        (other_mode_l >> 26) & 0xFu,
        (other_mode_l >> 22) & 0xFu,
        (other_mode_l >> 18) & 0xFu,
    };
}

CycleBlend extract_cycle2(uint32_t other_mode_l) {
    return {
        (other_mode_l >> 28) & 3u,
        (other_mode_l >> 24) & 0xFu,
        (other_mode_l >> 20) & 0xFu,
        (other_mode_l >> 16) & 0xFu,
    };
}

CycleBlend active_blender(uint32_t other_mode_l, uint32_t other_mode_h) {
    const uint32_t cycle = other_mode_h & (3u << G_MDSFT_CYCLETYPE);
    if (cycle == G_CYC_2CYCLE) {
        const CycleBlend c1 = extract_cycle1(other_mode_l);
        // Cycle 1 fog / pass-through: final framebuffer blend comes from cycle 2.
        if (c1.m1a == G_BL_CLR_FOG || (c1.m1a == 0 && c1.m1b == 0 && c1.m2a == 0 && c1.m2b == 0)) {
            return extract_cycle2(other_mode_l);
        }
    }
    return extract_cycle1(other_mode_l);
}

bool is_opaque_replace(const CycleBlend& blend) {
    return blend.m1a == G_BL_CLR_IN && blend.m1b == G_BL_0
        && blend.m2a == G_BL_CLR_IN && blend.m2b == G_BL_1;
}

bool is_visibility_cvg(const CycleBlend& blend) {
    // G_RM_VISCVG: IN*0 + BL*A_MEM — coverage visualization, not framebuffer XLU.
    return blend.m1a == G_BL_CLR_IN && blend.m1b == G_BL_0
        && blend.m2a == G_BL_CLR_BL && blend.m2b == G_BL_A_MEM;
}

bool blends_framebuffer(const CycleBlend& blend, bool force_bl) {
    if (!force_bl) {
        return false;
    }
    if (is_opaque_replace(blend) || is_visibility_cvg(blend)) {
        return false;
    }
    // gfx_pc: AA opaque modes use G_BL_A_MEM on the B mux.
    if (blend.m2b == G_BL_A_MEM) {
        return false;
    }
    return blend.m2a == G_BL_CLR_MEM
        || blend.m1b == G_BL_A_IN
        || blend.m1b == G_BL_A_FOG
        || blend.m2b == G_BL_1MA
        || blend.m2b == G_BL_1;
}

void map_pvr_blend(const CycleBlend& blend, int& src, int& dst) {
    if (blend.m1b == G_BL_A_FOG && blend.m2b == G_BL_1 && blend.m2a == G_BL_CLR_MEM) {
        // G_RM_ADD-style fog accumulation: src + dst.
        src = PVR_BLEND_SRCALPHA;
        dst = PVR_BLEND_ONE;
        return;
    }
    if (blend.m1b == G_BL_A_IN && blend.m2b == G_BL_1MA && blend.m2a == G_BL_CLR_MEM) {
        src = PVR_BLEND_SRCALPHA;
        dst = PVR_BLEND_INVSRCALPHA;
        return;
    }
    if (blend.m1b == G_BL_A_IN && blend.m2b == G_BL_1 && blend.m2a == G_BL_CLR_MEM) {
        // Rare modes that multiply destination by one (additive XLU variant).
        src = PVR_BLEND_SRCALPHA;
        dst = PVR_BLEND_ONE;
        return;
    }
    src = PVR_BLEND_SRCALPHA;
    dst = PVR_BLEND_INVSRCALPHA;
}

void apply_zmode(BlendState& state, uint32_t other_mode_l, bool geometry_zbuffer) {
    const uint32_t zmode = other_mode_l & 0x0C00u;
    state.zmode_decal = zmode == ZMODE_DEC;
    state.zmode_inter = zmode == ZMODE_INTER;
    state.zmode_xlu = zmode == ZMODE_XLU;

    const bool z_cmp = (other_mode_l & Z_CMP) != 0;
    const bool z_upd = (other_mode_l & Z_UPD) != 0;

    if (!geometry_zbuffer || !z_cmp) {
        state.depth_compare = PVR_DEPTHCMP_ALWAYS;
        state.depth_write = false;
        return;
    }

    if (state.zmode_decal || state.zmode_inter) {
        // Decal / interpenetrating: test LEQUAL, never write Z (gfx_pc).
        state.depth_compare = PVR_DEPTHCMP_LEQUAL;
        state.depth_write = false;
        return;
    }

    state.depth_compare = PVR_DEPTHCMP_GEQUAL;
    state.depth_write = z_upd && !state.translucent && !state.zmode_xlu;
}

int resolve_list_type(const BlendState& state) {
    if (state.punch_through) {
        return PVR_LIST_PT_POLY;
    }
    if (state.translucent) {
        return PVR_LIST_TR_POLY;
    }
    return PVR_LIST_OP_POLY;
}

} // anonymous namespace

BlendState decode_blend(
    uint32_t other_mode_l,
    uint32_t other_mode_h,
    bool geometry_zbuffer,
    uint32_t blend_color_rgba) {

    BlendState state{};

    const uint32_t cycle = other_mode_h & (3u << G_MDSFT_CYCLETYPE);
    if (cycle == G_CYC_COPY || cycle == G_CYC_FILL) {
        state.depth_compare = PVR_DEPTHCMP_ALWAYS;
        state.depth_write = false;
        state.list_type = PVR_LIST_OP_POLY;
        return state;
    }

    const uint32_t alpha_compare = other_mode_l & (3u << G_MDSFT_ALPHACOMPARE);
    const bool force_bl = (other_mode_l & FORCE_BL) != 0;
    const CycleBlend blend = active_blender(other_mode_l, other_mode_h);
    const bool framebuffer_blend = blends_framebuffer(blend, force_bl);

    state.translucent = framebuffer_blend;
    state.blend_enable = framebuffer_blend;

    if (framebuffer_blend) {
        map_pvr_blend(blend, state.blend_src, state.blend_dst);
    }

    apply_zmode(state, other_mode_l, geometry_zbuffer);

    if (alpha_compare == G_AC_THRESHOLD) {
        // N64: reject pixels with coverage alpha < blend_color.a; PVR PT uses ~50% cutoff.
        state.punch_through = true;
        state.alpha_threshold = static_cast<uint8_t>(blend_color_rgba & 0xFFu);
        // Punch-through replaces soft translucency for cutout UI/text (G_AC_THRESHOLD + XLU_SURF).
        state.translucent = false;
        state.blend_enable = false;
    }

    state.list_type = resolve_list_type(state);
    return state;
}

} // namespace dreamcast::rdp

#endif // DREAMCAST
