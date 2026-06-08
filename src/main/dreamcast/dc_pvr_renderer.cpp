// PVR polygon submission backend for the Dreamcast GBI high-level emulator.
//
// Mirrors the SM64 DC port architecture (gfx_retro_dc.c): display list commands
// produce GPU primitives instead of writing pixels into emulated RDRAM.

#ifdef DREAMCAST

#include "dc_pvr_renderer.h"

#include "dc_texture_cache.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <kos.h>
#include <dc/pvr.h>

#include "dreamcast_platform.h"

namespace dreamcast::pvr {

namespace {

constexpr int PVR_LIST_INVALID = -1;
constexpr uint8_t G_TX_WRAP = 0;
constexpr uint8_t G_TX_MIRROR = 0x1;
constexpr uint8_t G_TX_CLAMP = 0x2;

bool needs_translucent(uint32_t argb0, uint32_t argb1, uint32_t argb2) {
    // Combiner output is ARGB (alpha in bits 24-31).
    return ((argb0 | argb1 | argb2) & 0xFF000000u) != 0xFF000000u;
}

rdp::BlendState merge_blend(const rdp::BlendState& rdp_blend, bool vertex_translucent) {
    rdp::BlendState merged = rdp_blend;
    if (vertex_translucent) {
        merged.translucent = true;
        if (!merged.blend_enable) {
            merged.blend_enable = true;
            merged.blend_src = PVR_BLEND_SRCALPHA;
            merged.blend_dst = PVR_BLEND_INVSRCALPHA;
        }
        merged.depth_write = false;
    }
    return merged;
}

void apply_blend_to_key(BatchKey& key, const rdp::BlendState& blend, bool zbuffer_enabled) {
    key.translucent = blend.translucent;
    key.zbuffer_enabled = zbuffer_enabled;
    key.depth_write = zbuffer_enabled && blend.depth_write;
    key.blend_enable = blend.blend_enable;
    key.blend_src = blend.blend_src;
    key.blend_dst = blend.blend_dst;
}

} // anonymous namespace

bool Renderer::BatchKey::operator==(const BatchKey& other) const {
    return list_type == other.list_type
        && textured == other.textured
        && translucent == other.translucent
        && gouraud == other.gouraud
        && zbuffer_enabled == other.zbuffer_enabled
        && depth_write == other.depth_write
        && blend_enable == other.blend_enable
        && blend_src == other.blend_src
        && blend_dst == other.blend_dst
        && texture_vram == other.texture_vram
        && pvr_format == other.pvr_format
        && tex_stride == other.tex_stride
        && tex_height == other.tex_height
        && cms == other.cms
        && cmt == other.cmt;
}

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
    update_zbuffer_size();
}

void Renderer::update_zbuffer_size() {
    zbuffer_.set_size(mapping_.fb_width, mapping_.fb_height);
}

bool Renderer::is_occluded(float n64_x, float n64_y, float depth) const {
    return zbuffer_.is_occluded(n64_x, n64_y, depth);
}

void Renderer::record_depth_triangle(
    float x0, float y0, float z0,
    float x1, float y1, float z1,
    float x2, float y2, float z2,
    bool zbuffer_enabled,
    bool translucent) {
    if (!zbuffer_enabled || translucent) {
        return;
    }
    zbuffer_.rasterize_triangle(x0, y0, z0, x1, y1, z1, x2, y2, z2);
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

    flush_batch();
    close_list();
    pvr_list_begin(list_type);
    list_open_ = true;
    current_list_ = list_type;
    batch_hdr_valid_ = false;
}

void Renderer::close_list() {
    flush_batch();
    end_dr();
    if (list_open_) {
        pvr_list_finish();
        list_open_ = false;
    }
    current_list_ = PVR_LIST_INVALID;
    batch_hdr_valid_ = false;
}

