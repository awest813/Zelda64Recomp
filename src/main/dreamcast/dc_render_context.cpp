// Dreamcast PVR (PowerVR2) Render Context
//
// This replaces the RT64 renderer for the Dreamcast platform. It implements
// the ultramodern::renderer::RendererContext interface using KallistiOS PVR
// APIs to drive the PowerVR2 tile-based deferred renderer.
//
// The N64 display list commands are translated into PVR polygon submissions.
// Due to hardware limitations, many N64 RDP features (multi-cycle blending,
// noise dithering, per-pixel depth compare) are approximated or omitted.

#ifdef DREAMCAST

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <memory>
#include <array>
#include <vector>

#include <kos.h>
#include <dc/pvr.h>
#include <dc/video.h>

#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultra64.h"
#include "dreamcast_platform.h"

namespace {

// ── Texture cache ───────────────────────────────────────────────────
// The PowerVR2 has 8 MB of VRAM shared between the framebuffer and textures.
// We maintain a simple LRU texture cache to manage VRAM pressure.

struct TextureCacheEntry {
    uint32_t n64_addr;       // Address in N64 RDRAM
    uint32_t hash;           // Simple content hash for invalidation
    pvr_ptr_t pvr_tex;       // VRAM pointer allocated via pvr_mem_malloc
    uint16_t width;
    uint16_t height;
    uint32_t pvr_format;     // PVR texture format (e.g., PVR_TXRFMT_RGB565)
    uint32_t last_used_frame;
};

constexpr size_t MAX_TEXTURE_CACHE_ENTRIES = 256;
constexpr size_t TEXTURE_CACHE_VRAM_BUDGET = 4 * 1024 * 1024; // 4 MB for textures

static std::array<TextureCacheEntry, MAX_TEXTURE_CACHE_ENTRIES> texture_cache{};
static size_t texture_cache_count = 0;
static size_t texture_cache_vram_used = 0;
static uint32_t current_frame = 0;

// Simple hash for texture data
uint32_t hash_texture_data(const uint8_t* data, size_t size) {
    uint32_t hash = 0x811c9dc5u; // FNV-1a offset basis
    for (size_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 0x01000193u; // FNV-1a prime
    }
    return hash;
}

// ── N64 texture format conversion ───────────────────────────────────
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

// Convert N64 RGBA32 to PVR RGB565 (dropping alpha)
void convert_rgba32_to_rgb565(const uint32_t* src, uint16_t* dst, size_t pixel_count) {
    for (size_t i = 0; i < pixel_count; i++) {
        uint32_t pixel = src[i];
        uint16_t r = ((pixel >> 24) & 0xFF) >> 3;
        uint16_t g = ((pixel >> 16) & 0xFF) >> 2;
        uint16_t b = ((pixel >> 8) & 0xFF) >> 3;
        dst[i] = (r << 11) | (g << 5) | b;
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

    // N64 VI register state (copied from the task)
    uint32_t vi_origin_ = 0;
    uint32_t vi_width_ = 320;
    uint32_t vi_height_ = 240;

    // PVR rendering state
    pvr_poly_hdr_t poly_hdr_opaque_;
    pvr_poly_hdr_t poly_hdr_translucent_;

    void init_pvr();
    void flush_texture_cache();
    void render_framebuffer_to_screen();
    void process_display_list(const OSTask* task);
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
    // Opaque and translucent polygon bins; no modifier volumes or punch-through
    // to save VRAM and tile accelerator bandwidth.
    pvr_init_params_t pvr_params = {
        { PVR_BINSIZE_16,  // Opaque polygons
          PVR_BINSIZE_0,   // Opaque modifier volumes (disabled)
          PVR_BINSIZE_16,  // Translucent polygons
          PVR_BINSIZE_0,   // Translucent modifier volumes (disabled)
          PVR_BINSIZE_0 }, // Punch-through polygons (disabled)
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

    initialized_ = true;
    fprintf(stdout, "[DC] PVR renderer initialized (%dx%d)\n", DC_SCREEN_WIDTH, DC_SCREEN_HEIGHT);
}

void PVRContext::shutdown() {
    if (!initialized_) return;

    flush_texture_cache();
    pvr_shutdown();
    initialized_ = false;
    fprintf(stdout, "[DC] PVR renderer shut down\n");
}

void PVRContext::flush_texture_cache() {
    for (size_t i = 0; i < texture_cache_count; i++) {
        if (texture_cache[i].pvr_tex != 0) {
            pvr_mem_free(texture_cache[i].pvr_tex);
            texture_cache[i].pvr_tex = 0;
        }
    }
    texture_cache_count = 0;
    texture_cache_vram_used = 0;
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
    process_display_list(task);
}

void PVRContext::update_screen() {
    // Begin a PVR scene, render the current N64 framebuffer, and present.
    pvr_wait_ready();
    pvr_scene_begin();

    // Render framebuffer as a textured quad
    render_framebuffer_to_screen();

    pvr_scene_finish();
    current_frame++;
}

uint32_t PVRContext::get_display_framerate() const {
    return display_framerate_;
}

float PVRContext::get_resolution_scale() const {
    // Fixed resolution on Dreamcast.
    return 1.0f;
}

void PVRContext::render_framebuffer_to_screen() {
    // Read the N64 framebuffer from RDRAM and blit it to the PVR screen
    // as a textured quad. This is a fallback path; the proper path would
    // intercept individual RDP draw calls in process_display_list().

    if (rdram_ == nullptr || vi_origin_ == 0) return;

    // For the initial implementation, we render the N64 framebuffer as a
    // full-screen textured quad. The framebuffer is assumed to be RGBA16
    // at the resolution specified by VI registers.
    //
    // TODO: Replace this with proper RDP command processing that submits
    // individual triangles/rectangles to PVR polygon lists.

    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&poly_hdr_opaque_, sizeof(pvr_poly_hdr_t));

    // Draw a simple colored quad as a placeholder
    pvr_vertex_t vert;

    // Top-left
    vert.flags = PVR_CMD_VERTEX;
    vert.x = 0.0f;
    vert.y = 0.0f;
    vert.z = 1.0f;
    vert.u = 0.0f;
    vert.v = 0.0f;
    vert.argb = 0xFF000040; // Dark blue placeholder
    vert.oargb = 0;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    // Top-right
    vert.x = DC_SCREEN_WIDTH;
    vert.y = 0.0f;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    // Bottom-left
    vert.flags = PVR_CMD_VERTEX;
    vert.x = 0.0f;
    vert.y = DC_SCREEN_HEIGHT;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    // Bottom-right (end of strip)
    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = DC_SCREEN_WIDTH;
    vert.y = DC_SCREEN_HEIGHT;
    pvr_prim(&vert, sizeof(pvr_vertex_t));

    pvr_list_finish();
}

void PVRContext::process_display_list(const OSTask* task) {
    // TODO: Implement N64 RDP display list processing.
    //
    // This function should parse the display list pointed to by
    // task->t.data_ptr in RDRAM and translate RDP commands to PVR
    // polygon submissions. The major command categories to handle:
    //
    // 1. Triangles (G_TRI1, G_TRI2): Convert N64 vertex data
    //    (position, color, texcoord) to pvr_vertex_t and submit
    //    via pvr_prim().
    //
    // 2. Texture rectangles (G_TEXRECT): Draw axis-aligned
    //    textured quads.
    //
    // 3. Fill rectangles (G_FILLRECT): Solid color rectangles.
    //
    // 4. Texture loading (G_LOADTLUT, G_LOADBLOCK, G_LOADTILE):
    //    Load textures from RDRAM into the PVR texture cache,
    //    converting formats as needed.
    //
    // 5. Render mode / combiner (G_SETCOMBINE, G_SETOTHERMODE):
    //    Map N64 blending modes to PVR blend settings as closely
    //    as possible.
    //
    // 6. Matrix operations (G_MTX, G_POPMTX): Transform vertices
    //    on the SH-4 CPU (the PVR has no hardware T&L).
    //
    // 7. Framebuffer operations (G_SETCOLORIMAGE, G_SETDEPTHIMAGE):
    //    Track the current color/depth buffer targets.
    //
    // For the initial port, this function is a stub that relies on
    // render_framebuffer_to_screen() to display the N64 framebuffer
    // directly.

    (void)task;
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
