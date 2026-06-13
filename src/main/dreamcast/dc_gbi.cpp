// Dreamcast F3DZEX2 / RDP display list high-level emulator.
//
// Gfx commands update RSP state; geometry is submitted directly to the PVR
// tile accelerator via dc_pvr_renderer (SM64 DC port architecture).

#ifdef DREAMCAST

#include "dc_gbi.h"
#include "dc_combiner.h"
#include "dc_math.h"
#include "dc_pvr_renderer.h"
#include "dc_rdp_blend.h"
#include "dc_texture_cache.h"
#include "dc_tmem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(DC_HAS_SH4ZAM)
#include <sh4zam/shz_scalar.h>
#endif

#include "ultramodern/ultra64.h"

namespace {

// ── F3DEX2 / F3DZEX2 Gfx opcodes ────────────────────────────────────
constexpr uint8_t G_NOOP = 0x00;
constexpr uint8_t G_VTX = 0x01;
constexpr uint8_t G_MODIFYVTX = 0x02;
constexpr uint8_t G_CULLDL = 0x03;
constexpr uint8_t G_BRANCH_W = 0x04;
constexpr uint8_t G_TRI1 = 0x05;
constexpr uint8_t G_TRI2 = 0x06;
constexpr uint8_t G_QUAD = 0x07;
constexpr uint8_t G_LINE3D = 0x08;
constexpr uint8_t G_SPECIAL_1 = 0xD5;
constexpr uint8_t G_DMA_IO = 0xD6;
constexpr uint8_t G_TEXTURE = 0xD7;
constexpr uint8_t G_POPMTX = 0xD8;
constexpr uint8_t G_GEOMETRYMODE = 0xD9;
constexpr uint8_t G_MTX = 0xDA;
constexpr uint8_t G_MOVEWORD = 0xDB;
constexpr uint8_t G_MOVEMEM = 0xDC;
constexpr uint8_t G_LOAD_UCODE = 0xDD;
constexpr uint8_t G_DL = 0xDE;
constexpr uint8_t G_ENDDL = 0xDF;
constexpr uint8_t G_SPNOOP = 0xE0;
constexpr uint8_t G_RDPHALF_1 = 0xE1;
constexpr uint8_t G_SETOTHERMODE_H = 0xE3;
constexpr uint8_t G_SETOTHERMODE_L = 0xE2;
constexpr uint8_t G_RDPHALF_2 = 0xF1;
constexpr uint8_t G_RDPSETOTHERMODE = 0xEF;

// RDP opcodes (high byte of w0 in HLE mode).
constexpr uint8_t G_SETCIMG = 0xFF;
constexpr uint8_t G_SETZIMG = 0xFE;
constexpr uint8_t G_SETTIMG = 0xFD;
constexpr uint8_t G_SETCOMBINE = 0xFC;
constexpr uint8_t G_SETENVCOLOR = 0xFB;
constexpr uint8_t G_SETPRIMCOLOR = 0xFA;
constexpr uint8_t G_SETBLENDCOLOR = 0xF9;
constexpr uint8_t G_SETFOGCOLOR = 0xF8;
constexpr uint8_t G_SETFILLCOLOR = 0xF7;
constexpr uint8_t G_FILLRECT = 0xF6;
constexpr uint8_t G_SETTILE = 0xF5;
constexpr uint8_t G_LOADTILE = 0xF4;
constexpr uint8_t G_LOADBLOCK = 0xF3;
constexpr uint8_t G_SETTILESIZE = 0xF2;
constexpr uint8_t G_LOADTLUT = 0xF0;
constexpr uint8_t G_SETSCISSOR = 0xED;
constexpr uint8_t G_RDPFULLSYNC = 0xE9;
constexpr uint8_t G_RDPTILESYNC = 0xE8;
constexpr uint8_t G_RDPPIPESYNC = 0xE7;
constexpr uint8_t G_RDPLOADSYNC = 0xE6;
constexpr uint8_t G_TEXRECT = 0xE4;
constexpr uint8_t G_TEXRECTFLIP = 0xE5;
constexpr uint8_t G_RDPNOOP = 0xC0;

// F3DEX2 constants.
constexpr uint8_t G_MTX_PROJECTION = 0x04;
constexpr uint8_t G_MTX_LOAD = 0x02;
constexpr uint8_t G_MTX_PUSH = 0x01;
constexpr uint8_t G_MTX_MODELVIEW = 0x00;

constexpr uint32_t G_ZBUFFER = 0x00000001;
constexpr uint32_t G_SHADE = 0x00000004;
constexpr uint32_t G_FOG = 0x00010000;
constexpr uint32_t G_LIGHTING = 0x00020000;
constexpr uint32_t G_CULL_FRONT = 0x00000200;
constexpr uint32_t G_CULL_BACK = 0x00000400;
constexpr uint32_t G_CULL_BOTH = 0x00000600;
constexpr uint32_t G_TEXTURE_ENABLE = 0x00000000; // F3DEX2: texture on when G_TEXTURE level/on set

constexpr uint32_t G_MW_SEGMENT = 0x06;
constexpr uint32_t G_MW_NUMLIGHT = 0x02;
constexpr uint32_t G_MW_FOG = 0x08;
constexpr uint32_t G_MW_LIGHTCOL = 0x0A;
constexpr uint32_t G_MW_CLIP = 0x04;
constexpr uint32_t G_MW_FORCEMTX = 0x0C;
constexpr uint32_t G_MW_PERSPNORM = 0x0E;

constexpr uint32_t G_MDSFT_CYCLETYPE = 20;
constexpr uint32_t G_CYC_FILL = 3u << G_MDSFT_CYCLETYPE;
constexpr uint32_t G_CYC_COPY = 2u << G_MDSFT_CYCLETYPE;

constexpr uint32_t G_IM_SIZ_4b = 0;
constexpr uint32_t G_IM_SIZ_8b = 1;
constexpr uint32_t G_IM_SIZ_16b = 2;
constexpr uint32_t G_IM_SIZ_32b = 3;

constexpr uint32_t G_IM_FMT_RGBA = 0;
constexpr uint32_t G_IM_FMT_CI = 2;
constexpr uint32_t G_IM_FMT_IA = 3;
constexpr uint32_t G_IM_FMT_I = 4;

constexpr uint8_t G_TX_RENDERTILE = 0;
constexpr uint8_t G_TX_LOADTILE = 7;

constexpr uint32_t G_TEXTURE_IMAGE_FRAC = 2;

constexpr uint32_t G_CCMUX_TEXEL0 = 1;
constexpr uint32_t G_CCMUX_TEXEL1 = 2;
constexpr uint32_t G_CCMUX_PRIMITIVE = 3;
constexpr uint32_t G_CCMUX_SHADE = 4;
constexpr uint32_t G_CCMUX_ENVIRONMENT = 5;

constexpr uint32_t RT64_HOOK_MAGIC = 0x525464;
constexpr uint32_t RT64_HOOK_OP_ENABLE = 0x1;
constexpr uint32_t RT64_HOOK_OP_DISABLE = 0x2;
constexpr uint32_t RT64_HOOK_OP_DL = 0x3;
constexpr uint32_t RT64_HOOK_OP_BRANCH = 0x4;

constexpr uint8_t G_EX_NOOP = 0x00;
constexpr uint8_t G_EX_FILLRECT_V1 = 0x03;
constexpr uint8_t G_EX_SETVIEWPORT_V1 = 0x04;
constexpr uint8_t G_EX_SETSCISSOR_V1 = 0x05;
constexpr uint8_t G_EX_SETRECTALIGN_V1 = 0x06;
constexpr uint8_t G_EX_SETSCISSORALIGN_V1 = 0x08;
constexpr uint8_t G_EX_TEXRECT_V1 = 0x02;
constexpr uint8_t G_EX_SETVIEWPORTALIGN_V1 = 0x07;
constexpr uint8_t G_EX_PUSHOTHERMODE_V1 = 0x19;
constexpr uint8_t G_EX_POPOTHERMODE_V1 = 0x1A;
constexpr uint8_t G_EX_PUSHCOMBINE_V1 = 0x1B;
constexpr uint8_t G_EX_POPCOMBINE_V1 = 0x1C;
constexpr uint8_t G_EX_PUSHPROJMATRIX_V1 = 0x1D;
constexpr uint8_t G_EX_POPPROJMATRIX_V1 = 0x1E;
constexpr uint8_t G_EX_PUSHGEOMETRYMODE_V1 = 0x29;
constexpr uint8_t G_EX_POPGEOMETRYMODE_V1 = 0x2A;
constexpr uint8_t G_EX_PUSHPRIMCOLOR_V1 = 0x27;
constexpr uint8_t G_EX_POPPRIMCOLOR_V1 = 0x28;
constexpr uint8_t G_EX_PUSHENVCOLOR_V1 = 0x1F;
constexpr uint8_t G_EX_POPENVCOLOR_V1 = 0x20;
constexpr uint8_t G_EX_FORCEBRANCH_V1 = 0x11;
constexpr uint8_t G_EX_VERTEX_V1 = 0x14;
constexpr uint8_t G_EX_PUSHVIEWPORT_V1 = 0x15;
constexpr uint8_t G_EX_POPVIEWPORT_V1 = 0x16;
constexpr uint8_t G_EX_PUSHSCISSOR_V1 = 0x17;
constexpr uint8_t G_EX_POPSCISSOR_V1 = 0x18;
constexpr uint8_t G_EX_VERTEXZTEST_V1 = 0x0A;
constexpr uint8_t G_EX_ENDVERTEXZTEST_V1 = 0x0B;
constexpr uint8_t G_EX_MATRIXGROUP_V1 = 0x0C;
constexpr uint8_t G_EX_POPMATRIXGROUP_V1 = 0x0D;
constexpr uint8_t G_EX_SETREFRESHRATE_V1 = 0x09;
constexpr uint8_t G_EX_SETRDRAMEXTENDED_V1 = 0x2C;

constexpr uint8_t G_S2DEX_OBJ_RECTANGLE = 0x0C;

constexpr size_t MAX_VERTICES = 256;
constexpr size_t MAX_SEGMENTS = 16;
constexpr size_t MATRIX_STACK_SIZE = 32;
constexpr size_t VIEWPORT_STACK_SIZE = 16;
constexpr size_t STATE_STACK_SIZE = 16;
constexpr float DEPTH_RANGE = 1024.0f;
constexpr float REF_WIDTH = 320.0f;

constexpr uint32_t G_EX_ORIGIN_NONE = 0x800;
constexpr uint32_t G_EX_ORIGIN_LEFT = 0x0;
constexpr uint32_t G_EX_ORIGIN_CENTER = 0x200;
constexpr uint32_t G_EX_ORIGIN_RIGHT = 0x400;

constexpr uint8_t G_S2DEX_BG_RECT_COPY = 0x0A;

constexpr uint16_t G_MWO_POINT_RGBA = 0x10;
constexpr uint16_t G_MWO_POINT_ST = 0x14;
constexpr uint16_t G_MWO_POINT_XYSCREEN = 0x18;
constexpr uint16_t G_MWO_POINT_ZSCREEN = 0x1C;

struct DisplayList {
    uint32_t w0;
    uint32_t w1;

    uint32_t p0(uint8_t pos, uint8_t bits) const {
        return (w0 >> pos) & ((1u << bits) - 1u);
    }

    uint32_t p1(uint8_t pos, uint8_t bits) const {
        return (w1 >> pos) & ((1u << bits) - 1u);
    }
};

struct Vp_t {
    int16_t vscale[4];
    int16_t vtrans[4];
};

struct N64Vertex {
    int16_t y;
    int16_t x;
    uint16_t flag;
    int16_t z;
    int16_t t;
    int16_t s;
    uint8_t a;
    uint8_t b;
    uint8_t g;
    uint8_t r;
};

struct FixedMatrix {
    int16_t integer[4][4];
    uint16_t frac[4][4];

