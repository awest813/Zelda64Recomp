// Dreamcast PVR (PowerVR2) Render Context
//
// This replaces the RT64 renderer for the Dreamcast platform. It implements
// the ultramodern::renderer::RendererContext interface using KallistiOS PVR
// APIs to drive the PowerVR2 tile-based deferred renderer.
//
// Display lists are high-level-emulated (F3DZEX2) and geometry is submitted
// directly to the PVR tile accelerator (SM64 DC port architecture). N64
// TMEM staging, texture cache, combiners, S2DEX2 BgRectCopy, extended GBI
// viewport/scissor stacks, Z-buffering, and PVR polygon batching are implemented.

#ifdef DREAMCAST

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <memory>
#include <array>
#include <vector>

#include <kos.h>
#include <dc/pvr.h>
#include <dc/video.h>

#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultra64.h"
#include "dreamcast_platform.h"
#include "dc_gbi.h"
#include "dc_pvr_renderer.h"
#include "dc_texture_cache.h"

namespace {

// VI control flags (mirrored from ultramodern/events.cpp and RT64).
constexpr uint32_t VI_CTRL_TYPE_BLANK = 0;
constexpr uint32_t VI_CTRL_TYPE_16 = 0x00002;
constexpr uint32_t VI_CTRL_TYPE_32 = 0x00003;
constexpr uint32_t VI_CTRL_SERRATE_ON = 0x00040;

struct DecodedVI {
    bool valid = false;
    bool visible = false;
    uint32_t origin = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool is_32bit = false;
};

float vi_x_scale_float(uint32_t x_scale) {
    return (x_scale != 0) ? (1024.0f / static_cast<float>(x_scale)) : 1.0f;
}

float vi_y_scale_float(uint32_t y_scale) {
    return (y_scale != 0) ? (1024.0f / static_cast<float>(y_scale)) : 1.0f;
}

DecodedVI decode_vi(const ultramodern::renderer::ViRegs& regs) {
    DecodedVI vi{};

    const uint32_t type = regs.VI_STATUS_REG & 0x3;
    const uint32_t h_start = regs.VI_H_START_REG & 0x3FF;
    vi.visible = (type != VI_CTRL_TYPE_BLANK) && (h_start > 0);
    if (!vi.visible) {
        return vi;
    }

    const uint32_t width = regs.VI_WIDTH_REG & 0xFFF;
    const uint32_t v_start = regs.VI_V_START_REG & 0x3FF;
    const uint32_t v_end = (regs.VI_V_START_REG >> 16) & 0x3FF;
    const bool serrate = (regs.VI_STATUS_REG & VI_CTRL_SERRATE_ON) != 0;

    uint32_t fb_width = width;
    if (serrate) {
        const uint32_t h_end = (regs.VI_H_START_REG >> 16) & 0x3FF;
        const float estimated_width = static_cast<float>(h_end - h_start) / vi_x_scale_float(regs.VI_X_SCALE_REG & 0xFFF);
        constexpr float interlaced_tolerance = 1.875f;
        if (estimated_width < (static_cast<float>(width) / interlaced_tolerance)) {
            fb_width = width / 2;
        }
    }

    const float y_scale = vi_y_scale_float(regs.VI_Y_SCALE_REG & 0xFFF);
    uint32_t fb_height = static_cast<uint32_t>(std::lround(
        static_cast<float>(v_end - v_start) / (2.0f * y_scale * (static_cast<float>(fb_width) / static_cast<float>(width)))
    ));

    // RT64 adds a couple of rows to account for filtering and alignment.
    constexpr uint32_t extra_rows = 2;
    constexpr uint32_t divisor = 4;
    fb_height += extra_rows;
    fb_height = static_cast<uint32_t>(std::lround(static_cast<float>(fb_height) / divisor)) * divisor;

    if (fb_width == 0 || fb_height == 0) {
        return vi;
    }

    uint32_t origin = regs.VI_ORIGIN_REG & 0xFFFFFF;
    if (type >= VI_CTRL_TYPE_16) {
        const bool interlaced_step = serrate && ((regs.VI_V_CURRENT_LINE_REG & 0x1) != 0);
        const uint32_t bytes_per_pixel = (type == VI_CTRL_TYPE_32) ? 4u : 2u;
        const uint32_t row_offset = width * bytes_per_pixel * (interlaced_step ? 2u : 1u);
        if (origin >= row_offset) {
            origin -= row_offset;
        }
    }

    vi.origin = origin;
    vi.width = fb_width;
    vi.height = fb_height;
    vi.is_32bit = (type == VI_CTRL_TYPE_32);
    vi.valid = true;
    return vi;
}

uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1u) / alignment * alignment;
}

