#ifndef __DC_PVR_RENDERER_H__
#define __DC_PVR_RENDERER_H__

#ifdef DREAMCAST

#include <cstdint>

#include <dc/pvr.h>

namespace dreamcast::tex {
class Cache;
struct Surface;
}

namespace dreamcast::pvr {

// SM64-style PVR rendering backend for the GBI high-level emulator.
// Gfx/RDP commands submit triangles and rectangles directly to the PowerVR2
// tile accelerator instead of software-rasterizing into N64 RDRAM.
class Renderer {
public:
    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void set_texture_cache(tex::Cache* cache);
    void set_frame_index(uint32_t frame);

    void begin_frame();
    void end_frame();

    void set_framebuffer_size(uint16_t width, uint16_t height);

    void submit_triangle(
        float x0, float y0, float z0, uint32_t argb0,
        float x1, float y1, float z1, uint32_t argb1,
        float x2, float y2, float z2, uint32_t argb2,
        bool translucent);

    void submit_textured_triangle(
        float x0, float y0, float z0, float u0, float v0, uint32_t argb0,
        float x1, float y1, float z1, float u1, float v1, uint32_t argb1,
        float x2, float y2, float z2, float u2, float v2, uint32_t argb2,
        const tex::Surface& texture,
        bool translucent);

    void submit_fill_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint32_t argb);

    void submit_tex_rect(
        int32_t ulx, int32_t uly, int32_t lrx, int32_t lry,
        float uls, float ult, float lrs, float lrt,
        const tex::Surface& texture,
        uint32_t argb,
        bool translucent);

    bool scene_active() const { return scene_active_; }
    bool drew_geometry() const { return drew_geometry_; }

private:
    struct ScreenMapping {
        float offset_x = 0.0f;
        float offset_y = 0.0f;
        float scale = 1.0f;
        uint16_t fb_width = 320;
        uint16_t fb_height = 240;
    };

    struct BatchKey {
        int list_type = -1;
        bool textured = false;
        bool translucent = false;
        bool gouraud = true;
        pvr_ptr_t texture_vram = 0;
        uint32_t pvr_format = 0;
        uint16_t tex_stride = 0;
        uint16_t tex_height = 0;
        uint8_t cms = 0;
        uint8_t cmt = 0;

        bool operator==(const BatchKey& other) const;
    };

    tex::Cache* texture_cache_ = nullptr;
    uint32_t frame_index_ = 0;

    bool scene_active_ = false;
    bool drew_geometry_ = false;
    bool list_open_ = false;
    int current_list_ = -1;

    BatchKey batch_key_{};
    bool batch_hdr_valid_ = false;
    pvr_poly_hdr_t batch_hdr_{};

    ScreenMapping mapping_;

    void update_mapping();
    void clear_screen();
    void ensure_list(int list_type);
    void close_list();
    void flush_batch();
    void begin_batch(const BatchKey& key);
    void apply_wrap_modes(pvr_poly_cxt_t& cxt, uint8_t cms, uint8_t cmt) const;

    float map_x(float n64_x) const;
    float map_y(float n64_y) const;
};

} // namespace dreamcast::pvr

#endif // DREAMCAST

#endif // __DC_PVR_RENDERER_H__
