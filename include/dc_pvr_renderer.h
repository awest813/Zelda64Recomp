#ifndef __DC_PVR_RENDERER_H__
#define __DC_PVR_RENDERER_H__

#ifdef DREAMCAST

#include <cstdint>

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

    void begin_frame();
    void end_frame();

    void set_framebuffer_size(uint16_t width, uint16_t height);

    void submit_triangle(
        float x0, float y0, float z0, uint32_t argb0,
        float x1, float y1, float z1, uint32_t argb1,
        float x2, float y2, float z2, uint32_t argb2,
        bool translucent);

    void submit_fill_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint32_t argb);

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

    bool scene_active_ = false;
    bool drew_geometry_ = false;
    bool list_open_ = false;
    int current_list_ = -1;

    ScreenMapping mapping_;

    void update_mapping();
    void clear_screen();
    void ensure_list(int list_type);
    void close_list();

    float map_x(float n64_x) const;
    float map_y(float n64_y) const;
    static uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
};

} // namespace dreamcast::pvr

#endif // DREAMCAST

#endif // __DC_PVR_RENDERER_H__
