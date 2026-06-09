#ifdef DREAMCAST

#include "dc_zbuffer.h"

#include <algorithm>
#include <cmath>

namespace dreamcast::zbuf {

namespace {

int clamp_int(int value, int min_value, int max_value) {
    return std::max(min_value, std::min(value, max_value));
}

} // anonymous namespace

void Buffer::set_size(uint16_t width, uint16_t height) {
    width_ = std::max<uint16_t>(width, 1);
    height_ = std::max<uint16_t>(height, 1);
    pixels_.assign(static_cast<size_t>(width_) * height_, 0.0f);
}

void Buffer::clear() {
    std::fill(pixels_.begin(), pixels_.end(), 0.0f);
}

size_t Buffer::pixel_index(int x, int y) const {
    return static_cast<size_t>(y) * width_ + static_cast<size_t>(x);
}

void Buffer::rasterize_triangle(
    float x0, float y0, float z0,
    float x1, float y1, float z1,
    float x2, float y2, float z2) {

    if (pixels_.empty()) {
        return;
    }

    const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (std::fabs(area) < 1e-6f) {
        return;
    }
    const float inv_area = 1.0f / area;

    const int min_x = clamp_int(static_cast<int>(std::floor(std::min({x0, x1, x2}))), 0, width_ - 1);
    const int max_x = clamp_int(static_cast<int>(std::ceil(std::max({x0, x1, x2}))), 0, width_ - 1);
    const int min_y = clamp_int(static_cast<int>(std::floor(std::min({y0, y1, y2}))), 0, height_ - 1);
    const int max_y = clamp_int(static_cast<int>(std::ceil(std::max({y0, y1, y2}))), 0, height_ - 1);

    for (int y = min_y; y <= max_y; y++) {
        const float py = static_cast<float>(y) + 0.5f;
        for (int x = min_x; x <= max_x; x++) {
            const float px = static_cast<float>(x) + 0.5f;
            const float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) * inv_area;
            const float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) * inv_area;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                continue;
            }

            const float z = w0 * z0 + w1 * z1 + w2 * z2;
            float& stored = pixels_[pixel_index(x, y)];
            if (z > stored) {
                stored = z;
            }
        }
    }
}

bool Buffer::is_occluded(float x, float y, float depth) const {
    if (pixels_.empty()) {
        return false;
    }

    // KOS GCC 9 exposes lround in <math.h> but not in namespace std.
    const int px = static_cast<int>(lround(x));
    const int py = static_cast<int>(lround(y));
    if (px < 0 || py < 0 || px >= width_ || py >= height_) {
        return true;
    }

    const float stored = pixels_[pixel_index(px, py)];
    // A small epsilon avoids flicker when the test point shares depth with the surface.
    return depth < stored - 1e-4f;
}

} // namespace dreamcast::zbuf

#endif // DREAMCAST