// ── N64 framebuffer format conversion (VI blit fallback) ──────────────
// Convert N64 RGBA16 (5551) to PVR ARGB1555
void convert_rgba16_to_argb1555(const uint16_t* src, uint16_t* dst, size_t pixel_count) {
    for (size_t i = 0; i < pixel_count; i++) {
        uint16_t pixel = src[i];
        // N64 RGBA16: RRRRR GGGGG BBBBB A
        // PVR ARGB1555: A RRRRR GGGGG BBBBB
        uint16_t r = (pixel >> 11) & 0x1F;
        uint16_t g = (pixel >> 6) & 0x1F;
        uint16_t b = (pixel >> 1) & 0x1F;
        uint16_t a = pixel & 0x01;
        dst[i] = (a << 15) | (r << 10) | (g << 5) | b;
    }
}

// Convert N64 RGBA32 (RA GB byte order) to PVR ARGB1555.
void convert_rgba32_to_argb1555(const uint32_t* src, uint16_t* dst, size_t pixel_count) {
    for (size_t i = 0; i < pixel_count; i++) {
        uint32_t pixel = src[i];
        uint16_t r = ((pixel >> 24) & 0xFF) >> 3;
        uint16_t g = ((pixel >> 16) & 0xFF) >> 3;
        uint16_t b = ((pixel >> 8) & 0xFF) >> 3;
        uint16_t a = (pixel & 0xFF) ? 0x8000u : 0u;
        dst[i] = a | (r << 10) | (g << 5) | b;
    }
}

// Convert N64 IA8 (4-bit intensity + 4-bit alpha) to PVR ARGB4444
void convert_ia8_to_argb4444(const uint8_t* src, uint16_t* dst, size_t pixel_count) {
    for (size_t i = 0; i < pixel_count; i++) {
        uint8_t intensity = (src[i] >> 4) & 0x0F;
        uint8_t alpha = src[i] & 0x0F;
        dst[i] = (alpha << 12) | (intensity << 8) | (intensity << 4) | intensity;
    }
}

} // anonymous namespace

namespace zelda64::renderer {

// ── PVR Render Context ──────────────────────────────────────────────
// Implements the RendererContext interface for the Dreamcast's PowerVR2 GPU.

class PVRContext final : public ultramodern::renderer::RendererContext {
public:
    PVRContext(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);
    ~PVRContext() override;

    bool valid() override { return initialized_; }

    bool update_config(
        const ultramodern::renderer::GraphicsConfig& old_config,
        const ultramodern::renderer::GraphicsConfig& new_config) override;

    void enable_instant_present() override;
    void send_dl(const OSTask* task) override;
    void update_screen() override;
    void shutdown() override;
    uint32_t get_display_framerate() const override;
    float get_resolution_scale() const override;

private:
    uint8_t* rdram_;
    bool initialized_ = false;
    bool developer_mode_ = false;
    uint32_t display_framerate_ = 60;

    // Cached N64 framebuffer texture in PVR VRAM.
    pvr_ptr_t fb_texture_ = 0;
    uint32_t fb_tex_width_ = 0;
    uint32_t fb_tex_height_ = 0;
    uint32_t fb_tex_stride_ = 0;
    std::vector<uint16_t> fb_upload_buffer_;

