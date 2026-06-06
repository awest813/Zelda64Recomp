// Dreamcast F3DZEX2 / RDP display list high-level emulator.
//
// Gfx commands update RSP state; geometry is submitted directly to the PVR
// tile accelerator via dc_pvr_renderer (SM64 DC port architecture).

#ifdef DREAMCAST

#include "dc_gbi.h"
#include "dc_pvr_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

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

constexpr uint32_t G_SHADE = 0x00000004;
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

constexpr uint32_t G_MDSFT_CYCLETYPE = 20;
constexpr uint32_t G_CYC_FILL = 3u << G_MDSFT_CYCLETYPE;
constexpr uint32_t G_CYC_COPY = 2u << G_MDSFT_CYCLETYPE;

constexpr uint32_t G_IM_SIZ_16b = 2;

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
constexpr uint8_t G_EX_FORCEBRANCH_V1 = 0x11;
constexpr uint8_t G_EX_VERTEX_V1 = 0x14;
constexpr uint8_t G_EX_PUSHVIEWPORT_V1 = 0x15;
constexpr uint8_t G_EX_POPVIEWPORT_V1 = 0x16;
constexpr uint8_t G_EX_PUSHSCISSOR_V1 = 0x17;
constexpr uint8_t G_EX_POPSCISSOR_V1 = 0x18;

constexpr size_t MAX_VERTICES = 256;
constexpr size_t MAX_SEGMENTS = 16;
constexpr size_t MATRIX_STACK_SIZE = 32;
constexpr float DEPTH_RANGE = 1024.0f;

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
    float w = 1.0f;
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
    uint8_t a = 255;
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
    int32_t left_offset = 0;
    int32_t top_offset = 0;
    int32_t right_offset = 0;
    int32_t bottom_offset = 0;
};

// ── Matrix math (4x4) ───────────────────────────────────────────────

static void mat4_identity(float m[4][4]) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            m[i][j] = (i == j) ? 1.0f : 0.0f;
        }
    }
}

