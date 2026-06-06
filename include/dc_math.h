#ifndef __DC_MATH_H__
#define __DC_MATH_H__

#ifdef DREAMCAST

// SH-4 optimized matrix math (sh4zam when available, portable C++ fallback).
namespace dreamcast::math {

void mat4_identity(float m[4][4]);
void mat4_mul(const float a[4][4], const float b[4][4], float out[4][4]);
void mat4_transform(const float m[4][4], float x, float y, float z, float& ox, float& oy, float& oz, float& ow);

} // namespace dreamcast::math

#endif // DREAMCAST

#endif // __DC_MATH_H__
