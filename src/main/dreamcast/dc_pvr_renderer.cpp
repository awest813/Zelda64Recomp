// PVR polygon submission backend for the Dreamcast GBI high-level emulator.
//
// Mirrors the SM64 DC port architecture (gfx_retro_dc.c): display list commands
// produce GPU primitives instead of writing pixels into emulated RDRAM.

#ifdef DREAMCAST

#include "dc_pvr_renderer.h"

#include "dc_texture_cache.h"

#include <algorithm>
#include <cmath>

#include <kos.h>
#include <dc/pvr.h>

#include "dreamcast_platform.h"

namespace dreamcast::pvr {

namespace {

constexpr int PVR_LIST_INVALID = -1;

bool needs_translucent(uint32_t argb0, uint32_t argb1, uint32_t argb2) {
    const uint32_t a0 = argb0 & 0xFF;
    const uint32_t a1 = argb1 & 0xFF;
    const uint32_t a2 = argb2 & 0xFF;
    return (a0 < 255u) || (a1 < 255u) || (a2 < 255u);
}

} // anonymous namespace

Renderer::Renderer() = default;

Renderer::~Renderer() {
    if (scene_active_) {
        end_frame();
    }
}

void Renderer::set_texture_cache(tex::Cache* cache) {
    texture_cache_ = cache;
}

void Renderer::set_frame_index(uint32_t frame) {
    frame_index_ = frame;
    if (texture_cache_ != nullptr) {
        texture_cache_->set_frame(frame);
    }
}

void Renderer::set_framebuffer_size(uint16_t width, uint16_t height) {
    if (width == 0 || height == 0) {
        return;
    }
    mapping_.fb_width = width;
    mapping_.fb_height = height;
    update_mapping();
}

void Renderer::update_mapping() {
    const float fb_w = static_cast<float>(mapping_.fb_width);
    const float fb_h = static_cast<float>(mapping_.fb_height);
    mapping_.scale = std::min(
        static_cast<float>(DC_SCREEN_WIDTH) / fb_w,
        static_cast<float>(DC_SCREEN_HEIGHT) / fb_h
    );
    const float draw_w = fb_w * mapping_.scale;
    const float draw_h = fb_h * mapping_.scale;
    mapping_.offset_x = (static_cast<float>(DC_SCREEN_WIDTH) - draw_w) * 0.5f;
    mapping_.offset_y = (static_cast<float>(DC_SCREEN_HEIGHT) - draw_h) * 0.5f;
}

float Renderer::map_x(float n64_x) const {
    return mapping_.offset_x + n64_x * mapping_.scale;
}

float Renderer::map_y(float n64_y) const {
    return mapping_.offset_y + n64_y * mapping_.scale;
}

void Renderer::ensure_list(int list_type) {
    if (current_list_ == list_type && list_open_) {
        return;
    }

    close_list();
    pvr_list_begin(list_type);
    list_open_ = true;
    current_list_ = list_type;
}

void Renderer::close_list() {
    if (list_open_) {
        pvr_list_finish();
        list_open_ = false;
    }
    current_list_ = PVR_LIST_INVALID;
}

void Renderer::clear_screen() {
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.shading = PVR_SHADE_FLAT;
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;

    pvr_poly_hdr_t hdr;
    pvr_poly_compile(&hdr, &cxt);

    ensure_list(PVR_LIST_OP_POLY);
    pvr_prim(&hdr, sizeof(pvr_poly_hdr_t));

    pvr_vertex_t vert{};
    vert.z = 1.0f;
    vert.argb = 0xFF000000;
    vert.oargb = 0;

    vert.flags = PVR_CMD_VERTEX;
    vert.x = 0.0f;
    vert.y = 0.0f;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    vert.x = static_cast<float>(DC_SCREEN_WIDTH);
    vert.y = 0.0f;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    vert.flags = PVR_CMD_VERTEX;
    vert.x = 0.0f;
    vert.y = static_cast<float>(DC_SCREEN_HEIGHT);
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = static_cast<float>(DC_SCREEN_WIDTH);
    vert.y = static_cast<float>(DC_SCREEN_HEIGHT);
    pvr_prim(&vert, sizeof(pvr_vertex_t));
}

void Renderer::begin_frame() {
    if (scene_active_) {
        return;
    }

    pvr_wait_ready();
    pvr_scene_begin();
    scene_active_ = true;
    drew_geometry_ = false;
    current_list_ = PVR_LIST_INVALID;
    list_open_ = false;

    update_mapping();
    clear_screen();
}

void Renderer::end_frame() {
    if (!scene_active_) {
        return;
    }

    close_list();
    pvr_scene_finish();
    scene_active_ = false;
    drew_geometry_ = false;
    current_list_ = PVR_LIST_INVALID;
}

void Renderer::submit_triangle(
    float x0, float y0, float z0, uint32_t argb0,
    float x1, float y1, float z1, uint32_t argb1,
    float x2, float y2, float z2, uint32_t argb2,
    bool translucent) {

    if (!scene_active_) {
        begin_frame();
    }

    const bool use_translucent = translucent || needs_translucent(argb0, argb1, argb2);
    const int list_type = use_translucent ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY;

    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_col(&cxt, list_type);
    cxt.gen.shading = PVR_SHADE_GOURAUD;
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    cxt.depth.write = use_translucent ? PVR_DEPTHWRITE_DISABLE : PVR_DEPTHWRITE_ENABLE;
    if (use_translucent) {
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
    }

    pvr_poly_hdr_t hdr;
    pvr_poly_compile(&hdr, &cxt);

    ensure_list(list_type);
    pvr_prim(&hdr, sizeof(pvr_poly_hdr_t));

    const float mx0 = map_x(x0);
    const float my0 = map_y(y0);
    const float mx1 = map_x(x1);
    const float my1 = map_y(y1);
    const float mx2 = map_x(x2);
    const float my2 = map_y(y2);

    pvr_vertex_t vert{};
    vert.oargb = 0;

    vert.flags = PVR_CMD_VERTEX;
    vert.x = mx0;
    vert.y = my0;
    vert.z = z0;
    vert.argb = argb0;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    vert.x = mx1;
    vert.y = my1;
    vert.z = z1;
    vert.argb = argb1;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = mx2;
    vert.y = my2;
    vert.z = z2;
    vert.argb = argb2;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    drew_geometry_ = true;
}

void Renderer::submit_textured_triangle(
    float x0, float y0, float z0, float u0, float v0, uint32_t argb0,
    float x1, float y1, float z1, float u1, float v1, uint32_t argb1,
    float x2, float y2, float z2, float u2, float v2, uint32_t argb2,
    const tex::Surface& texture,
    bool translucent) {

    if (!texture.valid || texture.vram == 0) {
        submit_triangle(x0, y0, z0, argb0, x1, y1, z1, argb1, x2, y2, z2, argb2, translucent);
        return;
    }

    if (!scene_active_) {
        begin_frame();
    }

    const bool use_translucent = translucent || needs_translucent(argb0, argb1, argb2);
    const int list_type = use_translucent ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY;

    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, list_type, texture.pvr_format, texture.stride, texture.height, texture.vram, PVR_FILTER_NONE);
    cxt.gen.shading = PVR_SHADE_GOURAUD;
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    cxt.depth.write = use_translucent ? PVR_DEPTHWRITE_DISABLE : PVR_DEPTHWRITE_ENABLE;
    cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
    if (use_translucent) {
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
    }