    void to_float4x4(float m[4][4]) const {
        for (uint32_t i = 0; i < 4; i++) {
            for (uint32_t j = 0; j < 4; j++) {
                const int xor_j = static_cast<int>(j ^ 1);
                const uint32_t word = (static_cast<uint32_t>(integer[i][xor_j]) << 16) | frac[i][xor_j];
                m[i][j] = static_cast<int32_t>(word) / 65536.0f;
            }
        }
    }
};

struct Viewport {
    float scale[3] = {1.0f, 1.0f, 1.0f};
    float translate[3] = {};
};

struct TransformedVertex {
    float screen_x = 0.0f;
    float screen_y = 0.0f;
    float depth = 0.5f;
    float screen_z = 0.0f;
    float tex_u = 0.0f;
    float tex_v = 0.0f;
    float w = 1.0f;
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
    uint8_t a = 255;
    uint8_t fog_alpha = 255;
};

struct ScissorRect {
    int32_t ulx = 0;
    int32_t uly = 0;
    int32_t lrx = 2048;
    int32_t lry = 2048;
    bool enabled = false;

    bool contains_pixel(int x, int y) const {
        if (!enabled) {
            return true;
        }
        const int fx = x * 4;
        const int fy = y * 4;
        return fx >= ulx && fx < lrx && fy >= uly && fy < lry;
    }
};

struct RectAlign {
    int32_t left_origin = G_EX_ORIGIN_NONE;
    int32_t right_origin = G_EX_ORIGIN_NONE;
    int32_t left_offset = 0;
    int32_t top_offset = 0;
    int32_t right_offset = 0;
    int32_t bottom_offset = 0;
};

struct ViewportAlign {
    int32_t origin = G_EX_ORIGIN_NONE;
    int32_t x_offset = 0;
    int32_t y_offset = 0;
};

struct ScissorAlign {
    int32_t left_origin = G_EX_ORIGIN_NONE;
    int32_t right_origin = G_EX_ORIGIN_NONE;
    int32_t ulx_offset = 0;
    int32_t uly_offset = 0;
    int32_t lrx_offset = 0;
    int32_t lry_offset = 0;
    int32_t ulx_bound = 0;
    int32_t uly_bound = 0;
    int32_t lrx_bound = 2048;
    int32_t lry_bound = 2048;
};

using dreamcast::math::mat4_identity;
using dreamcast::math::mat4_mul;
using dreamcast::math::mat4_transform;

// The render/texture/combiner subsystems live under the dreamcast:: namespace;
// alias them so the file-scope interpreter state can refer to them unqualified.
namespace pvr = dreamcast::pvr;
namespace tex = dreamcast::tex;
namespace tmem = dreamcast::tmem;
namespace combiner = dreamcast::combiner;
namespace rdp = dreamcast::rdp;

// ── Interpreter state ───────────────────────────────────────────────

struct TextureImage {
    uint32_t offset = 0; // physical RDRAM byte offset of the source image
    uint8_t siz = G_IM_SIZ_16b;
    uint16_t width = 0;
};

struct TileDescriptor {
    uint8_t fmt = 0;
    uint8_t siz = G_IM_SIZ_16b;
    uint8_t cms = 0;
    uint8_t cmt = 0;
    uint16_t line_size_bytes = 0;
    uint16_t uls = 0;
    uint16_t ult = 0;
    uint16_t lrs = 0;
    uint16_t lrt = 0;
};

struct LoadedTextureSlot {
    const uint8_t* addr = nullptr;
    uint32_t size_bytes = 0;
    bool valid = false;
};

struct GbiState {
    uint8_t* rdram = nullptr;
    pvr::Renderer* renderer = nullptr;
    tex::Cache* texture_cache = nullptr;

    std::array<uint32_t, MAX_SEGMENTS> segments{};
    float model_matrix[4][4]{};
    int model_stack_size = 1;
    float model_stack[MATRIX_STACK_SIZE][4][4]{};

    float proj_matrix[4][4]{};
    float mvp_matrix[4][4]{};
    bool mvp_dirty = true;

    Viewport viewport{};
    std::array<Viewport, VIEWPORT_STACK_SIZE> viewport_stack{};
    int viewport_stack_size = 1;
    std::array<ScissorRect, 16> scissor_stack{};
    int scissor_stack_size = 1;
    RectAlign rect_align{};
    ViewportAlign viewport_align{};
    ScissorAlign scissor_align{};

    std::array<uint32_t, STATE_STACK_SIZE> geometry_mode_stack{};
    std::array<uint64_t, STATE_STACK_SIZE> combine_stack{};
    std::array<uint32_t, STATE_STACK_SIZE> other_mode_h_stack{};
    std::array<uint32_t, STATE_STACK_SIZE> other_mode_l_stack{};
    std::array<uint32_t, STATE_STACK_SIZE> prim_color_stack{};
    std::array<uint32_t, STATE_STACK_SIZE> env_color_stack{};
    int geometry_mode_stack_size = 0;
    int combine_stack_size = 0;
    int other_mode_h_stack_size = 0;
    int other_mode_l_stack_size = 0;
    int prim_color_stack_size = 0;
    int env_color_stack_size = 0;

    float proj_matrix_stack[MATRIX_STACK_SIZE][4][4]{};
    int proj_stack_size = 1;

    uint32_t geometry_mode = G_CULL_BACK;
    uint32_t other_mode_h = 0x080CFF;
    uint32_t other_mode_l = 0;

    std::array<N64Vertex, MAX_VERTICES> vtx_buffer{};
    std::array<TransformedVertex, MAX_VERTICES> xf_buffer{};
    std::array<uint8_t, MAX_VERTICES> vtx_loaded{};

    struct ColorImage {
        uint32_t address = 0;
        uint16_t width = 320;
        uint8_t fmt = 0;
        uint8_t siz = G_IM_SIZ_16b;
    } color_image;

    struct ZImage {
        uint32_t address = 0;
        uint16_t width = 320;
    } z_image;

    bool vertex_ztest_skip = false;

    uint32_t prim_color = 0xFFFFFFFF;
    uint32_t fill_color = 0;
    uint32_t env_color = 0xFFFFFFFF;
    uint32_t blend_color = 0;
    uint32_t fog_color = 0;
    uint32_t fog_factor = 0;
    uint64_t combine_mode = 0;

    tmem::Buffer tmem{};
    uint32_t tmem_offset = 0;

    TextureImage texture_to_load{};
    TileDescriptor render_tile{};
    TileDescriptor tile1_desc{};
    bool tile1_desc_valid = false;
    std::array<LoadedTextureSlot, 2> loaded_textures{};
    uint8_t load_tile_slot = 0;
    uint16_t persp_norm = 0x4000;
    const uint8_t* palette = nullptr;
    std::array<uint8_t, 512> palette_buf{}; // de-swizzled TLUT (max 256 16-bit entries)

    uint16_t texture_scale_s = 0xFFFF;
    uint16_t texture_scale_t = 0xFFFF;
    bool texture_on = false;
    bool texture_changed = true;
    bool force_branch = true;
    bool s2dex_active = false;
    uint8_t extended_opcode = 0;

    // ── Vertex lighting (F3DEX2) ────────────────────────────────────
    // raw_lights holds up to 7 directional lights followed by the ambient
    // light (G_MV_LIGHT slots). num_dir_lights is the directional count from
    // G_MW_NUMLIGHT; the ambient light is at raw_lights[num_dir_lights].
    struct RawLight {
        uint8_t col[3];
        int8_t dir[3];
    };
    std::array<RawLight, 8> raw_lights{};
    int num_dir_lights = 0;
    bool lights_dirty = true;
    float light_coeffs[8][3]{}; // directional light dirs transformed to object space

    std::vector<DisplayList*> dl_stack;

    float fb_width() const {
        return static_cast<float>(std::max<uint16_t>(color_image.width, 320));
    }

    bool zbuffer_enabled() const {
        return (geometry_mode & G_ZBUFFER) != 0;
    }

    rdp::BlendState current_blend_state() const {
        return rdp::decode_blend(other_mode_l, other_mode_h, zbuffer_enabled(), blend_color);
    }

    static float compute_pvr_depth(float ndc_z) {
        // PVR DEPTHCMP_GEQUAL treats larger Z as nearer.
        return std::clamp((1.0f - ndc_z) * 0.5f, 0.0f, 1.0f);
    }

    float origin_offset_x(int32_t origin, int32_t offset) const {
        switch (origin & 0xF00) {
        case G_EX_ORIGIN_LEFT:
            return static_cast<float>(offset) / 4.0f;
        case G_EX_ORIGIN_CENTER:
            return (fb_width() - REF_WIDTH) * 0.5f + static_cast<float>(offset) / 4.0f;
        case G_EX_ORIGIN_RIGHT:
            return fb_width() - REF_WIDTH + static_cast<float>(offset) / 4.0f;
        default:
            return static_cast<float>(offset) / 4.0f;
        }
    }

    float origin_offset_y(int32_t origin, int32_t offset) const {
        (void)origin;
        return static_cast<float>(offset) / 4.0f;
    }

    int32_t apply_rect_origin_x(int32_t origin, int32_t value, int32_t offset) const {
        if ((origin & 0xF00) == G_EX_ORIGIN_NONE) {
            return value;
        }
        return static_cast<int32_t>(origin_offset_x(origin, offset) * 4.0f) + value;
    }

    Viewport effective_viewport() const {
        Viewport vp = viewport;
        if ((viewport_align.origin & 0xF00) != G_EX_ORIGIN_NONE) {
            vp.translate[0] += origin_offset_x(viewport_align.origin, viewport_align.x_offset);
            vp.translate[1] += origin_offset_y(viewport_align.origin, viewport_align.y_offset);
        }
        return vp;
    }

    ScissorRect effective_scissor() const {
        ScissorRect sc = scissor_stack[scissor_stack_size - 1];
        if ((scissor_align.left_origin & 0xF00) != G_EX_ORIGIN_NONE
            || (scissor_align.right_origin & 0xF00) != G_EX_ORIGIN_NONE) {
            // Offsets and bounds arrive in 1/4-pixel units from the extended DL macros.
            const int32_t left_anchor = static_cast<int32_t>(origin_offset_x(scissor_align.left_origin, 0) * 4.0f);
            const int32_t right_anchor = static_cast<int32_t>((origin_offset_x(scissor_align.right_origin, 0) + fb_width() - REF_WIDTH) * 4.0f);
            sc.ulx = left_anchor + scissor_align.ulx_bound + scissor_align.ulx_offset;
            sc.uly = scissor_align.uly_bound + scissor_align.uly_offset;
            sc.lrx = right_anchor + scissor_align.lrx_bound + scissor_align.lrx_offset;
            sc.lry = scissor_align.lry_bound + scissor_align.lry_offset;
            sc.enabled = true;
        }
        return sc;
    }

    bool passes_scissor(float screen_x, float screen_y) const {
        const ScissorRect sc = effective_scissor();
        return sc.contains_pixel(static_cast<int>(screen_x), static_cast<int>(screen_y));
    }

    uint32_t from_segmented(uint32_t seg_addr) const {
        return segments[(seg_addr >> 24) & 0x0F] + (seg_addr & 0x00FFFFFF);
    }

    uint32_t from_segmented_masked(uint32_t seg_addr) const {
        return from_segmented(seg_addr) & 0x00FFFFF8;
    }

    // RDRAM is stored as host-native 32-bit words (big-endian N64 words byte-
    // swapped), so byte reads are XOR-3 swizzled. These helpers read logical
    // (big-endian) values from a physical RDRAM byte offset.
    uint8_t rdram_u8(uint32_t addr) const {
        return rdram[addr ^ 3u];
    }

    uint16_t rdram_be16(uint32_t addr) const {
        return static_cast<uint16_t>((rdram_u8(addr) << 8) | rdram_u8(addr + 1));
    }

