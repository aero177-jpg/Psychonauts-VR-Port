#pragma once
// Psychonauts' camera maths for stereo (docs/shaders.md).
//
// Every 3D vertex shader does `m4x4 oPos, v0, c6`: o.i = dot(v, c[6 + i]).
// So, as a row-vector matrix W (clip = v * W), register c[6 + i] is column i
// of W. W = World * View * Proj, and that is all the shader gets.
//
// Proj is a D3D left-handed perspective (row-vector):
//     [sx 0  0   0]
//     [0  sy 0   0]
//     [0  0  q   1]
//     [0  0  q32 0]      q32 = -q * near
// so, with A = World * View (its upper 3x3 rows a rotation times a uniform
// scale for ordinary objects):
//     c6.xyz = sx * A.col0, c7.xyz = sy * A.col1, c9.xyz = A.col2
//     c8.xyz = q * c9.xyz,  c8.w = q * c9.w + q32
// which gives sx, sy, q and near from W alone. The scales need A's columns
// to be orthogonal and of equal length (no non-uniform world scale); q and
// near do not.

#include "vr_math.h"

#include <cmath>

namespace psyvr
{
    struct PsyProjection
    {
        float sx = 0, sy = 0;   // x, y scales (cot of half fov)
        float q = 0, q32 = 0;   // depth terms
        float near_z = 0, far_z = 0;
    };

    // Registers c6..c9 as uploaded -> the row-vector matrix W.
    inline Mat4 psy_wvp_from_regs(const float r[4][4])
    {
        Mat4 w;
        for (int i = 0; i < 4; ++i)
            for (int k = 0; k < 4; ++k)
                w.m[k][i] = r[i][k];
        return w;
    }

    inline void psy_regs_from_wvp(const Mat4& w, float r[4][4])
    {
        for (int i = 0; i < 4; ++i)
            for (int k = 0; k < 4; ++k)
                r[i][k] = w.m[k][i];
    }

    inline float psy_dot3(const float* a, const float* b)
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    // Orthographic (2D, HUD): w does not depend on the position.
    inline bool psy_is_orthographic(const float r[4][4])
    {
        const float* t = r[3];
        const float len = sqrtf(psy_dot3(t, t));
        return len < 1e-6f * (1.0f + fabsf(t[3]));
    }

    // Depth terms only (robust to any world matrix). False if not perspective.
    inline bool psy_depth_terms(const float r[4][4], PsyProjection* p)
    {
        const float* c8 = r[2];
        const float* c9 = r[3];
        const float tt = psy_dot3(c9, c9);
        if (!(tt > 1e-20f))
            return false;
        const float q = psy_dot3(c8, c9) / tt;
        const float q32 = c8[3] - q * c9[3];
        // q > 1: a far plane; q <= 1 (the main scene, q 0.99): depth never
        // reaches 1, an effectively infinite far plane.
        if (!(q > 0.0f) || !(q32 < 0.0f))
            return false;
        p->q = q;
        p->q32 = q32;
        p->near_z = -q32 / q;
        p->far_z = q > 1.0001f ? q * p->near_z / (q - 1.0f) : 1e30f;
        return std::isfinite(p->near_z) && p->near_z > 0.0f && p->far_z > p->near_z;
    }

    // Full estimate. `clean` = the world part looked rigid with uniform
    // scale, so sx and sy are trustworthy (use them for other draws).
    inline bool psy_projection_from_regs(const float r[4][4], PsyProjection* p, bool* clean)
    {
        *clean = false;
        if (!psy_depth_terms(r, p))
            return false;
        const float* c6 = r[0];
        const float* c7 = r[1];
        const float* c9 = r[3];
        const float l6 = sqrtf(psy_dot3(c6, c6)), l7 = sqrtf(psy_dot3(c7, c7));
        const float l9 = sqrtf(psy_dot3(c9, c9));
        if (!(l6 > 0) || !(l7 > 0) || !(l9 > 0))
            return false;
        p->sx = l6 / l9;
        p->sy = l7 / l9;
        const float tol = 2e-3f;
        *clean = fabsf(psy_dot3(c6, c9)) < tol * l6 * l9 && fabsf(psy_dot3(c7, c9)) < tol * l7 * l9 &&
                 fabsf(psy_dot3(c6, c7)) < tol * l6 * l7;
        return true;
    }

    inline Mat4 psy_projection_matrix(const PsyProjection& p)
    {
        Mat4 m = {};
        m.m[0][0] = p.sx;
        m.m[1][1] = p.sy;
        m.m[2][2] = p.q;
        m.m[2][3] = 1.0f;
        m.m[3][2] = p.q32;
        return m;
    }

    // Inverse of the projection above (closed form).
    inline Mat4 psy_projection_inverse(const PsyProjection& p)
    {
        Mat4 m = {};
        m.m[0][0] = 1.0f / p.sx;
        m.m[1][1] = 1.0f / p.sy;
        m.m[3][2] = 1.0f;
        m.m[2][3] = 1.0f / p.q32;
        m.m[3][3] = -p.q / p.q32;
        return m;
    }

    // Near limit (start menu): the eyes sit `forward` game units ahead of the
    // game camera; a draw whose centre is `depth` ahead of the camera (its
    // WVP's c9.w) would come nearer than `limit`. Returns how far (<= 0,
    // along the view) to move that draw's eyes so its centre stays `limit`
    // away. 0 when off (limit <= 0), when the centre is not ahead of the
    // camera (skies, drawn round it), or when it is far enough already.
    inline float psy_near_limit_shift(float forward, float limit, float depth)
    {
        if (!(limit > 0.0f) || !(depth > 0.0f))
            return 0.0f;
        const float allowed = depth - limit;    // the furthest forward the eyes may sit
        return allowed < forward ? allowed - forward : 0.0f;
    }
}