    pvr_poly_hdr_t hdr;
    pvr_poly_compile(&hdr, &cxt);

    ensure_list(list_type);
    pvr_prim(&hdr, sizeof(pvr_poly_hdr_t));

    pvr_vertex_t vert{};
    vert.oargb = 0;

    vert.flags = PVR_CMD_VERTEX;
    vert.x = map_x(x0);
    vert.y = map_y(y0);
    vert.z = z0;
    vert.u = u0;
    vert.v = v0;
    vert.argb = argb0;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    vert.x = map_x(x1);
    vert.y = map_y(y1);
    vert.z = z1;
    vert.u = u1;
    vert.v = v1;
    vert.argb = argb1;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = map_x(x2);
    vert.y = map_y(y2);
    vert.z = z2;
    vert.u = u2;
    vert.v = v2;
    vert.argb = argb2;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    drew_geometry_ = true;
}

void Renderer::submit_fill_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint32_t argb) {
    if (!scene_active_) {
        begin_frame();
    }

    // RDP rectangle coordinates are in 1/4-pixel units.
    const float x0 = static_cast<float>(ulx) / 4.0f;
    const float y0 = static_cast<float>(uly) / 4.0f;
    const float x1 = static_cast<float>(lrx) / 4.0f;
    const float y1 = static_cast<float>(lry) / 4.0f;

    const bool translucent = (argb & 0xFFu) < 255u;
    const int list_type = translucent ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY;

    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_col(&cxt, list_type);
    cxt.gen.shading = PVR_SHADE_FLAT;
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    if (translucent) {
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
    }

    pvr_poly_hdr_t hdr;
    pvr_poly_compile(&hdr, &cxt);

    ensure_list(list_type);
    pvr_prim(&hdr, sizeof(pvr_poly_hdr_t));

    const float mx0 = map_x(x0);
    const float my0 = map_y(y0);
    const float mx1 = map_x(x1);
    const float my1 = map_y(y1);

    pvr_vertex_t vert{};
    vert.z = 0.5f;
    vert.argb = argb;
    vert.oargb = 0;

    vert.flags = PVR_CMD_VERTEX;
    vert.x = mx0;
    vert.y = my0;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    vert.x = mx1;
    vert.y = my0;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    vert.flags = PVR_CMD_VERTEX;
    vert.x = mx0;
    vert.y = my1;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = mx1;
    vert.y = my1;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    drew_geometry_ = true;
}

