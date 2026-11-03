#pragma once
// World points to the screen for a gn_view (the camera as drawn this frame): the math the layout
// editor draws with, header-only for mods and host tests. Frostbite is y up; the view's forward is the
// way the camera looks.

#include <cmath>

#include "gridnode/api.h"

namespace gn::projection {

constexpr float kNear = 0.05f; // metres in front of the camera: nearer points are cut

inline float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Distance in front of the camera along its forward (negative: behind it).
inline float depth(const gn_view& v, const float* p) {
    const float q[3] = {p[0] - v.position[0], p[1] - v.position[1], p[2] - v.position[2]};
    return dot3(q, v.forward);
}

// The screen point (pixels, y down) of a world point; false when it is behind the near plane (out is
// then left alone). A point off the sides of the screen still projects (outside 0..width).
inline bool toScreen(const gn_view& v, const float* p, float* out) {
    const float q[3] = {p[0] - v.position[0], p[1] - v.position[1], p[2] - v.position[2]};
    const float z = dot3(q, v.forward);
    if (z < kNear || v.height < 1.f) return false;
    const float th = std::tan(v.fov_y * 3.14159265f / 360.f), aspect = v.width / v.height;
    const float x = dot3(q, v.right) / (z * th * aspect), y = dot3(q, v.up) / (z * th);
    out[0] = (x + 1.f) * 0.5f * v.width;
    out[1] = (1.f - y) * 0.5f * v.height;
    return true;
}

// The segment a-b cut at the near plane, as two screen points; false when all of it is behind.
inline bool segmentToScreen(const gn_view& v, const float* a, const float* b, float* outA, float* outB) {
    float p[3] = {a[0], a[1], a[2]}, q[3] = {b[0], b[1], b[2]};
    const float za = depth(v, p), zb = depth(v, q);
    if (za < kNear && zb < kNear) return false;
    if (za < kNear || zb < kNear) {
        const float t = (kNear * 1.01f - za) / (zb - za); // just in front, so rounding keeps it projectable
        float* cut = za < kNear ? p : q;
        for (int k = 0; k < 3; ++k) cut[k] = a[k] + (b[k] - a[k]) * t;
    }
    return toScreen(v, p, outA) && toScreen(v, q, outB);
}

} // namespace gn::projection
