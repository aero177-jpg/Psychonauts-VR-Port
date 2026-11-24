#pragma once
// Theatre mode (after the Underworld/Legend mods' theatre): the game as in the
// flat game, on a screen floating in a dark room, in stereo.
//
// A 3D screen, not a hole in the wall (docs/findings.md, 2026-11-16). The
// eyes are not the head: while the theatre shows, each eye renders the game
// from the game camera itself, moved only sideways by half the eyes'
// separation, with the game's own vertical field of view (cutscene lenses
// included) at the screen's shape. No head pose, horizon lock or god seat
// goes in, so the picture is the game's own framing. At Present each eye's
// image is laid on a quad fixed in the room (hud.cpp), seen with the real
// head pose: the screen, its picture and the HUD on it stay where they are
// however the head moves; the depth is the eyes' real disparity.
//
// The images are made so that something at infinity lands on the screen one
// eye separation apart (each eye's image shifted sideways by
// (eye x) / (screen half width), in NDC), so it looks infinitely far, and
// with the eyes' real separation in game units ([vr] world_scale) a point at
// screen distance x world scale lands on the screen with no disparity: when
// the screen looks as wide as the game's field of view, depth is life size.
//
// The window is placed `distance_m` ahead of the head (level, at head height)
// when the theatre opens or at a recentre, `width_deg` wide as seen from there,
// `aspect` = width/height.
//
// Pure maths: no config(), no Windows calls (mathtest links it).

#include "vr_math.h"

#include <cmath>

namespace psyvr
{
    struct TheatreWindowSize
    {
        float distance = 2.5f;      // metres
        float half_w = 1.0f;        // metres
        float half_h = 0.56f;       // metres
    };

    inline TheatreWindowSize theatre_window_size(float distance_m, float width_deg, float aspect)
    {
        TheatreWindowSize w;
        w.distance = distance_m;
        w.half_w = distance_m * tanf(width_deg * 0.5f * 0.01745329f);
        w.half_h = aspect > 0.1f ? w.half_w / aspect : w.half_w;
        return w;
    }

    // When the theatre shows. [theatre] mode: 0 = never, 1 = cutscenes (the
    // game draws with its script camera; not in the start menu, which uses
    // one too), 2 = always.
    inline bool theatre_wanted(int mode, bool cutscene_camera, bool start_menu)
    {
        if (mode >= 2)
            return true;
        return mode == 1 && cutscene_camera && !start_menu;
    }

    // [theatre] style. Screen: the 3D screen above. Window: a window you can
    // look into (as the Underworld mod's theatre with fit_fov): the real eyes,
    // placed relative to the anchor (where the head was when the window was
    // placed, `distance` straight back from its centre), and the game camera
    // at the anchor looking through the window. The world is warped about
    // the game camera (theatre_fit_warp) so the window shows the game's whole
    // shot from the anchor; leaning and turning look into and round it. Each
    // eye's picture is cut to the window's outline at Present (hud.cpp).
    enum TheatreStyle { kTheatreScreen = 0, kTheatreWindow = 1 };

    // The eyes while the theatre shows, from the shim to the stereo
    // (logic_abi.h).
    //  Screen: x_units: each eye's sideways place from the game camera, game
    //  units (D3D view space, + = right); ndc_shift: each eye's image shift
    //  (NDC x); aspect: the screen's width / height.
    //  Window: eye_to_view: each eye -> game view space (rotation, and the
    //  translation in game units), the anchor being the game camera; fit_s:
    //  the warp's scale (theatre_fit_scale).
    struct TheatreEyes
    {
        int style;
        float x_units[2];
        float ndc_shift[2];
        float aspect;
        Mat4 eye_to_view[2];
        float fit_s;
    };

    // Window: the game's frame (vertical scale game_sy, 1 / tan of half its
    // vertical view) fitted to the window's height (half_h_m at distance_m
    // from the anchor): x and y about the game camera are scaled by
    // s = tan(window) / tan(game), clamped to 0.1 .. 10 (1 for bad input).
    inline float theatre_fit_scale(float half_h_m, float distance_m, float game_sy)
    {
        if (!(distance_m > 1e-3f) || !(game_sy > 1e-3f) || !(half_h_m > 0.0f))
            return 1.0f;
        float s = half_h_m / distance_m * game_sy;
        if (s < 0.1f) s = 0.1f;
        if (s > 10.0f) s = 10.0f;
        return s;
    }

    // The warp in game view space (row vectors): diag(s, s, 1, 1). Depth
    // along the game camera's view is kept, so something at
    // distance x world scale still lies on the window.
    inline Mat4 theatre_fit_warp(float s)
    {
        Mat4 m{};
        m.m[0][0] = s;
        m.m[1][1] = s;
        m.m[2][2] = 1.0f;
        m.m[3][3] = 1.0f;
        return m;
    }

    // eye_x_m: each eye's sideways offset from the head, metres (+ = right);
    // depth: [theatre] depth, the eyes' separation in the game times this.
    // 1 = life size; 0 = flat on the screen; below 1 infinity comes nearer
    // too (the shift is scaled with it), above 1 it stays at infinity (more
    // shift would make the eyes diverge) and near things get deeper.
    inline TheatreEyes theatre_eyes(const float eye_x_m[2], float world_scale, float depth,
                                    float half_w_m, float aspect)
    {
        TheatreEyes t{};
        const float k = depth > 0.0f ? depth : 0.0f;
        const float shift_k = k < 1.0f ? k : 1.0f;
        for (int e = 0; e < 2; ++e)
        {
            t.x_units[e] = eye_x_m[e] * world_scale * k;
            t.ndc_shift[e] = half_w_m > 1e-3f ? eye_x_m[e] * shift_k / half_w_m : 0.0f;
            if (t.ndc_shift[e] > 0.5f) t.ndc_shift[e] = 0.5f;
            if (t.ndc_shift[e] < -0.5f) t.ndc_shift[e] = -0.5f;
        }
        t.aspect = aspect > 0.1f ? aspect : 1.78f;
        return t;
    }

    // An eye's projection (D3D, left-handed, row vectors): the game's
    // vertical scale sy (1 / tan of half its vertical field of view) at the
    // screen's aspect, the image shifted by ndc_shift, and the game's own
    // depth terms (q = m[2][2], q32 = m[3][2]) so the depth buffer matches.
    inline Mat4 theatre_projection(float sy, float aspect, float ndc_shift, float q, float q32)
    {
        Mat4 p{};
        p.m[0][0] = sy / (aspect > 0.1f ? aspect : 1.78f);
        p.m[1][1] = sy;
        p.m[2][0] = ndc_shift;      // NDC x = x / z * m00 + m20
        p.m[2][2] = q;
        p.m[2][3] = 1.0f;
        p.m[3][2] = q32;
        return p;
    }
}
