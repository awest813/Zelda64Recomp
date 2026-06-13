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
    width_ = std::max<uint16_t>(width / 2, 1);
    height_ = std::max<uint16_t>(height / 2, 1);
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

    // Scale coordinates to the downsampled resolution (0.5x)
    x0 *= 0.5f; y0 *= 0.5f;
    x1 *= 0.5f; y1 *= 0.5f;
    x2 *= 0.5f; y2 *= 0.5f;

    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (std::fabs(area) < 1e-6f) {
        return;
    }

    // Ensure winding is counter-clockwise (area > 0)
    if (area < 0.0f) {
        std::swap(x1, x2);
        std::swap(y1, y2);
        std::swap(z1, z2);
        area = -area;
    }
    const float inv_area = 1.0f / area;

    const int min_x = clamp_int(static_cast<int>(std::floor(std::min({x0, x1, x2}))), 0, width_ - 1);
    const int max_x = clamp_int(static_cast<int>(std::ceil(std::max({x0, x1, x2}))), 0, width_ - 1);
    const int min_y = clamp_int(static_cast<int>(std::floor(std::min({y0, y1, y2}))), 0, height_ - 1);
    const int max_y = clamp_int(static_cast<int>(std::ceil(std::max({y0, y1, y2}))), 0, height_ - 1);

    const float A0 = y1 - y2;
    const float B0 = x2 - x1;
    const float C0 = x1 * y2 - x2 * y1;

    const float A1 = y2 - y0;
    const float B1 = x0 - x2;
    const float C1 = x2 * y0 - x0 * y2;

    const float A2 = y0 - y1;
    const float B2 = x1 - x0;
    const float C2 = x0 * y1 - x1 * y0;

    const float start_x = static_cast<float>(min_x) + 0.5f;
    const float start_y = static_cast<float>(min_y) + 0.5f;

    float E0_row = A0 * start_x + B0 * start_y + C0;
    float E1_row = A1 * start_x + B1 * start_y + C1;
    float E2_row = A2 * start_x + B2 * start_y + C2;

    for (int y = min_y; y <= max_y; y++) {
        float E0 = E0_row;
        float E1 = E1_row;
        float E2 = E2_row;
        const size_t row_offset = static_cast<size_t>(y) * width_;
        for (int x = min_x; x <= max_x; x++) {
            if (E0 >= 0.0f && E1 >= 0.0f && E2 >= 0.0f) {
                const float w0 = E0 * inv_area;
                const float w1 = E1 * inv_area;
                const float w2 = 1.0f - w0 - w1;
                const float z = w0 * z0 + w1 * z1 + w2 * z2;
                float& stored = pixels_[row_offset + x];
                if (z > stored) {
                    stored = z;
                }
            }
            E0 += A0;
            E1 += A1;
            E2 += A2;
        }
        E0_row += B0;
        E1_row += B1;
        E2_row += B2;
    }
}

bool Buffer::is_occluded(float x, float y, float depth) const {
    if (pixels_.empty()) {
        return false;
    }

    const float sx = x * 0.5f;
    const float sy = y * 0.5f;

    // KOS GCC 9 exposes lround in <math.h> but not in namespace std.
    const int px = static_cast<int>(lround(sx));
    const int py = static_cast<int>(lround(sy));
    if (px < 0 || py < 0 || px >= width_ || py >= height_) {
        return true;
    }

    const float stored = pixels_[pixel_index(px, py)];
    // A small epsilon avoids flicker when the test point shares depth with the surface.
    return depth < stored - 1e-4f;
}

} // namespace dreamcast::zbuf

#endif // DREAMCAST