    uint32_t rdram_be32(uint32_t addr) const {
        return (static_cast<uint32_t>(rdram_u8(addr)) << 24)
             | (static_cast<uint32_t>(rdram_u8(addr + 1)) << 16)
             | (static_cast<uint32_t>(rdram_u8(addr + 2)) << 8)
             | static_cast<uint32_t>(rdram_u8(addr + 3));
    }

    void recompute_mvp() {
        mat4_mul(model_matrix, proj_matrix, mvp_matrix);
        mvp_dirty = false;
        // Light coefficients depend on the modelview, so they go stale here.
        lights_dirty = true;
    }

    // Transform each directional light's world-space direction into object
    // space using the modelview's upper-left 3x3 (matching the F3DEX2 RSP /
    // gfx_pc convention), then normalize. The dot of this with the object-space
    // vertex normal gives the diffuse intensity.
    void compute_light_coeffs() {
        for (int i = 0; i < num_dir_lights; i++) {
            const float lx = static_cast<float>(raw_lights[i].dir[0]) / 127.0f;
            const float ly = static_cast<float>(raw_lights[i].dir[1]) / 127.0f;
            const float lz = static_cast<float>(raw_lights[i].dir[2]) / 127.0f;
            float cx = lx * model_matrix[0][0] + ly * model_matrix[0][1] + lz * model_matrix[0][2];
            float cy = lx * model_matrix[1][0] + ly * model_matrix[1][1] + lz * model_matrix[1][2];
            float cz = lx * model_matrix[2][0] + ly * model_matrix[2][1] + lz * model_matrix[2][2];
            const float len = std::sqrt(cx * cx + cy * cy + cz * cz);
            if (len > 1e-6f) {
                const float inv = 1.0f / len;
                cx *= inv; cy *= inv; cz *= inv;
            }
            light_coeffs[i][0] = cx;
            light_coeffs[i][1] = cy;
            light_coeffs[i][2] = cz;
        }
        lights_dirty = false;
    }

    void light_vertex(const N64Vertex& v, TransformedVertex& out) {
        if (lights_dirty) {
            compute_light_coeffs();
        }
        // Vertex normal bytes are signed; the RGB fields map to normal x/y/z.
        const float nx = static_cast<float>(static_cast<int8_t>(v.r));
        const float ny = static_cast<float>(static_cast<int8_t>(v.g));
        const float nz = static_cast<float>(static_cast<int8_t>(v.b));

        const RawLight& ambient = raw_lights[num_dir_lights];
        float r = static_cast<float>(ambient.col[0]);
        float g = static_cast<float>(ambient.col[1]);
        float b = static_cast<float>(ambient.col[2]);

        for (int i = 0; i < num_dir_lights; i++) {
            float intensity = (nx * light_coeffs[i][0] + ny * light_coeffs[i][1] + nz * light_coeffs[i][2]) / 127.0f;
            if (intensity > 0.0f) {
                r += intensity * static_cast<float>(raw_lights[i].col[0]);
                g += intensity * static_cast<float>(raw_lights[i].col[1]);
                b += intensity * static_cast<float>(raw_lights[i].col[2]);
            }
        }

        out.r = static_cast<uint8_t>(std::min(r, 255.0f));
        out.g = static_cast<uint8_t>(std::min(g, 255.0f));
        out.b = static_cast<uint8_t>(std::min(b, 255.0f));
        out.a = v.a; // vertex alpha is preserved under lighting
    }

    void transform_vertex(uint32_t index) {
        if (index >= MAX_VERTICES) {
            return;
        }

        if (mvp_dirty) {
            recompute_mvp();
        }

        const N64Vertex& v = vtx_buffer[index];
        float tx, ty, tz, tw;
        mat4_transform(mvp_matrix, static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z), tx, ty, tz, tw);
        if (std::fabs(tw) < 1e-6f) {
            tw = 1.0f;
        }

        TransformedVertex& out = xf_buffer[index];
        out.w = tw;
        const Viewport vp = effective_viewport();
        const float inv_w = 1.0f / tw;
        out.screen_x = (tx * inv_w) * vp.scale[0] + vp.translate[0];
        out.screen_y = (ty * -inv_w) * vp.scale[1] + vp.translate[1];
        const float ndc_z = tz * inv_w;
        out.depth = compute_pvr_depth(ndc_z);
        out.screen_z = compute_screen_z(ndc_z, vp);
        out.fog_alpha = (geometry_mode & G_FOG) != 0 ? compute_fog_alpha(out.screen_z) : 255;
        float tex_u = static_cast<float>((static_cast<int32_t>(v.s) * static_cast<int32_t>(texture_scale_s)) >> 16);
        float tex_v = static_cast<float>((static_cast<int32_t>(v.t) * static_cast<int32_t>(texture_scale_t)) >> 16);
        if (persp_norm > 0) {
            const float persp_scale = static_cast<float>(persp_norm) / (tw * 65536.0f);
            tex_u *= persp_scale;
            tex_v *= persp_scale;
        }
        out.tex_u = tex_u;
        out.tex_v = tex_v;
        if (geometry_mode & G_LIGHTING) {
            light_vertex(v, out);
        } else {
            out.r = v.r;
            out.g = v.g;
            out.b = v.b;
            out.a = v.a;
        }
        vtx_loaded[index] = 1;
    }

    static uint8_t mul_u8(uint8_t a, uint8_t b) {
        return static_cast<uint8_t>((static_cast<uint16_t>(a) * b) / 255u);
    }

    void unpack_color(uint32_t rgba, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const {
        r = static_cast<uint8_t>((rgba >> 24) & 0xFF);
        g = static_cast<uint8_t>((rgba >> 16) & 0xFF);
        b = static_cast<uint8_t>((rgba >> 8) & 0xFF);
        a = static_cast<uint8_t>(rgba & 0xFF);
    }

    static void unpack_argb(uint32_t argb, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
        a = static_cast<uint8_t>((argb >> 24) & 0xFF);
        r = static_cast<uint8_t>((argb >> 16) & 0xFF);
        g = static_cast<uint8_t>((argb >> 8) & 0xFF);
        b = static_cast<uint8_t>(argb & 0xFF);
    }

    bool combine_uses_texel0() const {
        return combiner::uses_texel0(combine_mode);
    }

    bool combine_uses_texel1() const {
        return combiner::uses_texel1(combine_mode);
    }

    combiner::ColorSource to_combiner_color(const tex::TexelColor& texel) const {
        return {texel.r, texel.g, texel.b, texel.a};
    }

    combiner::ColorSource sample_texel0(float raw_u, float raw_v) const {
        if (!loaded_textures[0].valid) {
            return {255, 255, 255, 255};
        }

        tex::LoadedTexture tex{};
        tex.addr = loaded_textures[0].addr;
        tex.size_bytes = loaded_textures[0].size_bytes;

        tex::TileState tile{};
        tile.fmt = render_tile.fmt;
        tile.siz = render_tile.siz;
        tile.uls = render_tile.uls;
        tile.ult = render_tile.ult;
        tile.lrs = render_tile.lrs;
        tile.lrt = render_tile.lrt;
        tile.line_size_bytes = render_tile.line_size_bytes;

        const int x = static_cast<int>((raw_u - render_tile.uls * 8.0f) / 32.0f);
        const int y = static_cast<int>((raw_v - render_tile.ult * 8.0f) / 32.0f);
        return to_combiner_color(tex::sample_texel(tex, tile, palette, x, y));
    }

    combiner::ColorSource sample_texel1(float raw_u, float raw_v) const {
        if (!loaded_textures[1].valid) {
            return {255, 255, 255, 255};
        }

        tex::LoadedTexture tex{};
        tex.addr = loaded_textures[1].addr;
        tex.size_bytes = loaded_textures[1].size_bytes;

        const TileDescriptor& desc = tile1_desc_valid ? tile1_desc : render_tile;
        tex::TileState tile{};
        tile.fmt = desc.fmt;
        tile.siz = desc.siz;
        tile.uls = desc.uls;
        tile.ult = desc.ult;
        tile.lrs = desc.lrs;
        tile.lrt = desc.lrt;
        tile.line_size_bytes = desc.line_size_bytes;

        const int x = static_cast<int>((raw_u - desc.uls * 8.0f) / 32.0f);
        const int y = static_cast<int>((raw_v - desc.ult * 8.0f) / 32.0f);
        return to_combiner_color(tex::sample_texel(tex, tile, palette, x, y));
    }

    uint32_t vertex_combine_factor(
        uint8_t vr, uint8_t vg, uint8_t vb, uint8_t va,
        const combiner::ColorSource& texel0 = {255, 255, 255, 255},
        const combiner::ColorSource& texel1 = {255, 255, 255, 255}) const {
        combiner::Inputs inputs{};
        inputs.shade = {vr, vg, vb, va};
        unpack_color(prim_color, inputs.prim.r, inputs.prim.g, inputs.prim.b, inputs.prim.a);
        unpack_color(env_color, inputs.env.r, inputs.env.g, inputs.env.b, inputs.env.a);
        inputs.texel0 = texel0;
        inputs.texel1 = texel1;
        const bool two_cycle = (other_mode_h & (1u << G_MDSFT_CYCLETYPE)) != 0;
        return combiner::evaluate(combine_mode, inputs, two_cycle);
    }

    bool vertex_in_view(const TransformedVertex& vert) const {
        if (vert.w <= 0.0f) {
            return false;
        }
        constexpr float margin = 64.0f;
        const float max_x = fb_width() + margin;
        const float max_y = 240.0f + margin;
        return vert.screen_x >= -margin && vert.screen_x <= max_x
            && vert.screen_y >= -margin && vert.screen_y <= max_y;
    }

    bool should_cull_dl(uint8_t vfirst, uint8_t vend) {
        if (vfirst > vend || vend >= MAX_VERTICES) {
            return false;
        }
        for (uint8_t i = vfirst; i <= vend; i++) {
            if (!vtx_loaded[i]) {
                transform_vertex(i);
            }
            if (vertex_in_view(xf_buffer[i])) {
                return false;
            }
        }
        return true;
    }

    void apply_modify_vtx(uint8_t index, uint8_t where, uint32_t value) {
        if (index >= MAX_VERTICES) {
            return;
        }

        N64Vertex& vtx = vtx_buffer[index];
        TransformedVertex& xf = xf_buffer[index];

        switch (where) {
        case G_MWO_POINT_RGBA:
            vtx.r = static_cast<uint8_t>((value >> 24) & 0xFF);
            vtx.g = static_cast<uint8_t>((value >> 16) & 0xFF);
            vtx.b = static_cast<uint8_t>((value >> 8) & 0xFF);
            vtx.a = static_cast<uint8_t>(value & 0xFF);
            xf.r = vtx.r;
            xf.g = vtx.g;
            xf.b = vtx.b;
            xf.a = vtx.a;
            break;
        case G_MWO_POINT_ST:
            vtx.s = static_cast<int16_t>(value >> 16);
            vtx.t = static_cast<int16_t>(value & 0xFFFF);
            xf.tex_u = static_cast<float>((static_cast<int32_t>(vtx.s) * static_cast<int32_t>(texture_scale_s)) >> 16);
            xf.tex_v = static_cast<float>((static_cast<int32_t>(vtx.t) * static_cast<int32_t>(texture_scale_t)) >> 16);
            break;
        case G_MWO_POINT_XYSCREEN:
            xf.screen_x = static_cast<float>(static_cast<int16_t>(value >> 16)) / 4.0f;
            xf.screen_y = static_cast<float>(static_cast<int16_t>(value & 0xFFFF)) / 4.0f;
            break;
        case G_MWO_POINT_ZSCREEN:
            xf.screen_z = static_cast<float>(value >> 16);
            xf.depth = std::clamp(1.0f - xf.screen_z / 32768.0f, 0.0f, 1.0f);
            break;
        default:
            break;
        }
        vtx_loaded[index] = 1;
    }

    tex::Surface resolve_texture() {
        if (texture_cache == nullptr || !loaded_textures[0].valid) {
            return {};
        }
        tex::LoadedTexture tex{};
        tex.addr = loaded_textures[0].addr;
        tex.size_bytes = loaded_textures[0].size_bytes;

        tex::TileState tile{};
        tile.fmt = render_tile.fmt;
        tile.siz = render_tile.siz;
        tile.cms = render_tile.cms;
        tile.cmt = render_tile.cmt;
        tile.uls = render_tile.uls;
        tile.ult = render_tile.ult;
        tile.lrs = render_tile.lrs;
        tile.lrt = render_tile.lrt;
        tile.line_size_bytes = render_tile.line_size_bytes;

        return texture_cache->upload(tex, tile, palette);
    }

    void normalize_uv(float raw_u, float raw_v, float& u, float& v) const {
        const float tex_w = static_cast<float>(std::max<uint16_t>((render_tile.lrs - render_tile.uls + 4) / 4, 1));
        const float tex_h = static_cast<float>(std::max<uint16_t>((render_tile.lrt - render_tile.ult + 4) / 4, 1));
        u = (raw_u - render_tile.uls * 8.0f) / 32.0f / tex_w;
        v = (raw_v - render_tile.ult * 8.0f) / 32.0f / tex_h;
    }

    static uint32_t pack_argb(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        return (static_cast<uint32_t>(a) << 24)
             | (static_cast<uint32_t>(r) << 16)
             | (static_cast<uint32_t>(g) << 8)
             | static_cast<uint32_t>(b);
    }

    // N64 screen Z used by the RSP fog path: 32 * ((z/w) * vscale[2] + vtrans[2]).
    float compute_screen_z(float ndc_z, const Viewport& vp) const {
        const float raw_scale_z = vp.scale[2] * DEPTH_RANGE;
        const float raw_trans_z = vp.translate[2] * DEPTH_RANGE;
        return 32.0f * (ndc_z * raw_scale_z + raw_trans_z);
    }

    // Fog visibility alpha: 255 = no fog (near), 0 = full fog (far).
    uint8_t compute_fog_alpha(float screen_z) const {
        const uint16_t fog_min = static_cast<uint16_t>(fog_factor & 0xFFFFu);
        const uint16_t fog_max = static_cast<uint16_t>((fog_factor >> 16) & 0xFFFFu);
        if (fog_max <= fog_min) {
            return 255;
        }
        if (screen_z <= static_cast<float>(fog_min)) {
            return 255;
        }
        if (screen_z >= static_cast<float>(fog_max)) {
            return 0;
        }
#if defined(DC_HAS_SH4ZAM)
        const float range = static_cast<float>(fog_max - fog_min);
        const float t = (screen_z - static_cast<float>(fog_min)) * shz_divf(1.0f, range);
#else
        const float t = (screen_z - static_cast<float>(fog_min))
            / (static_cast<float>(fog_max) - static_cast<float>(fog_min));
#endif
        return static_cast<uint8_t>((1.0f - t) * 255.0f);
    }

    // G_RM_FOG_SHADE_A: lerp combiner output toward fog color using per-vertex fog alpha.
    uint32_t apply_fog_blend(uint32_t color, uint8_t fog_alpha) const {
        if ((geometry_mode & G_FOG) == 0) {
            return color;
        }

        uint8_t r, g, b, a;
        unpack_argb(color, r, g, b, a);
        uint8_t fr, fg, fb, fa;
        unpack_color(fog_color, fr, fg, fb, fa);
        (void)fa;

        const float visibility = fog_alpha / 255.0f;
        const float fog_weight = 1.0f - visibility;
#if defined(DC_HAS_SH4ZAM)
        return pack_argb(
            static_cast<uint8_t>(std::min(255.0f, shz_fmaf(r, visibility, fr * fog_weight))),
            static_cast<uint8_t>(std::min(255.0f, shz_fmaf(g, visibility, fg * fog_weight))),
            static_cast<uint8_t>(std::min(255.0f, shz_fmaf(b, visibility, fb * fog_weight))),
            a
        );
#else
        return pack_argb(
            static_cast<uint8_t>(std::min(255.0f, r * visibility + fr * fog_weight)),
            static_cast<uint8_t>(std::min(255.0f, g * visibility + fg * fog_weight)),
            static_cast<uint8_t>(std::min(255.0f, b * visibility + fb * fog_weight)),
            a
        );
#endif
    }

    void fill_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry,
                   int32_t left_origin = G_EX_ORIGIN_NONE, int32_t right_origin = G_EX_ORIGIN_NONE) {
        if (vertex_ztest_skip) {
            return;
        }

        ulx = apply_rect_origin_x(left_origin, ulx, rect_align.left_offset);
        uly += rect_align.top_offset;
        lrx = apply_rect_origin_x(right_origin, lrx, rect_align.right_offset);
        lry += rect_align.bottom_offset;

        if (lrx < ulx || lry < uly) {
            return;
        }

        const uint32_t cycle = other_mode_h & (3u << G_MDSFT_CYCLETYPE);
        uint32_t color = prim_color;
        if (cycle == G_CYC_FILL || cycle == G_CYC_COPY) {
            lrx |= 3;
            lry |= 3;
            color = fill_color;
        }

        uint8_t r, g, b, a;
        unpack_color(color, r, g, b, a);

        if (renderer != nullptr) {
            renderer->submit_fill_rect(ulx, uly, lrx, lry, pack_argb(r, g, b, a), current_blend_state(), zbuffer_enabled());
        }
    }