    // PVR rendering state
    pvr_poly_hdr_t poly_hdr_opaque_;
    pvr_poly_hdr_t poly_hdr_translucent_;
    pvr_poly_hdr_t poly_hdr_textured_;
    bool textured_hdr_valid_ = false;

    void init_pvr();
    void flush_texture_cache();
    void release_framebuffer_texture();
    bool ensure_framebuffer_texture(uint32_t width, uint32_t height);
    bool upload_framebuffer_texture(const DecodedVI& vi);
    void compile_textured_header();
    void render_framebuffer_to_screen();
    void render_solid_quad(uint32_t argb);
    void process_display_list(const OSTask* task);

    dreamcast::gbi::Interpreter gbi_;
    dreamcast::pvr::Renderer pvr_renderer_;
    dreamcast::tex::Cache texture_cache_;
    uint32_t frame_index_ = 0;
};

PVRContext::PVRContext(uint8_t* rdram, ultramodern::renderer::WindowHandle /*window_handle*/, bool developer_mode)
    : rdram_(rdram), developer_mode_(developer_mode) {
    init_pvr();
}

PVRContext::~PVRContext() {
    shutdown();
}

void PVRContext::init_pvr() {
    // Initialize PVR with basic parameters.
    // Opaque, translucent, and punch-through polygon bins for RDP alpha-test UI.
    pvr_init_params_t pvr_params = {
        { PVR_BINSIZE_16,  // Opaque polygons
          PVR_BINSIZE_0,   // Opaque modifier volumes (disabled)
          PVR_BINSIZE_16,  // Translucent polygons
          PVR_BINSIZE_0,   // Translucent modifier volumes (disabled)
          PVR_BINSIZE_8 }, // Punch-through (G_AC_THRESHOLD)
        512 * 1024,        // Vertex buffer size (512 KB; rest of VRAM for textures/FB)
        0,                 // No DMA
        0,                 // No FSAA
        0                  // Disable translucent auto-sort (we sort manually)
    };

    pvr_init(&pvr_params);

    // Set up default polygon headers
    pvr_poly_cxt_t cxt;

    // Opaque polygon context
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.shading = PVR_SHADE_GOURAUD;
    cxt.gen.culling = PVR_CULLING_NONE; // N64 handles its own culling
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    cxt.depth.write = PVR_DEPTHWRITE_ENABLE;
    pvr_poly_compile(&poly_hdr_opaque_, &cxt);

    // Translucent polygon context
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.shading = PVR_SHADE_GOURAUD;
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    cxt.blend.src = PVR_BLEND_SRCALPHA;
    cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
    pvr_poly_compile(&poly_hdr_translucent_, &cxt);

    pvr_renderer_.set_texture_cache(&texture_cache_);
    gbi_.set_renderer(&pvr_renderer_);
    gbi_.set_texture_cache(&texture_cache_);

    initialized_ = true;
    fprintf(stdout, "[DC] PVR renderer initialized (%dx%d)\n", DC_SCREEN_WIDTH, DC_SCREEN_HEIGHT);
}

void PVRContext::shutdown() {
    if (!initialized_) return;

    release_framebuffer_texture();
    flush_texture_cache();
    pvr_shutdown();
    initialized_ = false;
    fprintf(stdout, "[DC] PVR renderer shut down\n");
}

void PVRContext::release_framebuffer_texture() {
    if (fb_texture_ != 0) {
        pvr_mem_free(fb_texture_);
        fb_texture_ = 0;
    }
    fb_tex_width_ = 0;
    fb_tex_height_ = 0;
    fb_tex_stride_ = 0;
    textured_hdr_valid_ = false;
    fb_upload_buffer_.clear();
}

void PVRContext::flush_texture_cache() {
    texture_cache_.flush();
}

bool PVRContext::update_config(
    const ultramodern::renderer::GraphicsConfig& /*old_config*/,
    const ultramodern::renderer::GraphicsConfig& /*new_config*/) {
    // The Dreamcast has fixed output resolution (640x480 or 320x240 depending
    // on video mode). Most graphics config options are no-ops on this platform.
    return true;
}

void PVRContext::enable_instant_present() {
    // No-op on Dreamcast: PVR always presents after pvr_scene_finish().
}

void PVRContext::send_dl(const OSTask* task) {
    // Begin the PVR scene on the first display list of the frame; subsequent
    // Gfx tasks append geometry before update_screen() presents.
    pvr_renderer_.set_frame_index(frame_index_);
    pvr_renderer_.begin_frame();
    process_display_list(task);
}

void PVRContext::update_screen() {
    if (pvr_renderer_.scene_active()) {
        recompui::render_menu_pvr_background();
        pvr_renderer_.end_frame();
    } else {
        // Fallback: no Gfx tasks ran this frame; present VI framebuffer if set.
        pvr_wait_ready();
        pvr_scene_begin();
        render_framebuffer_to_screen();
        recompui::render_menu_pvr_background();
        pvr_scene_finish();
    }

    // BIOS-font labels are composited on the framebuffer after the PVR scene.
    recompui::render_menu_overlay();

    frame_index_++;
}

uint32_t PVRContext::get_display_framerate() const {
    return display_framerate_;
}

float PVRContext::get_resolution_scale() const {
    // Fixed resolution on Dreamcast.
    return 1.0f;
}

bool PVRContext::ensure_framebuffer_texture(uint32_t width, uint32_t height) {
    const uint32_t stride = align_up(width, 32);
    if (fb_texture_ != 0 && fb_tex_width_ == width && fb_tex_height_ == height && fb_tex_stride_ == stride) {
        return true;
    }

    release_framebuffer_texture();

    const size_t tex_bytes = static_cast<size_t>(stride) * height * sizeof(uint16_t);
    const size_t padded_bytes = align_up(static_cast<uint32_t>(tex_bytes), 32);
    fb_texture_ = pvr_mem_malloc(padded_bytes);
    if (fb_texture_ == 0) {
        fprintf(stderr, "[DC] Failed to allocate %zu bytes for framebuffer texture\n", padded_bytes);
        return false;
    }

    fb_tex_width_ = width;
    fb_tex_height_ = height;
    fb_tex_stride_ = stride;
    fb_upload_buffer_.assign(padded_bytes / sizeof(uint16_t), 0);
    textured_hdr_valid_ = false;
    return true;
}

bool PVRContext::upload_framebuffer_texture(const DecodedVI& vi) {
    if (rdram_ == nullptr || !vi.valid) {
        return false;
    }

    if (!ensure_framebuffer_texture(vi.width, vi.height)) {
        return false;
    }

    const uint32_t n64_stride_pixels = vi.width;
    fb_upload_buffer_.assign(static_cast<size_t>(fb_tex_stride_) * fb_tex_height_, 0);

    if (vi.is_32bit) {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(rdram_ + vi.origin);
        for (uint32_t y = 0; y < vi.height; y++) {
            const uint32_t* row_src = src + static_cast<size_t>(y) * n64_stride_pixels;
            uint16_t* row_dst = fb_upload_buffer_.data() + static_cast<size_t>(y) * fb_tex_stride_;
            convert_rgba32_to_argb1555(row_src, row_dst, vi.width);
        }
    } else {
        const uint16_t* src = reinterpret_cast<const uint16_t*>(rdram_ + vi.origin);
        for (uint32_t y = 0; y < vi.height; y++) {
            const uint16_t* row_src = src + static_cast<size_t>(y) * n64_stride_pixels;
            uint16_t* row_dst = fb_upload_buffer_.data() + static_cast<size_t>(y) * fb_tex_stride_;
            convert_rgba16_to_argb1555(row_src, row_dst, vi.width);
        }
    }

    const size_t upload_bytes = static_cast<size_t>(fb_tex_stride_) * fb_tex_height_ * sizeof(uint16_t);
    const size_t padded_bytes = align_up(static_cast<uint32_t>(upload_bytes), 32);
    pvr_txr_set_stride(fb_tex_stride_);
    pvr_txr_load(fb_upload_buffer_.data(), fb_texture_, padded_bytes);
    return true;
}

void PVRContext::compile_textured_header() {
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    cxt.txr.enable = true;
    cxt.txr.format = PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE;
    cxt.txr.filter = PVR_FILTER_NONE;
    cxt.txr.mipmap = false;
    cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
    cxt.txr.width = static_cast<int>(fb_tex_stride_);
    cxt.txr.height = static_cast<int>(fb_tex_height_);
    cxt.txr.base = fb_texture_;
    pvr_poly_compile(&poly_hdr_textured_, &cxt);
    textured_hdr_valid_ = true;
}

void PVRContext::render_solid_quad(uint32_t argb) {
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&poly_hdr_opaque_, sizeof(pvr_poly_hdr_t));

