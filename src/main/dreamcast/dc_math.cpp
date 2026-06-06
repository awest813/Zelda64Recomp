#ifdef DREAMCAST

#include "dc_math.h"

#include <cmath>
#include <cstring>

#if defined(DC_HAS_SH4ZAM)
#include <sh4zam/shz_matrix.h>
#endif

namespace dreamcast::math {

void mat4_identity(float m[4][4]) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            m[i][j] = (i == j) ? 1.0f : 0.0f;
        }
    }
}

void mat4_mul(const float a[4][4], const float b[4][4], float out[4][4]) {
#if defined(DC_HAS_SH4ZAM)
    shz_mat4x4_t lhs{};
    shz_mat4x4_t rhs{};
    shz_mat4x4_t result{};
    std::memcpy(&lhs, a, sizeof(lhs));
    std::memcpy(&rhs, b, sizeof(rhs));
    shz_mat4x4_init_mul(&result, &lhs, &rhs);
    std::memcpy(out, &result, sizeof(result));
#else
    float temp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            temp[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    std::memcpy(out, temp, sizeof(temp));
#endif
}

void mat4_transform(const float m[4][4], float x, float y, float z, float& ox, float& oy, float& oz, float& ow) {
#if defined(DC_HAS_SH4ZAM)
    shz_vec3_t in{x, y, z};
    shz_vec3_t out{};
    shz_mat4x4_t matrix{};
    std::memcpy(&matrix, m, sizeof(matrix));
    shz_mat4x4_transform_vec3(&out, &matrix, &in);
    ox = out.x;
    oy = out.y;
    oz = out.z;
    ow = m[3][0] * x + m[3][1] * y + m[3][2] * z + m[3][3];
#else
    ox = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
    oy = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
    oz = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
    ow = m[3][0] * x + m[3][1] * y + m[3][2] * z + m[3][3];
#endif
}

} // namespace dreamcast::math

#endif // DREAMCAST