    void submit_triangle(uint8_t i0, uint8_t i1, uint8_t i2) {
        if (vertex_ztest_skip) {
            return;
        }

        if (i0 >= MAX_VERTICES || i1 >= MAX_VERTICES || i2 >= MAX_VERTICES) {
            return;
        }

        if (!vtx_loaded[i0]) transform_vertex(i0);
        if (!vtx_loaded[i1]) transform_vertex(i1);
        if (!vtx_loaded[i2]) transform_vertex(i2);

        const TransformedVertex& v0 = xf_buffer[i0];
        const TransformedVertex& v1 = xf_buffer[i1];
        const TransformedVertex& v2 = xf_buffer[i2];

        // Back-face culling (screen-space winding).
        const float edge1x = v1.screen_x - v0.screen_x;
        const float edge1y = v1.screen_y - v0.screen_y;
        const float edge2x = v2.screen_x - v0.screen_x;
        const float edge2y = v2.screen_y - v0.screen_y;
        const float cross = edge1x * edge2y - edge1y * edge2x;
        const bool front_face = cross >= 0.0f;

        if ((geometry_mode & G_CULL_BOTH) == G_CULL_BOTH) {
            return;
        }
        if ((geometry_mode & G_CULL_FRONT) && front_face) {
            return;
        }
        if ((geometry_mode & G_CULL_BACK) && !front_face) {
            return;
        }

        const ScissorRect scissor = effective_scissor();
        if (scissor.enabled) {
            const bool any_inside = passes_scissor(v0.screen_x, v0.screen_y)
                || passes_scissor(v1.screen_x, v1.screen_y)
                || passes_scissor(v2.screen_x, v2.screen_y);
            if (!any_inside) {
                return;
            }
        }

        if (renderer == nullptr) {
            return;
        }

        const bool use_shade = (geometry_mode & G_SHADE) != 0;
        uint8_t pr, pg, pb, pa;
        unpack_color(prim_color, pr, pg, pb, pa);

        uint8_t r0, g0, b0, a0;
        uint8_t r1, g1, b1, a1;
        uint8_t r2, g2, b2, a2;

        // When G_LIGHTING is active, transform_vertex has already replaced the
        // per-vertex RGB (originally normals) with the computed shade color.
        if (use_shade) {
            r0 = v0.r; g0 = v0.g; b0 = v0.b; a0 = v0.a;
            r1 = v1.r; g1 = v1.g; b1 = v1.b; a1 = v1.a;
            r2 = v2.r; g2 = v2.g; b2 = v2.b; a2 = v2.a;
        } else {
            r0 = r1 = r2 = pr;
            g0 = g1 = g2 = pg;
            b0 = b1 = b2 = pb;
            a0 = a1 = a2 = pa;
        }

        const bool use_texture = texture_on && loaded_textures[0].valid;
        const bool combiner_needs_texel0 = use_texture && combine_mode != 0 && combine_uses_texel0();
        const bool combiner_needs_texel1 = use_texture && combine_mode != 0 && combine_uses_texel1();
        const bool combiner_needs_texel = combiner_needs_texel0 || combiner_needs_texel1;

        const combiner::ColorSource tex0_0 = combiner_needs_texel0 ? sample_texel0(v0.tex_u, v0.tex_v) : combiner::ColorSource{255, 255, 255, 255};
        const combiner::ColorSource tex1_0 = combiner_needs_texel1 ? sample_texel1(v0.tex_u, v0.tex_v) : combiner::ColorSource{255, 255, 255, 255};
        const combiner::ColorSource tex0_1 = combiner_needs_texel0 ? sample_texel0(v1.tex_u, v1.tex_v) : combiner::ColorSource{255, 255, 255, 255};
        const combiner::ColorSource tex1_1 = combiner_needs_texel1 ? sample_texel1(v1.tex_u, v1.tex_v) : combiner::ColorSource{255, 255, 255, 255};
        const combiner::ColorSource tex0_2 = combiner_needs_texel0 ? sample_texel0(v2.tex_u, v2.tex_v) : combiner::ColorSource{255, 255, 255, 255};
        const combiner::ColorSource tex1_2 = combiner_needs_texel1 ? sample_texel1(v2.tex_u, v2.tex_v) : combiner::ColorSource{255, 255, 255, 255};

        uint32_t c0 = vertex_combine_factor(r0, g0, b0, a0, tex0_0, tex1_0);
        uint32_t c1 = vertex_combine_factor(r1, g1, b1, a1, tex0_1, tex1_1);
        uint32_t c2 = vertex_combine_factor(r2, g2, b2, a2, tex0_2, tex1_2);
        if (geometry_mode & G_FOG) {
            c0 = apply_fog_blend(c0, v0.fog_alpha);
            c1 = apply_fog_blend(c1, v1.fog_alpha);
            c2 = apply_fog_blend(c2, v2.fog_alpha);
        }
        const rdp::BlendState blend = current_blend_state();
        const bool z_enabled = zbuffer_enabled();

        if (use_texture && !combiner_needs_texel) {
            const tex::Surface surface = resolve_texture();
            float tu0, tv0, tu1, tv1, tu2, tv2;
            normalize_uv(v0.tex_u, v0.tex_v, tu0, tv0);
            normalize_uv(v1.tex_u, v1.tex_v, tu1, tv1);
            normalize_uv(v2.tex_u, v2.tex_v, tu2, tv2);
            renderer->submit_textured_triangle(
                v0.screen_x, v0.screen_y, v0.depth, tu0, tv0, c0,
                v1.screen_x, v1.screen_y, v1.depth, tu1, tv1, c1,
                v2.screen_x, v2.screen_y, v2.depth, tu2, tv2, c2,
                surface, blend, z_enabled
            );
        } else {
            renderer->submit_triangle(
                v0.screen_x, v0.screen_y, v0.depth, c0,
                v1.screen_x, v1.screen_y, v1.depth, c1,
                v2.screen_x, v2.screen_y, v2.depth, c2,
                blend, z_enabled
            );
        }
    }

