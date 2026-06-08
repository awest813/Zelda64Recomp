#ifndef __DC_ZBUFFER_H__
#define __DC_ZBUFFER_H__

#ifdef DREAMCAST

#include <cstdint>
#include <vector>

namespace dreamcast::zbuf {

// CPU-side depth buffer used to emulate N64 Z testing on Dreamcast.
// Depth values follow PVR convention: larger Z = closer to the camera.
class Buffer {
public:
    void set_size(uint16_t width, uint16_t height);
    void clear();

    void rasterize_triangle(
        float x0, float y0, float z0,
        float x1, float y1, float z1,
        float x2, float y2, float z2);

    // Returns true when (x, y) is behind the closest stored surface.
    bool is_occluded(float x, float y, float depth) const;

    uint16_t width() const { return width_; }
    uint16_t height() const { return height_; }

private:
    uint16_t width_ = 320;
    uint16_t height_ = 240;
    std::vector<float> pixels_;

    size_t pixel_index(int x, int y) const;
};

} // namespace dreamcast::zbuf

#endif // DREAMCAST

#endif // __DC_ZBUFFER_H__