    pvr_vertex_t vert{};
    vert.z = 1.0f;
    vert.argb = argb;
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

    pvr_list_finish();
}

void PVRContext::render_framebuffer_to_screen() {
    const ultramodern::renderer::ViRegs* vi_regs = ultramodern::renderer::get_vi_regs();
    if (vi_regs == nullptr) {
        render_solid_quad(0xFF000000);
        return;
    }

    const DecodedVI vi = decode_vi(*vi_regs);
    if (!vi.visible) {
        render_solid_quad(0xFF000000);
        return;
    }

    if (!upload_framebuffer_texture(vi)) {
        render_solid_quad(0xFF000000);
        return;
    }

    if (!textured_hdr_valid_) {
        compile_textured_header();
    }

    const float scale = std::min(
        static_cast<float>(DC_SCREEN_WIDTH) / static_cast<float>(vi.width),
        static_cast<float>(DC_SCREEN_HEIGHT) / static_cast<float>(vi.height)
    );
    const float draw_w = static_cast<float>(vi.width) * scale;
    const float draw_h = static_cast<float>(vi.height) * scale;
    const float offset_x = (static_cast<float>(DC_SCREEN_WIDTH) - draw_w) * 0.5f;
    const float offset_y = (static_cast<float>(DC_SCREEN_HEIGHT) - draw_h) * 0.5f;
    const float u_max = static_cast<float>(vi.width) / static_cast<float>(fb_tex_stride_);

    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&poly_hdr_textured_, sizeof(pvr_poly_hdr_t));

    pvr_vertex_t vert{};
    vert.z = 1.0f;
    vert.argb = 0xFFFFFFFF;
    vert.oargb = 0;

    vert.flags = PVR_CMD_VERTEX;
    vert.x = offset_x;
    vert.y = offset_y;
    vert.u = 0.0f;
    vert.v = 0.0f;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    vert.x = offset_x + draw_w;
    vert.y = offset_y;
    vert.u = u_max;
    vert.v = 0.0f;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    vert.flags = PVR_CMD_VERTEX;
    vert.x = offset_x;
    vert.y = offset_y + draw_h;
    vert.u = 0.0f;
    vert.v = 1.0f;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = offset_x + draw_w;
    vert.y = offset_y + draw_h;
    vert.u = u_max;
    vert.v = 1.0f;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    pvr_list_finish();
}

void PVRContext::process_display_list(const OSTask* task) {
    if (task == nullptr || rdram_ == nullptr) {
        return;
    }

    gbi_.process_display_list(rdram_, task);
}

// ── Factory function ────────────────────────────────────────────────

std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
    uint8_t* rdram,
    ultramodern::renderer::WindowHandle window_handle,
    bool developer_mode) {

    auto ctx = std::make_unique<PVRContext>(rdram, window_handle, developer_mode);
    if (!ctx->valid()) {
        fprintf(stderr, "[DC] Failed to create PVR render context\n");
        return nullptr;
    }
    return ctx;
}

} // namespace zelda64::renderer

#endif // DREAMCAST