void Renderer::submit_tex_rect(
    int32_t ulx, int32_t uly, int32_t lrx, int32_t lry,
    float uls, float ult, float lrs, float lrt,
    const tex::Surface& texture,
    uint32_t argb,
    bool translucent) {

    if (!texture.valid || texture.vram == 0) {
        submit_fill_rect(ulx, uly, lrx, lry, argb);
        return;
    }

    if (!scene_active_) {
        begin_frame();
    }

    const float x0 = static_cast<float>(ulx) / 4.0f;
    const float y0 = static_cast<float>(uly) / 4.0f;
    const float x1 = static_cast<float>(lrx) / 4.0f;
    const float y1 = static_cast<float>(lry) / 4.0f;

    const float tex_w = static_cast<float>(texture.width);
    const float tex_h = static_cast<float>(texture.height);
    const float u0 = uls / tex_w;
    const float v0 = ult / tex_h;
    const float u1 = lrs / tex_w;
    const float v1 = lrt / tex_h;

    const bool use_translucent = translucent || ((argb & 0xFFu) < 255u);
    const int list_type = use_translucent ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY;

    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, list_type, texture.pvr_format, texture.stride, texture.height, texture.vram, PVR_FILTER_NONE);
    cxt.gen.shading = PVR_SHADE_FLAT;
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
    if (use_translucent) {
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
    }

    pvr_poly_hdr_t hdr;
    pvr_poly_compile(&hdr, &cxt);

    ensure_list(list_type);
    pvr_prim(&hdr, sizeof(pvr_poly_hdr_t));

    const float mx0 = map_x(x0);
    const float my0 = map_y(y0);
    const float mx1 = map_x(x1);
    const float my1 = map_y(y1);

    pvr_vertex_t vert{};
    vert.z = 0.5f;
    vert.argb = argb;
    vert.oargb = 0;

    vert.flags = PVR_CMD_VERTEX;
    vert.x = mx0;
    vert.y = my0;
    vert.u = u0;
    vert.v = v0;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    vert.x = mx1;
    vert.y = my0;
    vert.u = u1;
    vert.v = v0;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    vert.flags = PVR_CMD_VERTEX;
    vert.x = mx0;
    vert.y = my1;
    vert.u = u0;
    vert.v = v1;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = mx1;
    vert.y = my1;
    vert.u = u1;
    vert.v = v1;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    drew_geometry_ = true;
}

} // namespace dreamcast::pvr

#endif // DREAMCAST