static void mat4_mul(const float a[4][4], const float b[4][4], float out[4][4]) {
    float temp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            temp[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(out, temp, sizeof(temp));
}

static void mat4_transform(const float m[4][4], float x, float y, float z, float& ox, float& oy, float& oz, float& ow) {
    ox = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
    oy = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
    oz = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
    ow = m[3][0] * x + m[3][1] * y + m[3][2] * z + m[3][3];
}

// ── Interpreter state ───────────────────────────────────────────────

struct GbiState {
    uint8_t* rdram = nullptr;
    pvr::Renderer* renderer = nullptr;

    std::array<uint32_t, MAX_SEGMENTS> segments{};
    float model_matrix[4][4]{};
    int model_stack_size = 1;
    float model_stack[MATRIX_STACK_SIZE][4][4]{};

    float proj_matrix[4][4]{};
    float mvp_matrix[4][4]{};
    bool mvp_dirty = true;

    Viewport viewport{};
    std::array<ScissorRect, 16> scissor_stack{};
    int scissor_stack_size = 1;
    RectAlign rect_align{};

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

    uint32_t prim_color = 0xFFFFFFFF;
    uint32_t fill_color = 0;
    uint32_t env_color = 0xFFFFFFFF;

    bool texture_on = false;
    bool force_branch = true;
    uint8_t extended_opcode = 0;

    std::vector<DisplayList*> dl_stack;

    uint32_t from_segmented(uint32_t seg_addr) const {
        return segments[(seg_addr >> 24) & 0x0F] + (seg_addr & 0x00FFFFFF);
    }

    uint32_t from_segmented_masked(uint32_t seg_addr) const {
        return from_segmented(seg_addr) & 0x00FFFFF8;
    }

    void recompute_mvp() {
        mat4_mul(model_matrix, proj_matrix, mvp_matrix);
        mvp_dirty = false;
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
        const float inv_w = 1.0f / tw;
        out.screen_x = (tx * inv_w) * viewport.scale[0] + viewport.translate[0];
        out.screen_y = (ty * -inv_w) * viewport.scale[1] + viewport.translate[1];
        const float ndc_z = tz * inv_w;
        out.depth = std::clamp((ndc_z + 1.0f) * 0.5f, 0.0f, 1.0f);
        out.r = v.r;
        out.g = v.g;
        out.b = v.b;
        out.a = v.a;
        vtx_loaded[index] = 1;
    }

    static uint32_t pack_argb(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        return (static_cast<uint32_t>(a) << 24)
             | (static_cast<uint32_t>(r) << 16)
             | (static_cast<uint32_t>(g) << 8)
             | static_cast<uint32_t>(b);
    }

    void unpack_color(uint32_t rgba, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const {
        r = static_cast<uint8_t>((rgba >> 24) & 0xFF);
        g = static_cast<uint8_t>((rgba >> 16) & 0xFF);
        b = static_cast<uint8_t>((rgba >> 8) & 0xFF);
        a = static_cast<uint8_t>(rgba & 0xFF);
    }

    void fill_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
        ulx += rect_align.left_offset;
        uly += rect_align.top_offset;
        lrx += rect_align.right_offset;
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
            renderer->submit_fill_rect(ulx, uly, lrx, lry, pack_argb(r, g, b, a));
        }
    }

    void submit_triangle(uint8_t i0, uint8_t i1, uint8_t i2) {
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

        if (renderer == nullptr) {
            return;
        }

        const bool use_shade = (geometry_mode & G_SHADE) != 0;
        uint8_t pr, pg, pb, pa;
        unpack_color(prim_color, pr, pg, pb, pa);

        uint8_t r0, g0, b0, a0;
        uint8_t r1, g1, b1, a1;
        uint8_t r2, g2, b2, a2;

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

        const bool translucent = (a0 < 255) || (a1 < 255) || (a2 < 255);
        renderer->submit_triangle(
            v0.screen_x, v0.screen_y, v0.depth, pack_argb(r0, g0, b0, a0),
            v1.screen_x, v1.screen_y, v1.depth, pack_argb(r1, g1, b1, a1),
            v2.screen_x, v2.screen_y, v2.depth, pack_argb(r2, g2, b2, a2),
            translucent
        );
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
            break;
        case G_MW_FORCEMTX:
            s.mvp_dirty = (dl->w1 == 0);
            break;
        default:
            break;
        }
    }

    static void dl_movemem(GbiState& s, DisplayList*& dl) {
        const uint8_t index = static_cast<uint8_t>(dl->p0(0, 8));
        if (index == 8) { // F3DEX2_G_MV_VIEWPORT
            s.set_viewport(dl->w1);
        }
    }

    static void dl_geometrymode(GbiState& s, DisplayList*& dl) {
        const uint32_t off_mask = dl->p0(0, 24);
        const uint32_t on_mask = dl->w1;
        s.geometry_mode &= off_mask;
        s.geometry_mode |= on_mask;
    }

    static void dl_texture(GbiState& s, DisplayList*& dl) {
        s.texture_on = dl->p0(1, 7) != 0;
    }

    static void dl_vtx(GbiState& s, DisplayList*& dl) {
        const uint8_t count = static_cast<uint8_t>(dl->p0(12, 8));
        const uint8_t index = static_cast<uint8_t>(dl->p0(1, 7) - count);
        s.load_vertices(dl->w1, count, index);
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
        const uint32_t off = static_cast<uint32_t>(std::max(0, static_cast<int32_t>(32 - dl->p0(8, 8) - size)));
        const uint32_t mask = ((1u << size) - 1u) << off;
        s.other_mode_h = (s.other_mode_h & ~mask) | ((dl->w1 << off) & mask);
    }

    static void dl_setothermode_l(GbiState& s, DisplayList*& dl) {
        const uint32_t size = dl->p0(0, 8) + 1;
        const uint32_t off = static_cast<uint32_t>(std::max(0, static_cast<int32_t>(32 - dl->p0(8, 8) - size)));
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

    static void dl_setprimcolor(GbiState& s, DisplayList*& dl) {
        s.prim_color = dl->w1;
    }

    static void dl_setfillcolor(GbiState& s, DisplayList*& dl) {
        s.fill_color = dl->w1;
    }

    static void dl_setenvcolor(GbiState& s, DisplayList*& dl) {
        s.env_color = dl->w1;
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
        // Textured rectangles require TMEM; not yet implemented.
        (void)s;
        dl += 2;
    }

    static void dl_extended(GbiState& s, DisplayList*& dl) {
        const uint32_t sub_op = dl->p0(0, 24);
        switch (sub_op) {
        case G_EX_NOOP:
            break;
        case G_EX_FILLRECT_V1: {
            dl++;
            const int32_t ulx = static_cast<int16_t>(dl->p0(16, 16));
            const int32_t uly = static_cast<int16_t>(dl->p0(0, 16));
            const int32_t lrx = static_cast<int16_t>(dl->p1(16, 16));
            const int32_t lry = static_cast<int16_t>(dl->p1(0, 16));
            s.fill_rect(ulx, uly, lrx, lry);
            break;
        }
        case G_EX_SETVIEWPORT_V1:
            dl++;
            s.set_viewport(dl->w1);
            break;
        case G_EX_SETSCISSOR_V1: {
            dl++;
            const int32_t ulx = static_cast<int16_t>(dl->p0(16, 16));
            const int32_t uly = static_cast<int16_t>(dl->p0(0, 16));
            const int32_t lrx = static_cast<int16_t>(dl->p1(16, 16));
            const int32_t lry = static_cast<int16_t>(dl->p1(0, 16));
            s.set_scissor(1, ulx, uly, lrx, lry);
            break;
        }
        case G_EX_SETRECTALIGN_V1:
            dl++;
            s.rect_align.left_offset = static_cast<int16_t>(dl->p0(16, 16));
            s.rect_align.top_offset = static_cast<int16_t>(dl->p0(0, 16));
            s.rect_align.right_offset = static_cast<int16_t>(dl->p1(16, 16));
            s.rect_align.bottom_offset = static_cast<int16_t>(dl->p1(0, 16));
            break;
        case G_EX_FORCEBRANCH_V1:
            s.force_branch = dl->p1(0, 1) != 0;
            break;
        case G_EX_PUSHVIEWPORT_V1:
            break;
        case G_EX_POPVIEWPORT_V1:
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

    static DlHandler gbi_dispatch[256];

    static void init_dispatch() {
        static bool initialized = false;
        if (initialized) {
            return;
        }
        initialized = true;

        for (auto& handler : gbi_dispatch) {
            handler = dl_noop;
        }

        gbi_dispatch[G_NOOP] = dl_noop;
        gbi_dispatch[G_VTX] = dl_vtx;
        gbi_dispatch[G_TRI1] = dl_tri1;
        gbi_dispatch[G_TRI2] = dl_tri2;
        gbi_dispatch[G_QUAD] = dl_quad;
        gbi_dispatch[G_DL] = dl_run_dl;
        gbi_dispatch[G_ENDDL] = dl_enddl;
        gbi_dispatch[G_MTX] = dl_mtx;
        gbi_dispatch[G_POPMTX] = dl_popmtx;
        gbi_dispatch[G_MOVEWORD] = dl_moveword;
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
        gbi_dispatch[G_FILLRECT] = dl_fillrect;
        gbi_dispatch[G_SETSCISSOR] = dl_setscissor;
        gbi_dispatch[G_TEXRECT] = dl_texrect;
        gbi_dispatch[G_TEXRECTFLIP] = dl_texrect;
        gbi_dispatch[G_RDPNOOP] = dl_noop;
        gbi_dispatch[G_RDPLOADSYNC] = dl_noop;
        gbi_dispatch[G_RDPPIPESYNC] = dl_noop;
        gbi_dispatch[G_RDPTILESYNC] = dl_noop;
        gbi_dispatch[G_RDPFULLSYNC] = dl_noop;
        gbi_dispatch[G_SETZIMG] = dl_noop;
        gbi_dispatch[G_SETTIMG] = dl_noop;
        gbi_dispatch[G_SETCOMBINE] = dl_noop;
        gbi_dispatch[G_SETTILE] = dl_noop;
        gbi_dispatch[G_LOADBLOCK] = dl_noop;
        gbi_dispatch[G_LOADTILE] = dl_noop;
        gbi_dispatch[G_SETTILESIZE] = dl_noop;
        gbi_dispatch[G_LOADTLUT] = dl_noop;
    }

    void run_display_list(GbiState& state, DisplayList* dl) {
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
            } else {
                gbi_dispatch[opcode](state, dl);
            }

            if (dl != nullptr) {
                dl++;
            }
        }
    }

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
    GbiState& s = impl_->state;
    s = GbiState{};
    s.renderer = renderer;
    mat4_identity(s.model_matrix);
    mat4_identity(s.proj_matrix);
    mat4_identity(s.model_stack[0]);
    s.mvp_dirty = true;
    s.geometry_mode = G_CULL_BACK;
    s.other_mode_h = 0x080CFF;
    s.force_branch = true;
}

void Interpreter::set_renderer(pvr::Renderer* renderer) {
    if (impl_ == nullptr) {
        reset();
    }
    impl_->state.renderer = renderer;
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
    run_display_list(impl_->state, dl);
}

} // namespace dreamcast::gbi

#endif // DREAMCAST