void Renderer::flush_batch() {
    // The polygon header is submitted up-front in begin_batch(), so flushing a
    // batch only needs to close out any pending direct-render vertices and mark
    // the batch as inactive. The next begin_batch() will emit a fresh header.
    if (!batch_hdr_valid_) {
        return;
    }
    end_dr();
    batch_hdr_valid_ = false;
}

void Renderer::begin_dr() {
    if (!dr_active_) {
        pvr_dr_init(&dr_state_);
        dr_active_ = true;
    }
}

void Renderer::end_dr() {
    if (dr_active_) {
        pvr_dr_finish();
        dr_active_ = false;
    }
}

void Renderer::submit_vertex_dr(const pvr_vertex_t& vert) {
    begin_dr();
    pvr_vertex_t* dst = static_cast<pvr_vertex_t*>(pvr_dr_target(dr_state_));
    *dst = vert;
    pvr_dr_commit(dst);
}

void Renderer::apply_wrap_modes(pvr_poly_cxt_t& cxt, uint8_t cms, uint8_t cmt) const {
    const bool clamp_u = (cms & G_TX_CLAMP) != 0;
    const bool clamp_v = (cmt & G_TX_CLAMP) != 0;
    if (clamp_u && clamp_v) {
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
    } else if (clamp_u) {
        cxt.txr.uv_clamp = PVR_UVCLAMP_U;
    } else if (clamp_v) {
        cxt.txr.uv_clamp = PVR_UVCLAMP_V;
    } else {
        cxt.txr.uv_clamp = PVR_UVCLAMP_NONE;
    }
}

void Renderer::begin_batch(const BatchKey& key) {
    if (batch_hdr_valid_ && batch_key_ == key) {
        return;
    }

    flush_batch();
    batch_key_ = key;

    pvr_poly_cxt_t cxt;
    if (key.textured) {
        pvr_poly_cxt_txr(&cxt, key.list_type, key.pvr_format, key.tex_stride, key.tex_height, key.texture_vram, PVR_FILTER_NONE);
        apply_wrap_modes(cxt, key.cms, key.cmt);
    } else {
        pvr_poly_cxt_col(&cxt, key.list_type);
    }

    cxt.gen.shading = key.gouraud ? PVR_SHADE_GOURAUD : PVR_SHADE_FLAT;
    cxt.gen.culling = PVR_CULLING_NONE;
    if (!key.zbuffer_enabled) {
        cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    } else {
        cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
        cxt.depth.write = key.depth_write ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE;
    }
    if (key.blend_enable) {
        cxt.blend.src = key.blend_src;
        cxt.blend.dst = key.blend_dst;
    }

    pvr_poly_compile(&batch_hdr_, &cxt);

    // Emit the header into the TA input stream now, before any vertices for this
    // batch are submitted. The PVR processes the list sequentially, so vertices
    // must always follow their governing polygon header.
    end_dr();
    pvr_prim(&batch_hdr_, sizeof(pvr_poly_hdr_t));
    batch_hdr_valid_ = true;
}

void Renderer::clear_screen() {
    BatchKey key{};
    key.list_type = PVR_LIST_OP_POLY;
    key.gouraud = false;
    key.translucent = false;
    key.zbuffer_enabled = false;
    ensure_list(key.list_type);
    begin_batch(key);
    flush_batch();

    pvr_vertex_t vert{};
    vert.z = 0.0f;
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

    batch_hdr_valid_ = false;
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
    batch_hdr_valid_ = false;

    update_mapping();
    update_zbuffer_size();
    zbuffer_.clear();
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
    batch_hdr_valid_ = false;
}