    void draw_tex_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip,
                       int32_t left_origin = G_EX_ORIGIN_NONE, int32_t right_origin = G_EX_ORIGIN_NONE) {
        if (renderer == nullptr || vertex_ztest_skip) {
            return;
        }

        ulx = apply_rect_origin_x(left_origin, ulx, rect_align.left_offset);
        uly += rect_align.top_offset;
        lrx = apply_rect_origin_x(right_origin, lrx, rect_align.right_offset);
        lry += rect_align.bottom_offset;

        if (flip) {
            dsdx = static_cast<int16_t>(-dsdx);
            dtdy = static_cast<int16_t>(-dtdy);
        }

        const int16_t width = flip ? static_cast<int16_t>(lry - uly) : static_cast<int16_t>(lrx - ulx);
        const int16_t height = flip ? static_cast<int16_t>(lrx - ulx) : static_cast<int16_t>(lry - uly);
        const float lrs = static_cast<float>(((uls << 7) + dsdx * width) >> 7);
        const float lrt = static_cast<float>(((ult << 7) + dtdy * height) >> 7);

        const tex::Surface surface = resolve_texture();
        uint32_t vtx_color = vertex_combine_factor(255, 255, 255, 255);
        if (geometry_mode & G_FOG) {
            vtx_color = apply_fog_blend(vtx_color, compute_fog_alpha(0.0f));
        }
        renderer->submit_tex_rect(
            ulx, uly, lrx, lry,
            static_cast<float>(uls), static_cast<float>(ult), lrs, lrt,
            surface, vtx_color, current_blend_state(), zbuffer_enabled());
    }

    void draw_tri(uint8_t a, uint8_t b, uint8_t c) {
        submit_triangle(a, b, c);
    }

    void load_vertices(uint32_t address, uint8_t count, uint8_t dst_index) {
        if (dst_index + count > MAX_VERTICES) {
            return;
        }

        const uint32_t phys = from_segmented_masked(address);
        const N64Vertex* src = reinterpret_cast<const N64Vertex*>(rdram + phys);
        memcpy(&vtx_buffer[dst_index], src, sizeof(N64Vertex) * count);
        for (uint8_t i = 0; i < count; i++) {
            vtx_loaded[dst_index + i] = 0;
            transform_vertex(dst_index + i);
        }
    }

    void set_viewport(uint32_t address) {
        const uint32_t phys = from_segmented_masked(address);
        const Vp_t* vp = reinterpret_cast<const Vp_t*>(rdram + phys);
        viewport.scale[0] = static_cast<float>(vp->vscale[1]) / 4.0f;
        viewport.scale[1] = static_cast<float>(vp->vscale[0]) / 4.0f;
        viewport.scale[2] = static_cast<float>(vp->vscale[3]) / DEPTH_RANGE;
        viewport.translate[0] = static_cast<float>(vp->vtrans[1]) / 4.0f;
        viewport.translate[1] = static_cast<float>(vp->vtrans[0]) / 4.0f;
        viewport.translate[2] = static_cast<float>(vp->vtrans[3]) / DEPTH_RANGE;
        if (viewport_stack_size > 0) {
            viewport_stack[viewport_stack_size - 1] = viewport;
        }
    }

    void matrix_op(uint32_t address, uint8_t params) {
        const uint32_t phys = from_segmented_masked(address);
        FixedMatrix fixed{};
        memcpy(&fixed, rdram + phys, sizeof(FixedMatrix));

        float incoming[4][4];
        fixed.to_float4x4(incoming);

        const bool projection = (params & G_MTX_PROJECTION) != 0;
        const bool load = (params & G_MTX_LOAD) != 0;
        const bool push = (params & G_MTX_PUSH) != 0;

        if (projection) {
            if (load) {
                memcpy(proj_matrix, incoming, sizeof(proj_matrix));
            } else {
                float temp[4][4];
                mat4_mul(incoming, proj_matrix, temp);
                memcpy(proj_matrix, temp, sizeof(proj_matrix));
            }
        } else {
            if (push && model_stack_size < MATRIX_STACK_SIZE) {
                memcpy(model_stack[model_stack_size], model_matrix, sizeof(model_matrix));
                model_stack_size++;
            }

            if (load) {
                memcpy(model_matrix, incoming, sizeof(model_matrix));
            } else {
                float temp[4][4];
                mat4_mul(incoming, model_matrix, temp);
                memcpy(model_matrix, temp, sizeof(model_matrix));
            }
        }

        mvp_dirty = true;
    }

    void pop_matrix(uint32_t count) {
        while (count-- > 0 && model_stack_size > 1) {
            model_stack_size--;
            memcpy(model_matrix, model_stack[model_stack_size - 1], sizeof(model_matrix));
            mvp_dirty = true;
        }
    }

    void set_color_image(uint8_t fmt, uint8_t siz, uint16_t width, uint32_t address) {
        color_image.fmt = fmt;
        color_image.siz = siz;
        color_image.width = width;
        color_image.address = from_segmented(address) & 0x00FFFFFF;
        if (renderer != nullptr && width > 0) {
            renderer->set_framebuffer_size(width, 240);
        }
    }

    void set_scissor(uint8_t mode, int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
        ScissorRect& sc = scissor_stack[scissor_stack_size - 1];
        sc.ulx = ulx;
        sc.uly = uly;
        sc.lrx = lrx;
        sc.lry = lry;
        sc.enabled = (mode != 0);
    }

    // ── Display list dispatch ───────────────────────────────────────

    using DlHandler = void (*)(GbiState&, DisplayList*&);

    static void dl_noop(GbiState&, DisplayList*&) {}

    // Default handler for opcodes with no registered implementation. Unlike
    // dl_noop (used for genuine no-ops such as G_NOOP and the RDP sync
    // commands), this records the opcode and logs it once so a hardware
    // play-test surfaces missing GBI coverage instead of silently dropping
    // geometry or render state.
    static inline bool unimpl_logged[256]{};
    static void dl_unimplemented(GbiState&, DisplayList*& dl) {
        const uint8_t opcode = static_cast<uint8_t>(dl->w0 >> 24);
        if (!unimpl_logged[opcode]) {
            unimpl_logged[opcode] = true;
            fprintf(stderr,
                    "[DC GBI] Unimplemented opcode 0x%02X (w0=0x%08X w1=0x%08X)\n",
                    opcode, dl->w0, dl->w1);
        }
    }

    static void dl_enddl(GbiState& s, DisplayList*& dl) {
        if (!s.dl_stack.empty()) {
            dl = s.dl_stack.back();
            s.dl_stack.pop_back();
        } else {
            dl = nullptr;
        }
    }

    static void dl_run_dl(GbiState& s, DisplayList*& dl) {
        if (dl->p0(16, 1) == 0) {
            s.dl_stack.push_back(dl);
        }
        const uint32_t phys = s.from_segmented_masked(dl->w1);
        dl = reinterpret_cast<DisplayList*>(s.rdram + phys) - 1;
    }

    static void dl_rdp_half1(GbiState&, DisplayList*&) {}
    static void dl_rdp_half2(GbiState&, DisplayList*&) {}

    static void dl_mtx(GbiState& s, DisplayList*& dl) {
        s.matrix_op(dl->w1, static_cast<uint8_t>(dl->p0(0, 8) ^ G_MTX_PUSH));
    }

    static void dl_popmtx(GbiState& s, DisplayList*& dl) {
        s.pop_matrix(dl->w1 >> 6);
    }

    static void dl_moveword(GbiState& s, DisplayList*& dl) {
        const uint8_t type = static_cast<uint8_t>(dl->p0(16, 8));
        switch (type) {
        case G_MW_SEGMENT:
            s.segments[dl->p0(2, 4)] = dl->w1;
            break;
        case G_MW_NUMLIGHT:
            // F3DEX2 encodes the directional light count as NUML(n) = n * 24.
            s.num_dir_lights = std::clamp(static_cast<int>(dl->w1 / 24), 0, 7);
            s.lights_dirty = true;
            break;
        case G_MW_FORCEMTX:
            s.mvp_dirty = (dl->w1 == 0);
            break;
        case G_MW_FOG:
            s.fog_factor = dl->w1;
            break;
        case G_MW_PERSPNORM:
            s.persp_norm = static_cast<uint16_t>(dl->w1);
            break;
        default:
            break;
        }
    }

    static constexpr uint8_t G_MV_VIEWPORT = 8;
    static constexpr uint8_t G_MV_LIGHT = 10;

    static void dl_movemem(GbiState& s, DisplayList*& dl) {
        const uint8_t index = static_cast<uint8_t>(dl->p0(0, 8));
        if (index == G_MV_VIEWPORT) {
            s.set_viewport(dl->w1);
        } else if (index == G_MV_LIGHT) {
            // Offset (bytes) is encoded as ofs/8 in bits 8-15. The light DMEM
            // table is LOOKATX(0), LOOKATY(24), L0(48), L1(72)...; directional
            // light n (and the trailing ambient) map to slot ofs/24 - 2.
            const uint32_t ofs = dl->p0(8, 8) * 8u;
            const int slot = static_cast<int>(ofs / 24u) - 2;
            if (slot >= 0 && slot < static_cast<int>(s.raw_lights.size())) {
                const uint32_t addr = s.from_segmented(dl->w1) & 0x00FFFFFF;
                GbiState::RawLight& l = s.raw_lights[slot];
                // Light_t: col[3] at bytes 0-2, dir[3] (signed) at bytes 8-10.
                // RDRAM byte access is XOR-3 swizzled (big-endian word storage).
                l.col[0] = s.rdram[(addr + 0u) ^ 3u];
                l.col[1] = s.rdram[(addr + 1u) ^ 3u];
                l.col[2] = s.rdram[(addr + 2u) ^ 3u];
                l.dir[0] = static_cast<int8_t>(s.rdram[(addr + 8u) ^ 3u]);
                l.dir[1] = static_cast<int8_t>(s.rdram[(addr + 9u) ^ 3u]);
                l.dir[2] = static_cast<int8_t>(s.rdram[(addr + 10u) ^ 3u]);
                s.lights_dirty = true;
            }
        }
    }

    static void dl_geometrymode(GbiState& s, DisplayList*& dl) {
        const uint32_t off_mask = dl->p0(0, 24);
        const uint32_t on_mask = dl->w1;
        s.geometry_mode &= off_mask;
        s.geometry_mode |= on_mask;
    }

    static void dl_texture(GbiState& s, DisplayList*& dl) {
        s.texture_scale_s = static_cast<uint16_t>(dl->p1(16, 16));
        s.texture_scale_t = static_cast<uint16_t>(dl->p1(0, 16));
        s.texture_on = dl->p0(1, 7) != 0;
    }

    static void dl_vtx(GbiState& s, DisplayList*& dl) {
        const uint8_t count = static_cast<uint8_t>(dl->p0(12, 8));
        const uint8_t index = static_cast<uint8_t>(dl->p0(1, 7) - count);
        s.load_vertices(dl->w1, count, index);
    }

    static void dl_modifyvtx(GbiState& s, DisplayList*& dl) {
        const uint8_t index = static_cast<uint8_t>(dl->p0(0, 16));
        const uint8_t where = static_cast<uint8_t>(dl->p0(16, 8));
        s.apply_modify_vtx(index, where, dl->w1);
    }

    static void dl_culldl(GbiState& s, DisplayList*& dl) {
        const uint8_t vfirst = static_cast<uint8_t>(dl->p0(0, 16));
        const uint8_t vlast = static_cast<uint8_t>(dl->p1(0, 16));
        if (s.should_cull_dl(vfirst, vlast)) {
            dl = nullptr;
        }
    }

    static void dl_tri1(GbiState& s, DisplayList*& dl) {
        s.draw_tri(
            static_cast<uint8_t>(dl->p0(17, 7)),
            static_cast<uint8_t>(dl->p0(9, 7)),
            static_cast<uint8_t>(dl->p0(1, 7))
        );
    }

    static void dl_tri2(GbiState& s, DisplayList*& dl) {
        dl_tri1(s, dl);
        s.draw_tri(
            static_cast<uint8_t>(dl->p1(17, 7)),
            static_cast<uint8_t>(dl->p1(9, 7)),
            static_cast<uint8_t>(dl->p1(1, 7))
        );
    }

    static void dl_quad(GbiState& s, DisplayList*& dl) {
        dl_tri2(s, dl);
    }

    static void dl_special1(GbiState& s, DisplayList*& dl) {
        if (dl->p0(0, 8) == 1) {
            s.recompute_mvp();
        }
    }

    static void dl_spnoop(GbiState& s, DisplayList*& dl) {
        const uint32_t magic = dl->p0(0, 24);
        if (magic != RT64_HOOK_MAGIC) {
            return;
        }
        const uint32_t hook_op = dl->p1(28, 4);
        const uint32_t hook_val = dl->p1(0, 28);
        switch (hook_op) {
        case RT64_HOOK_OP_ENABLE:
            s.extended_opcode = static_cast<uint8_t>(hook_val & 0xFF);
            break;
        case RT64_HOOK_OP_DISABLE:
            s.extended_opcode = 0;
            break;
        case RT64_HOOK_OP_DL:
        case RT64_HOOK_OP_BRANCH:
            s.dl_stack.push_back(dl);
            {
                const uint32_t phys = s.from_segmented_masked(hook_val);
                dl = reinterpret_cast<DisplayList*>(s.rdram + phys) - 1;
            }
            break;
        default:
            break;
        }
    }

    static void dl_setothermode_h(GbiState& s, DisplayList*& dl) {
        const uint32_t size = dl->p0(0, 8) + 1;
        const uint32_t off = static_cast<uint32_t>(
            std::max<int32_t>(0, static_cast<int32_t>(32 - dl->p0(8, 8) - size)));
        const uint32_t mask = ((1u << size) - 1u) << off;
        s.other_mode_h = (s.other_mode_h & ~mask) | ((dl->w1 << off) & mask);
    }

    static void dl_setothermode_l(GbiState& s, DisplayList*& dl) {
        const uint32_t size = dl->p0(0, 8) + 1;
        const uint32_t off = static_cast<uint32_t>(
            std::max<int32_t>(0, static_cast<int32_t>(32 - dl->p0(8, 8) - size)));
        const uint32_t mask = ((1u << size) - 1u) << off;
        s.other_mode_l = (s.other_mode_l & ~mask) | ((dl->w1 << off) & mask);
    }

    static void dl_rdp_setothermode(GbiState& s, DisplayList*& dl) {
        s.other_mode_h = dl->p0(0, 24);
        s.other_mode_l = dl->w1;
    }

    static void dl_setcimg(GbiState& s, DisplayList*& dl) {
        s.set_color_image(
            static_cast<uint8_t>(dl->p0(21, 3)),
            static_cast<uint8_t>(dl->p0(19, 2)),
            static_cast<uint16_t>(dl->p0(0, 12) + 1),
            dl->w1
        );
    }

    static void dl_setzimg(GbiState& s, DisplayList*& dl) {
        s.z_image.address = s.from_segmented(dl->w1) & 0x00FFFFFF;
        s.z_image.width = s.color_image.width;
    }

    static void dl_setprimcolor(GbiState& s, DisplayList*& dl) {
        s.prim_color = dl->w1;
    }

    static void dl_setfillcolor(GbiState& s, DisplayList*& dl) {
        s.fill_color = dl->w1;
    }

    static void dl_setenvcolor(GbiState& s, DisplayList*& dl) {
        s.env_color = dl->w1;
    }

    static void dl_setblendcolor(GbiState& s, DisplayList*& dl) {
        s.blend_color = dl->w1;
    }

    static void dl_setfogcolor(GbiState& s, DisplayList*& dl) {
        s.fog_color = dl->w1;
    }

    static void dl_load_ucode(GbiState& s, DisplayList*& dl) {
        const DisplayList* next = dl + 1;
        if ((next->w0 >> 24) == 0 && next->w1 != 0) {
            s.s2dex_active = false;
            dl++;
        } else {
            s.s2dex_active = true;
        }
    }

    static void push_state(uint32_t* stack, int& size, uint32_t value) {
        if (size < STATE_STACK_SIZE) {
            stack[size++] = value;
        }
    }

    static bool pop_state(uint32_t* stack, int& size, uint32_t& value) {
        if (size <= 0) {
            return false;
        }
        value = stack[--size];
        return true;
    }

    static void push_u64(uint64_t* stack, int& size, uint64_t value) {
        if (size < STATE_STACK_SIZE) {
            stack[size++] = value;
        }
    }

    static bool pop_u64(uint64_t* stack, int& size, uint64_t& value) {
        if (size <= 0) {
            return false;
        }
        value = stack[--size];
        return true;
    }

    static void push_matrix_group(GbiState& s, bool projection) {
        if (projection) {
            if (s.proj_stack_size < MATRIX_STACK_SIZE) {
                memcpy(s.proj_matrix_stack[s.proj_stack_size++], s.proj_matrix, sizeof(s.proj_matrix));
            }
        } else if (s.model_stack_size < MATRIX_STACK_SIZE) {
            memcpy(s.model_stack[s.model_stack_size], s.model_matrix, sizeof(s.model_matrix));
            s.model_stack_size++;
        }
    }

    static void pop_matrix_group(GbiState& s, bool projection, uint8_t count) {
        for (uint8_t i = 0; i < count; i++) {
            if (projection && s.proj_stack_size > 1) {
                s.proj_stack_size--;
                memcpy(s.proj_matrix, s.proj_matrix_stack[s.proj_stack_size], sizeof(s.proj_matrix));
                s.mvp_dirty = true;
            } else if (!projection && s.model_stack_size > 1) {
                s.model_stack_size--;
                memcpy(s.model_matrix, s.model_stack[s.model_stack_size - 1], sizeof(s.model_matrix));
                s.mvp_dirty = true;
            }
        }
    }

    static void draw_s2dex_bg_copy(GbiState& s, uint32_t address) {
        const uint32_t phys = s.from_segmented_masked(address);

        const uint16_t image_w = s.rdram_be16(phys + 2);
        const int16_t frame_x = static_cast<int16_t>(s.rdram_be16(phys + 4));
        const uint16_t frame_w = s.rdram_be16(phys + 6);
        const uint16_t image_h = s.rdram_be16(phys + 10);
        const int16_t frame_y = static_cast<int16_t>(s.rdram_be16(phys + 12));
        const uint16_t frame_h = s.rdram_be16(phys + 14);
        const uint32_t image_ptr = s.rdram_be32(phys + 16);
        const uint8_t image_fmt = s.rdram_u8(phys + 22);
        const uint8_t image_siz = s.rdram_u8(phys + 23);

        s.texture_to_load.offset = image_ptr & 0x00FFFFFF;
        s.texture_to_load.siz = image_siz;
        s.texture_to_load.width = std::max<uint16_t>(image_w, 1);

        s.render_tile.fmt = image_fmt;
        s.render_tile.siz = image_siz;
        s.render_tile.uls = 0;
        s.render_tile.ult = 0;
        s.render_tile.lrs = static_cast<uint16_t>((image_w - 1) * 4);
        s.render_tile.lrt = static_cast<uint16_t>((image_h - 1) * 4);
        s.render_tile.line_size_bytes = static_cast<uint16_t>(image_w * (1u << image_siz));

        const uint32_t size_bytes = static_cast<uint32_t>(image_w) * image_h * (1u << std::min<uint32_t>(image_siz, 2u));
        s.tmem.load_block(s.rdram, s.texture_to_load.offset, 0, size_bytes);
        s.loaded_textures[0].addr = s.tmem.data();
        s.loaded_textures[0].size_bytes = size_bytes;
        s.loaded_textures[0].valid = true;
        s.texture_changed = true;

        const int32_t ulx = static_cast<int32_t>(frame_x) * 4;
        const int32_t uly = static_cast<int32_t>(frame_y) * 4;
        const int32_t lrx = ulx + static_cast<int32_t>(frame_w) * 4;
        const int32_t lry = uly + static_cast<int32_t>(frame_h) * 4;
        s.draw_tex_rect(ulx, uly, lrx, lry, 0, 0, 0x100, 0x100, false);
    }

    static void draw_s2dex_obj_rectangle(GbiState& s, uint32_t address) {
        const uint32_t phys = s.from_segmented_masked(address);

        const uint16_t imageW = s.rdram_be16(phys + 2);
        const int16_t objX = static_cast<int16_t>(s.rdram_be16(phys + 4));
        const uint16_t scaleW = s.rdram_be16(phys + 6);
        const uint16_t imageH = s.rdram_be16(phys + 10);
        const int16_t objY = static_cast<int16_t>(s.rdram_be16(phys + 12));
        const uint16_t scaleH = s.rdram_be16(phys + 14);
        const uint32_t image_ptr = s.rdram_be32(phys + 16);
        const uint8_t image_fmt = s.rdram_u8(phys + 22);
        const uint8_t image_siz = s.rdram_u8(phys + 23);

        s.texture_to_load.offset = image_ptr & 0x00FFFFFF;
        s.texture_to_load.siz = image_siz;
        s.texture_to_load.width = std::max<uint16_t>(imageW, 1);
        s.render_tile.fmt = image_fmt;
        s.render_tile.siz = image_siz;
        s.render_tile.uls = 0;
        s.render_tile.ult = 0;
        s.render_tile.lrs = static_cast<uint16_t>((imageW - 1) * 4);
        s.render_tile.lrt = static_cast<uint16_t>((imageH - 1) * 4);
        s.render_tile.line_size_bytes = static_cast<uint16_t>(imageW * (1u << image_siz));

        const uint32_t size_bytes = static_cast<uint32_t>(imageW) * imageH * (1u << std::min<uint32_t>(image_siz, 2u));
        s.tmem.load_block(s.rdram, s.texture_to_load.offset, 0, size_bytes);
        s.loaded_textures[0].addr = s.tmem.data();
        s.loaded_textures[0].size_bytes = size_bytes;
        s.loaded_textures[0].valid = true;

        const int32_t ulx = static_cast<int32_t>(objX) * 4;
        const int32_t uly = static_cast<int32_t>(objY) * 4;
        const int32_t lrx = ulx + static_cast<int32_t>(scaleW) * 4;
        const int32_t lry = uly + static_cast<int32_t>(scaleH) * 4;
        s.draw_tex_rect(ulx, uly, lrx, lry, 0, 0, 0x100, 0x100, false);
    }

    static void dl_s2dex(GbiState& s, DisplayList*& dl) {
        const uint8_t opcode = static_cast<uint8_t>(dl->w0 >> 24);
        switch (opcode) {
        case G_S2DEX_BG_RECT_COPY:
            draw_s2dex_bg_copy(s, dl->w1);
            break;
        case G_S2DEX_OBJ_RECTANGLE:
            draw_s2dex_obj_rectangle(s, dl->w1);
            break;
        default:
            break;
        }
    }

    static void dl_setcombine(GbiState& s, DisplayList*& dl) {
        s.combine_mode = (static_cast<uint64_t>(dl->w1) << 32) | dl->w0;
    }

    static void dl_settimg(GbiState& s, DisplayList*& dl) {
        const uint32_t address = s.from_segmented(dl->w1);
        s.texture_to_load.offset = address & 0x00FFFFFF;
        s.texture_to_load.siz = static_cast<uint8_t>(dl->p0(19, 2));
        s.texture_to_load.width = static_cast<uint16_t>(dl->p0(0, 12) + 1);
    }

    static void dl_settile(GbiState& s, DisplayList*& dl) {
        const uint8_t tile = static_cast<uint8_t>(dl->p1(24, 3));
        TileDescriptor* desc = nullptr;
        if (tile == G_TX_RENDERTILE) {
            desc = &s.render_tile;
        } else if (tile == 1) {
            desc = &s.tile1_desc;
            s.tile1_desc_valid = true;
        }
        if (desc != nullptr) {
            desc->fmt = static_cast<uint8_t>(dl->p0(21, 3));
            desc->siz = static_cast<uint8_t>(dl->p0(19, 2));
            desc->line_size_bytes = static_cast<uint16_t>(dl->p0(9, 9) * 8);
            desc->cms = static_cast<uint8_t>(dl->p1(8, 2));
            desc->cmt = static_cast<uint8_t>(dl->p1(18, 2));
            s.texture_changed = true;
        }
        if (tile == G_TX_LOADTILE) {
            s.load_tile_slot = static_cast<uint8_t>(dl->p0(0, 9) / 256);
        }
    }

    static void dl_settilesize(GbiState& s, DisplayList*& dl) {
        const uint8_t tile = static_cast<uint8_t>(dl->p1(24, 3));
        TileDescriptor* desc = nullptr;
        if (tile == G_TX_RENDERTILE) {
            desc = &s.render_tile;
        } else if (tile == 1) {
            desc = &s.tile1_desc;
            s.tile1_desc_valid = true;
        }
        if (desc != nullptr) {
            desc->uls = static_cast<uint16_t>(dl->p0(12, 12));
            desc->ult = static_cast<uint16_t>(dl->p0(0, 12));
            desc->lrs = static_cast<uint16_t>(dl->p1(12, 12));
            desc->lrt = static_cast<uint16_t>(dl->p1(0, 12));
            s.texture_changed = true;
        }
    }

    static void dl_loadblock(GbiState& s, DisplayList*& dl) {
        const uint8_t tile = static_cast<uint8_t>(dl->p1(24, 3));
        if (tile != G_TX_LOADTILE) {
            return;
        }

        const uint16_t lrs = static_cast<uint16_t>(dl->p1(12, 12));
        uint32_t word_size_shift = 0;
        switch (s.texture_to_load.siz) {
        case G_IM_SIZ_4b:
        case G_IM_SIZ_8b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_16b:
            word_size_shift = 1;
            break;
        case G_IM_SIZ_32b:
            word_size_shift = 2;
            break;
        default:
            break;
        }

        const uint32_t size_bytes = (static_cast<uint32_t>(lrs) + 1u) << word_size_shift;
        const uint8_t slot = std::min<uint8_t>(s.load_tile_slot, 1);
        const uint32_t dest_offset = s.tmem_offset % tmem::TMEM_SIZE;
        s.tmem.load_block(s.rdram, s.texture_to_load.offset, dest_offset, size_bytes);
        s.loaded_textures[slot].addr = s.tmem.data() + dest_offset;
        s.loaded_textures[slot].size_bytes = size_bytes;
        s.loaded_textures[slot].valid = true;
        s.tmem_offset = (dest_offset + size_bytes + 7u) & ~7u;
        s.texture_changed = true;
    }

    static void dl_loadtile(GbiState& s, DisplayList*& dl) {
        const uint8_t tile = static_cast<uint8_t>(dl->p1(24, 3));
        if (tile != G_TX_LOADTILE) {
            return;
        }

        const uint16_t uls = static_cast<uint16_t>(dl->p0(12, 12));
        const uint16_t ult = static_cast<uint16_t>(dl->p0(0, 12));
        const uint16_t lrs = static_cast<uint16_t>(dl->p1(12, 12));
        const uint16_t lrt = static_cast<uint16_t>(dl->p1(0, 12));

        uint32_t word_size_shift = 0;
        switch (s.texture_to_load.siz) {
        case G_IM_SIZ_4b:
        case G_IM_SIZ_8b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_16b:
            word_size_shift = 1;
            break;
        case G_IM_SIZ_32b:
            word_size_shift = 2;
            break;
        default:
            break;
        }

        const uint32_t width_tiles = (static_cast<uint32_t>(lrs) >> G_TEXTURE_IMAGE_FRAC) + 1u;
        const uint32_t height_tiles = (static_cast<uint32_t>(lrt) >> G_TEXTURE_IMAGE_FRAC) + 1u;
        const uint32_t width_bytes = width_tiles << word_size_shift;
        const uint32_t size_bytes = width_bytes * height_tiles;

        const uint8_t slot = std::min<uint8_t>(s.load_tile_slot, 1);
        const uint32_t dest_offset = s.tmem_offset % tmem::TMEM_SIZE;
        const uint32_t src_stride = std::max<uint32_t>(s.texture_to_load.width, 1u) << word_size_shift;
        s.tmem.load_tile(s.rdram, s.texture_to_load.offset, src_stride, dest_offset, width_bytes, height_tiles);
        s.loaded_textures[slot].addr = s.tmem.data() + dest_offset;
        s.loaded_textures[slot].size_bytes = size_bytes;
        s.loaded_textures[slot].valid = true;
        s.tmem_offset = (dest_offset + size_bytes + 7u) & ~7u;

        TileDescriptor& desc = (slot == 0) ? s.render_tile : s.tile1_desc;
        if (slot == 1) {
            s.tile1_desc_valid = true;
        }
        desc.uls = uls;
        desc.ult = ult;
        desc.lrs = lrs;
        desc.lrt = lrt;
        s.texture_changed = true;
    }

    static void dl_loadtlut(GbiState& s, DisplayList*& dl) {
        const uint8_t tile = static_cast<uint8_t>(dl->p1(24, 3));
        if (tile == G_TX_LOADTILE && s.texture_to_load.siz == G_IM_SIZ_16b) {
            // count is encoded as (entries-1) in w1 bits 14-23; each TLUT entry
            // is a 16-bit color. De-swizzle into palette_buf so the cache reads
            // logical big-endian palette colors.
            const uint32_t entries = std::min<uint32_t>(dl->p1(14, 10) + 1u, 256u);
            const uint32_t bytes = entries * 2u;
            for (uint32_t j = 0; j < bytes; j++) {
                s.palette_buf[j] = s.rdram[(s.texture_to_load.offset + j) ^ 3u];
            }
            s.palette = s.palette_buf.data();
        }
    }

    static void dl_fillrect(GbiState& s, DisplayList*& dl) {
        s.fill_rect(
            static_cast<int32_t>(dl->p1(12, 12)),
            static_cast<int32_t>(dl->p1(0, 12)),
            static_cast<int32_t>(dl->p0(12, 12)),
            static_cast<int32_t>(dl->p0(0, 12))
        );
    }

    static void dl_setscissor(GbiState& s, DisplayList*& dl) {
        s.set_scissor(
            static_cast<uint8_t>(dl->p1(24, 2)),
            static_cast<int32_t>(dl->p0(12, 12)),
            static_cast<int32_t>(dl->p0(0, 12)),
            static_cast<int32_t>(dl->p1(12, 12)),
            static_cast<int32_t>(dl->p1(0, 12))
        );
    }

    static void dl_texrect(GbiState& s, DisplayList*& dl) {
        const int32_t ulx = static_cast<int32_t>(dl->p1(12, 12));
        const int32_t uly = static_cast<int32_t>(dl->p1(0, 12));
        const int32_t lrx = static_cast<int32_t>(dl->p0(12, 12));
        const int32_t lry = static_cast<int32_t>(dl->p0(0, 12));
        dl++;

        const int16_t uls = static_cast<int16_t>(dl->p1(16, 16));
        const int16_t ult = static_cast<int16_t>(dl->p1(0, 16));
        dl++;

        const int16_t dsdx = static_cast<int16_t>(dl->p1(16, 16));
        const int16_t dtdy = static_cast<int16_t>(dl->p1(0, 16));
        const bool flip = static_cast<uint8_t>(dl->w0 >> 24) == G_TEXRECTFLIP;

        s.draw_tex_rect(ulx, uly, lrx, lry, uls, ult, dsdx, dtdy, flip);
    }

    static void dl_extended(GbiState& s, DisplayList*& dl) {
        const uint32_t sub_op = dl->p0(0, 24);
        switch (sub_op) {
        case G_EX_NOOP:
            break;
        case G_EX_FILLRECT_V1: {
            const int32_t left_origin = static_cast<int32_t>(dl->p1(0, 12));
            const int32_t right_origin = static_cast<int32_t>(dl->p1(12, 12));
            dl++;
            const int32_t ulx = static_cast<int16_t>(dl->p0(16, 16));
            const int32_t uly = static_cast<int16_t>(dl->p0(0, 16));
            const int32_t lrx = static_cast<int16_t>(dl->p1(16, 16));
            const int32_t lry = static_cast<int16_t>(dl->p1(0, 16));
            s.fill_rect(ulx, uly, lrx, lry, left_origin, right_origin);
            break;
        }
        case G_EX_TEXRECT_V1: {
            const int32_t left_origin = static_cast<int32_t>(dl->p1(3, 12));
            const int32_t right_origin = static_cast<int32_t>(dl->p1(15, 12));
            dl++;
            const int32_t ulx = static_cast<int16_t>(dl->p0(16, 16));
            const int32_t uly = static_cast<int16_t>(dl->p0(0, 16));
            const int32_t lrx = static_cast<int16_t>(dl->p1(16, 16));
            const int32_t lry = static_cast<int16_t>(dl->p1(0, 16));
            dl++;
            const int16_t uls = static_cast<int16_t>(dl->p0(16, 16));
            const int16_t ult = static_cast<int16_t>(dl->p0(0, 16));
            const int16_t dsdx = static_cast<int16_t>(dl->p1(16, 16));
            const int16_t dtdy = static_cast<int16_t>(dl->p1(0, 16));
            s.draw_tex_rect(ulx, uly, lrx, lry, uls, ult, dsdx, dtdy, false, left_origin, right_origin);
            break;
        }
        case G_EX_SETVIEWPORT_V1:
            dl++;
            s.set_viewport(dl->w1);
            break;
        case G_EX_SETSCISSOR_V1: {
            const uint8_t mode = static_cast<uint8_t>(dl->p1(0, 2));
            dl++;
            const int32_t ulx = static_cast<int16_t>(dl->p0(16, 16));
            const int32_t uly = static_cast<int16_t>(dl->p0(0, 16));
            const int32_t lrx = static_cast<int16_t>(dl->p1(16, 16));
            const int32_t lry = static_cast<int16_t>(dl->p1(0, 16));
            s.set_scissor(mode, ulx, uly, lrx, lry);
            break;
        }
        case G_EX_SETRECTALIGN_V1:
            s.rect_align.left_origin = static_cast<int16_t>(dl->p1(0, 12));
            s.rect_align.right_origin = static_cast<int16_t>(dl->p1(12, 12));
            dl++;
            s.rect_align.left_offset = static_cast<int16_t>(dl->p0(16, 16));
            s.rect_align.top_offset = static_cast<int16_t>(dl->p0(0, 16));
            s.rect_align.right_offset = static_cast<int16_t>(dl->p1(16, 16));
            s.rect_align.bottom_offset = static_cast<int16_t>(dl->p1(0, 16));
            break;
        case G_EX_SETVIEWPORTALIGN_V1:
            dl++;
            s.viewport_align.origin = static_cast<int16_t>(dl->p0(0, 12));
            s.viewport_align.x_offset = static_cast<int16_t>(dl->p1(16, 16));
            s.viewport_align.y_offset = static_cast<int16_t>(dl->p1(0, 16));
            break;
        case G_EX_SETSCISSORALIGN_V1:
            s.scissor_align.left_origin = static_cast<int16_t>(dl->p1(0, 12));
            s.scissor_align.right_origin = static_cast<int16_t>(dl->p1(12, 12));
            dl++;
            s.scissor_align.ulx_offset = static_cast<int16_t>(dl->p0(16, 16));
            s.scissor_align.uly_offset = static_cast<int16_t>(dl->p0(0, 16));
            s.scissor_align.lrx_offset = static_cast<int16_t>(dl->p1(16, 16));
            s.scissor_align.lry_offset = static_cast<int16_t>(dl->p1(0, 16));
            dl++;
            s.scissor_align.ulx_bound = static_cast<int16_t>(dl->p0(16, 16));
            s.scissor_align.uly_bound = static_cast<int16_t>(dl->p0(0, 16));
            s.scissor_align.lrx_bound = static_cast<int16_t>(dl->p1(16, 16));
            s.scissor_align.lry_bound = static_cast<int16_t>(dl->p1(0, 16));
            break;
        case G_EX_VERTEXZTEST_V1: {
            const uint8_t vtx_index = static_cast<uint8_t>(dl->p1(0, 8));
            s.vertex_ztest_skip = false;
            if (!s.vtx_loaded[vtx_index]) {
                s.transform_vertex(vtx_index);
            }
            const TransformedVertex& test_vert = s.xf_buffer[vtx_index];
            if (s.renderer != nullptr
                && s.renderer->is_occluded(test_vert.screen_x, test_vert.screen_y, test_vert.depth)) {
                s.vertex_ztest_skip = true;
            }
            break;
        }
        case G_EX_ENDVERTEXZTEST_V1:
            s.vertex_ztest_skip = false;
            break;
        case G_EX_MATRIXGROUP_V1: {
            dl++;
            const uint32_t flags = dl->w0;
            const bool push = (flags & 0x1u) != 0;
            const bool projection = (flags & 0x2u) != 0;
            if (push) {
                push_matrix_group(s, projection);
            }
            break;
        }
        case G_EX_POPMATRIXGROUP_V1: {
            const uint8_t count = static_cast<uint8_t>(dl->p1(0, 8));
            const bool projection = dl->p1(8, 1) != 0;
            pop_matrix_group(s, projection, std::max<uint8_t>(count, 1));
            break;
        }
        case G_EX_SETREFRESHRATE_V1:
        case G_EX_SETRDRAMEXTENDED_V1:
            break;
        case G_EX_FORCEBRANCH_V1:
            s.force_branch = dl->p1(0, 1) != 0;
            break;
        case G_EX_PUSHVIEWPORT_V1:
            if (s.viewport_stack_size < static_cast<int>(VIEWPORT_STACK_SIZE)) {
                s.viewport_stack[s.viewport_stack_size++] = s.viewport;
            }
            break;
        case G_EX_POPVIEWPORT_V1:
            if (s.viewport_stack_size > 1) {
                s.viewport = s.viewport_stack[--s.viewport_stack_size];
            }
            break;
        case G_EX_PUSHOTHERMODE_V1:
            push_state(s.other_mode_h_stack.data(), s.other_mode_h_stack_size, s.other_mode_h);
            push_state(s.other_mode_l_stack.data(), s.other_mode_l_stack_size, s.other_mode_l);
            break;
        case G_EX_POPOTHERMODE_V1:
            pop_state(s.other_mode_l_stack.data(), s.other_mode_l_stack_size, s.other_mode_l);
            pop_state(s.other_mode_h_stack.data(), s.other_mode_h_stack_size, s.other_mode_h);
            break;
        case G_EX_PUSHCOMBINE_V1:
            push_u64(s.combine_stack.data(), s.combine_stack_size, s.combine_mode);
            break;
        case G_EX_POPCOMBINE_V1:
            pop_u64(s.combine_stack.data(), s.combine_stack_size, s.combine_mode);
            break;
        case G_EX_PUSHPROJMATRIX_V1:
            if (s.proj_stack_size < MATRIX_STACK_SIZE) {
                memcpy(s.proj_matrix_stack[s.proj_stack_size++], s.proj_matrix, sizeof(s.proj_matrix));
            }
            break;
        case G_EX_POPPROJMATRIX_V1:
            if (s.proj_stack_size > 1) {
                memcpy(s.proj_matrix, s.proj_matrix_stack[--s.proj_stack_size], sizeof(s.proj_matrix));
                s.mvp_dirty = true;
            }
            break;
        case G_EX_PUSHGEOMETRYMODE_V1:
            push_state(s.geometry_mode_stack.data(), s.geometry_mode_stack_size, s.geometry_mode);
            break;
        case G_EX_POPGEOMETRYMODE_V1:
            pop_state(s.geometry_mode_stack.data(), s.geometry_mode_stack_size, s.geometry_mode);
            break;
        case G_EX_PUSHPRIMCOLOR_V1:
            push_state(s.prim_color_stack.data(), s.prim_color_stack_size, s.prim_color);
            break;
        case G_EX_POPPRIMCOLOR_V1:
            pop_state(s.prim_color_stack.data(), s.prim_color_stack_size, s.prim_color);
            break;
        case G_EX_PUSHENVCOLOR_V1:
            push_state(s.env_color_stack.data(), s.env_color_stack_size, s.env_color);
            break;
        case G_EX_POPENVCOLOR_V1:
            pop_state(s.env_color_stack.data(), s.env_color_stack_size, s.env_color);
            break;
        case G_EX_PUSHSCISSOR_V1:
            if (s.scissor_stack_size < 15) {
                s.scissor_stack[s.scissor_stack_size] = s.scissor_stack[s.scissor_stack_size - 1];
                s.scissor_stack_size++;
            }
            break;
        case G_EX_POPSCISSOR_V1:
            if (s.scissor_stack_size > 1) {
                s.scissor_stack_size--;
            }
            break;
        case G_EX_VERTEX_V1: {
            const uint8_t count = static_cast<uint8_t>(dl->p1(8, 8));
            const uint8_t index = static_cast<uint8_t>(dl->p1(0, 8));
            dl++;
            s.load_vertices(dl->w1, count, index);
            break;
        }
        default:
            break;
        }
    }

    static inline DlHandler gbi_dispatch[256]{};

    static void init_dispatch() {
        static bool initialized = false;
        if (initialized) {
            return;
        }
        initialized = true;

        for (auto& handler : gbi_dispatch) {
            handler = dl_unimplemented;
        }

        gbi_dispatch[G_NOOP] = dl_noop;
        gbi_dispatch[G_VTX] = dl_vtx;
        gbi_dispatch[G_MODIFYVTX] = dl_modifyvtx;
        gbi_dispatch[G_CULLDL] = dl_culldl;
        gbi_dispatch[G_TRI1] = dl_tri1;
        gbi_dispatch[G_TRI2] = dl_tri2;
        gbi_dispatch[G_QUAD] = dl_quad;
        gbi_dispatch[G_DL] = dl_run_dl;
        gbi_dispatch[G_ENDDL] = dl_enddl;
        gbi_dispatch[G_MTX] = dl_mtx;
        gbi_dispatch[G_POPMTX] = dl_popmtx;
        gbi_dispatch[G_MOVEWORD] = dl_moveword;
        gbi_dispatch[G_DMA_IO] = dl_moveword;
        gbi_dispatch[G_MOVEMEM] = dl_movemem;
        gbi_dispatch[G_GEOMETRYMODE] = dl_geometrymode;
        gbi_dispatch[G_TEXTURE] = dl_texture;
        gbi_dispatch[G_SPECIAL_1] = dl_special1;
        gbi_dispatch[G_SPNOOP] = dl_spnoop;
        gbi_dispatch[G_RDPHALF_1] = dl_rdp_half1;
        gbi_dispatch[G_RDPHALF_2] = dl_rdp_half2;
        gbi_dispatch[G_SETOTHERMODE_H] = dl_setothermode_h;
        gbi_dispatch[G_SETOTHERMODE_L] = dl_setothermode_l;
        gbi_dispatch[G_RDPSETOTHERMODE] = dl_rdp_setothermode;

        gbi_dispatch[G_SETCIMG] = dl_setcimg;
        gbi_dispatch[G_SETPRIMCOLOR] = dl_setprimcolor;
        gbi_dispatch[G_SETFILLCOLOR] = dl_setfillcolor;
        gbi_dispatch[G_SETENVCOLOR] = dl_setenvcolor;
        gbi_dispatch[G_SETBLENDCOLOR] = dl_setblendcolor;
        gbi_dispatch[G_SETFOGCOLOR] = dl_setfogcolor;
        gbi_dispatch[G_LOAD_UCODE] = dl_load_ucode;
        gbi_dispatch[G_FILLRECT] = dl_fillrect;
        gbi_dispatch[G_SETSCISSOR] = dl_setscissor;
        gbi_dispatch[G_TEXRECT] = dl_texrect;
        gbi_dispatch[G_TEXRECTFLIP] = dl_texrect;
        gbi_dispatch[G_RDPNOOP] = dl_noop;
        gbi_dispatch[G_RDPLOADSYNC] = dl_noop;
        gbi_dispatch[G_RDPPIPESYNC] = dl_noop;
        gbi_dispatch[G_RDPTILESYNC] = dl_noop;
        gbi_dispatch[G_RDPFULLSYNC] = dl_noop;
        gbi_dispatch[G_SETZIMG] = dl_setzimg;
        gbi_dispatch[G_SETTIMG] = dl_settimg;
        gbi_dispatch[G_SETCOMBINE] = dl_setcombine;
        gbi_dispatch[G_SETTILE] = dl_settile;
        gbi_dispatch[G_LOADBLOCK] = dl_loadblock;
        gbi_dispatch[G_LOADTILE] = dl_loadtile;
        gbi_dispatch[G_SETTILESIZE] = dl_settilesize;
        gbi_dispatch[G_LOADTLUT] = dl_loadtlut;
    }

    static void run_display_list(GbiState& state, DisplayList* dl) {
        init_dispatch();
        state.dl_stack.clear();

        uint32_t rdp_half1 = 0;

        while (dl != nullptr) {
            uint8_t opcode = static_cast<uint8_t>(dl->w0 >> 24);

            if (opcode == G_RDPHALF_1) {
                rdp_half1 = dl->w1;
                dl++;
                continue;
            }

            if (opcode == G_BRANCH_W) {
                if (state.force_branch) {
                    const uint32_t phys = state.from_segmented_masked(rdp_half1);
                    dl = reinterpret_cast<DisplayList*>(state.rdram + phys) - 1;
                }
                dl++;
                continue;
            }

            if (state.extended_opcode != 0 && opcode == state.extended_opcode) {
                dl_extended(state, dl);
            } else if (state.s2dex_active
                && (opcode == G_S2DEX_BG_RECT_COPY || opcode == G_S2DEX_OBJ_RECTANGLE)) {
                dl_s2dex(state, dl);
            } else {
                gbi_dispatch[opcode](state, dl);
            }

            if (dl != nullptr) {
                dl++;
            }
        }
    }
}; // struct GbiState

} // anonymous namespace