void Renderer::submit_triangle(
    float x0, float y0, float z0, uint32_t argb0,
    float x1, float y1, float z1, uint32_t argb1,
    float x2, float y2, float z2, uint32_t argb2,
    const rdp::BlendState& blend,
    bool zbuffer_enabled) {

    if (!scene_active_) {
        begin_frame();
    }

    const rdp::BlendState use_blend = merge_blend(blend, needs_translucent(argb0, argb1, argb2));
    const int list_type = use_blend.translucent ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY;

    BatchKey key{};
    key.list_type = list_type;
    key.gouraud = true;
    apply_blend_to_key(key, use_blend, zbuffer_enabled);
    ensure_list(list_type);
    begin_batch(key);

    pvr_vertex_t vert{};
    vert.oargb = 0;

    vert.flags = PVR_CMD_VERTEX;
    vert.x = map_x(x0);
    vert.y = map_y(y0);
    vert.z = z0;
    vert.argb = argb0;
    submit_vertex_dr(vert);

    vert.x = map_x(x1);
    vert.y = map_y(y1);
    vert.z = z1;
    vert.argb = argb1;
    submit_vertex_dr(vert);

    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = map_x(x2);
    vert.y = map_y(y2);
    vert.z = z2;
    vert.argb = argb2;
    submit_vertex_dr(vert);

    record_depth_triangle(x0, y0, z0, x1, y1, z1, x2, y2, z2, zbuffer_enabled, use_blend.translucent);
    drew_geometry_ = true;
}

void Renderer::submit_textured_triangle(
    float x0, float y0, float z0, float u0, float v0, uint32_t argb0,
    float x1, float y1, float z1, float u1, float v1, uint32_t argb1,
    float x2, float y2, float z2, float u2, float v2, uint32_t argb2,
    const tex::Surface& texture,
    const rdp::BlendState& blend,
    bool zbuffer_enabled) {

    if (!texture.valid || texture.vram == 0) {
        submit_triangle(x0, y0, z0, argb0, x1, y1, z1, argb1, x2, y2, z2, argb2, blend, zbuffer_enabled);
        return;
    }

    if (!scene_active_) {
        begin_frame();
    }

    const rdp::BlendState use_blend = merge_blend(blend, needs_translucent(argb0, argb1, argb2));
    const int list_type = use_blend.translucent ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY;

    BatchKey key{};
    key.list_type = list_type;
    key.textured = true;
    key.gouraud = true;
    key.texture_vram = texture.vram;
    key.pvr_format = texture.pvr_format;
    key.tex_stride = texture.stride;
    key.tex_height = texture.height;
    key.cms = texture.cms;
    key.cmt = texture.cmt;
    apply_blend_to_key(key, use_blend, zbuffer_enabled);
    ensure_list(list_type);
    begin_batch(key);

    pvr_vertex_t vert{};
    vert.oargb = 0;

    vert.flags = PVR_CMD_VERTEX;
    vert.x = map_x(x0);
    vert.y = map_y(y0);
    vert.z = z0;
    vert.u = u0;
    vert.v = v0;
    vert.argb = argb0;
    submit_vertex_dr(vert);

    vert.x = map_x(x1);
    vert.y = map_y(y1);
    vert.z = z1;
    vert.u = u1;
    vert.v = v1;
    vert.argb = argb1;
    submit_vertex_dr(vert);

    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = map_x(x2);
    vert.y = map_y(y2);
    vert.z = z2;
    vert.u = u2;
    vert.v = v2;
    vert.argb = argb2;
    submit_vertex_dr(vert);

    record_depth_triangle(x0, y0, z0, x1, y1, z1, x2, y2, z2, zbuffer_enabled, use_blend.translucent);
    drew_geometry_ = true;
}

void Renderer::submit_fill_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint32_t argb, const rdp::BlendState& blend, bool zbuffer_enabled) {
    if (!scene_active_) {
        begin_frame();
    }

    const float x0 = static_cast<float>(ulx) / 4.0f;
    const float y0 = static_cast<float>(uly) / 4.0f;
    const float x1 = static_cast<float>(lrx) / 4.0f;
    const float y1 = static_cast<float>(lry) / 4.0f;

    const rdp::BlendState use_blend = merge_blend(blend, (argb & 0xFF000000u) != 0xFF000000u);
    const int list_type = use_blend.translucent ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY;

    BatchKey key{};
    key.list_type = list_type;
    key.gouraud = false;
    apply_blend_to_key(key, use_blend, zbuffer_enabled);
    ensure_list(list_type);
    begin_batch(key);

    const float mx0 = map_x(x0);
    const float my0 = map_y(y0);
    const float mx1 = map_x(x1);
    const float my1 = map_y(y1);
    const float rect_depth = zbuffer_enabled ? 1.0f : 0.0f;

    pvr_vertex_t vert{};
    vert.z = rect_depth;
    vert.argb = argb;
    vert.oargb = 0;

    vert.flags = PVR_CMD_VERTEX;
    vert.x = mx0;
    vert.y = my0;
    submit_vertex_dr(vert);

    vert.x = mx1;
    vert.y = my0;
    submit_vertex_dr(vert);

    vert.flags = PVR_CMD_VERTEX;
    vert.x = mx0;
    vert.y = my1;
    submit_vertex_dr(vert);

    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = mx1;
    vert.y = my1;
    submit_vertex_dr(vert);

    record_depth_triangle(x0, y0, rect_depth, x1, y0, rect_depth, x0, y1, rect_depth, zbuffer_enabled, use_blend.translucent);
    record_depth_triangle(x1, y0, rect_depth, x1, y1, rect_depth, x0, y1, rect_depth, zbuffer_enabled, use_blend.translucent);
    drew_geometry_ = true;
}

void Renderer::submit_tex_rect(
    int32_t ulx, int32_t uly, int32_t lrx, int32_t lry,
    float uls, float ult, float lrs, float lrt,
    const tex::Surface& texture,
    uint32_t argb,
    const rdp::BlendState& blend,
    bool zbuffer_enabled) {

    if (!texture.valid || texture.vram == 0) {
        submit_fill_rect(ulx, uly, lrx, lry, argb, blend, zbuffer_enabled);
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

    const rdp::BlendState use_blend = merge_blend(blend, (argb & 0xFF000000u) != 0xFF000000u);
    const int list_type = use_blend.translucent ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY;

    BatchKey key{};
    key.list_type = list_type;
    key.textured = true;
    key.gouraud = false;
    key.texture_vram = texture.vram;
    key.pvr_format = texture.pvr_format;
    key.tex_stride = texture.stride;
    key.tex_height = texture.height;
    key.cms = texture.cms;
    key.cmt = texture.cmt;
    apply_blend_to_key(key, use_blend, zbuffer_enabled);
    ensure_list(list_type);
    begin_batch(key);

    const float mx0 = map_x(x0);
    const float my0 = map_y(y0);
    const float mx1 = map_x(x1);
    const float my1 = map_y(y1);
    const float rect_depth = zbuffer_enabled ? 1.0f : 0.0f;

    pvr_vertex_t vert{};
    vert.z = rect_depth;
    vert.argb = argb;
    vert.oargb = 0;

    vert.flags = PVR_CMD_VERTEX;
    vert.x = mx0;
    vert.y = my0;
    vert.u = u0;
    vert.v = v0;
    submit_vertex_dr(vert);

    vert.x = mx1;
    vert.y = my0;
    vert.u = u1;
    vert.v = v0;
    submit_vertex_dr(vert);

    vert.flags = PVR_CMD_VERTEX;
    vert.x = mx0;
    vert.y = my1;
    vert.u = u0;
    vert.v = v1;
    submit_vertex_dr(vert);

    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = mx1;
    vert.y = my1;
    vert.u = u1;
    vert.v = v1;
    submit_vertex_dr(vert);

    record_depth_triangle(x0, y0, rect_depth, x1, y0, rect_depth, x0, y1, rect_depth, zbuffer_enabled, use_blend.translucent);
    record_depth_triangle(x1, y0, rect_depth, x1, y1, rect_depth, x0, y1, rect_depth, zbuffer_enabled, use_blend.translucent);
    drew_geometry_ = true;
}

} // namespace dreamcast::pvr

#endif // DREAMCAST