namespace dreamcast::gbi {

struct Interpreter::Impl {
    GbiState state;
};

Interpreter::Interpreter() {
    reset();
}

Interpreter::~Interpreter() {
    delete impl_;
    impl_ = nullptr;
}

void Interpreter::reset() {
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }

    pvr::Renderer* renderer = impl_->state.renderer;
    tex::Cache* texture_cache = impl_->state.texture_cache;
    GbiState& s = impl_->state;
    s = GbiState{};
    s.renderer = renderer;
    s.texture_cache = texture_cache;
    mat4_identity(s.model_matrix);
    mat4_identity(s.proj_matrix);
    mat4_identity(s.model_stack[0]);
    mat4_identity(s.proj_matrix_stack[0]);
    s.viewport_stack[0] = s.viewport;
    s.mvp_dirty = true;
    s.geometry_mode = G_CULL_BACK;
    s.other_mode_h = 0x080CFF;
    s.force_branch = true;

    // Default lights to full-bright white so geometry that enables G_LIGHTING
    // before any light is uploaded renders visibly rather than black.
    for (auto& light : s.raw_lights) {
        light.col[0] = light.col[1] = light.col[2] = 255;
        light.dir[0] = light.dir[1] = light.dir[2] = 0;
    }
    s.num_dir_lights = 0;
    s.lights_dirty = true;
}

void Interpreter::set_renderer(pvr::Renderer* renderer) {
    if (impl_ == nullptr) {
        reset();
    }
    impl_->state.renderer = renderer;
}

void Interpreter::set_texture_cache(tex::Cache* cache) {
    if (impl_ == nullptr) {
        reset();
    }
    impl_->state.texture_cache = cache;
}

void Interpreter::process_display_list(uint8_t* rdram, const OSTask* task) {
    if (impl_ == nullptr) {
        reset();
    }

    if (rdram == nullptr || task == nullptr) {
        return;
    }

    impl_->state.rdram = rdram;
    const uint32_t dl_addr = task->t.data_ptr & 0x00FFFFFF;
    if (dl_addr == 0) {
        return;
    }

    DisplayList* dl = reinterpret_cast<DisplayList*>(rdram + dl_addr);
    GbiState::run_display_list(impl_->state, dl);
}

} // namespace dreamcast::gbi

#endif // DREAMCAST
