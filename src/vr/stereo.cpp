#include "stereo.h"
#include "psy_stereo_math.h"
#include "psy_shader_scan.h"
#include "psy_theatre.h"
#include "vr_session.h"

#include "../common/config.h"
#include "../common/log.h"
#include "../proxy/game_psy.h"

#include <windows.h>

// fxc output (shaders/water_ps.hlsl) and the game's originals.
#include "shaders/water_ps.h"
#include "shaders/cheapwater_ps.h"
#include "shaders/water_ps_game.h"

#include <cmath>
#include <cstring>
#include <unordered_map>

// Same-frame side-by-side stereo for Psychonauts (docs/shaders.md).
//
// The back buffer holds both eyes side by side; the game draws over all of it
// and each per-eye draw gets its half through the viewport.
//
// Classification per draw, from what the vertex shader reads (scanned from
// its bytecode when created; the game's shaders are hand-written assembly
// with no constant table):
//   * reads c6 (the WVP every 3D shader ends with):
//       - orthographic WVP: 2D (HUD, menus, full-screen images)
//       - perspective: drawn per eye with WVP * M_eye, where
//         M_eye = P_game^-1 * (game view -> eye view) * P_eye.
//         P_game is recovered from the WVP itself (psy_stereo_math.h).
//       - particle billboards (also read c10 and c14: clip-space corner
//         offsets) get c10/c14 multiplied by M_eye too.
//   * no c6 (Sprite.vsh: screen position from c10/c11), or no vertex shader:
//     2D, or a screen-space pass when a scene copy is bound.
// Targets of any other shape (shadow maps, reflections, small effects) are
// drawn as the game drew them.

namespace psyvr
{
    namespace
    {
        unsigned g_back_w = 0, g_back_h = 0;

        // Per vertex shader: what it reads (psy_shader_scan.h).
        SRWLOCK g_shader_lock = SRWLOCK_INIT;
        std::unordered_map<const void*, PsyShaderInfo> g_shader_info;
        PsyShaderInfo g_vs;
        bool g_have_vs = false;
        const void* g_vs_ptr = nullptr;

        PsyShaderInfo scan_shader(const DWORD* code)
        {
            static_assert(sizeof(DWORD) == sizeof(uint32_t), "token size");
            return psy_scan_shader(reinterpret_cast<const uint32_t*>(code));
        }
        // The game's c6..c9 and c0..c63 as last uploaded.
        float g_wvp[4][4] = {};
        float g_c[64][4] = {};

        bool g_rt_stereo = false;
        unsigned g_rt_w = 0, g_rt_h = 0;

        IDirect3DSurface9* g_rt_ptr = nullptr;
        // The texture g_rt_ptr belongs to (null = not a texture's level: the
        // back buffer or a plain render target). Only compared, never used.
        const void* g_rt_texture = nullptr;
        // The texture on stage 0 (only compared).
        const void* g_tex0_ptr = nullptr;
        // The last planned draw was the Sprite.vsh triangle over the whole
        // target (set in plan_game_draw).
        bool g_fullscreen_sprite = false;
        bool g_blend = false;

        // Fading objects ([graphics] fast_fades; docs/findings.md
        // 2026-11-16). The game draws a thing that is fading in or out (by
        // distance and size on screen, the entity fade at 0x43E4B0) through
        // a back-buffer-sized target of its own: a full-screen Sprite.vsh
        // triangle clears it to (0, 0, 0, 0) (c2 = 0, no texture, no blend),
        // the object is drawn into it opaque, and a full-screen triangle
        // lays the target over the back buffer with the fade in c2.w (src
        // alpha / inverse src alpha). Outside the object the target is 0, so
        // the composite changes nothing there, but each one is two passes
        // over the whole headset frame: 43 such objects in `gpc` cost ~13 ms
        // a frame at Quest size on a GTX 1070. The clear becomes a Clear
        // (plan 6), the object is also marked in a stencil bit of the back
        // buffer's depth-stencil (plan 7) and the composite is drawn only
        // there (plan 8): the same picture, paid for the object's pixels.
        IDirect3DSurface9* g_fade_rt = nullptr;     // the cleared target; null = none open
        const void* g_fade_tex = nullptr;           // its texture
        bool g_fade_ok = false;                     // every draw into it was marked
        unsigned g_fade_marked = 0;                 // draws marked since the clear
        unsigned g_fade_count = 0;                  // composites shortened, for the log
        // Where the objects drawn since the clear are on the back buffer
        // (pixels, both eyes), from their vertex boxes; g_fade_rect_all =
        // one could not be placed (the composite then covers everything).
        // The stencil keeps the picture exact; the scissor only stops the
        // composite paying for the whole frame (~0.1 ms each at Quest size).
        float g_fade_rect[4] = {};
        bool g_fade_rect_any = false, g_fade_rect_all = false;
        // Set for the composite's stereo_before_single_draw.
        bool g_fade_scissor = false;
        RECT g_fade_scissor_rect{};
        // The last draw's vertex box (stereo_note_vertex_box) for the fade.
        bool g_vbox_ok = false;
        float g_vbox[6] = {};
        // The stereo targets drawn into so far this frame. A skipped
        // full-screen pass ([debug] skip_fullscreen_passes) that would have
        // been the first, opaque, draw into one is not skipped: it is what
        // replaces last frame's image (the item and pause menus draw the
        // frozen scene that way), and without it the eyes kept every frame.
        IDirect3DSurface9* g_drawn_targets[8] = {};
        unsigned g_drawn_count = 0;
        bool first_draw_into_target()
        {
            for (unsigned i = 0; i < g_drawn_count; ++i)
                if (g_drawn_targets[i] == g_rt_ptr)
                    return false;
            if (g_drawn_count < 8)
                g_drawn_targets[g_drawn_count++] = g_rt_ptr;
            return true;
        }
        D3DVIEWPORT9 g_vp{ 0, 0, 0, 0, 0.0f, 1.0f };
        unsigned g_scene_copy_stages = 0;
        // Any render-target texture bound (any size): a 2D draw sampling one
        // is a screen effect (glow, distortion), not HUD art.
        unsigned g_rt_tex_stages = 0;
        bool g_tex0_is_target = false;

        // The projection's x/y scales from the last clean estimate.
        PsyProjection g_proj;
        bool g_have_scales = false;
        unsigned g_clean_this_frame = 0;

        // This draw, per eye.
        Mat4 g_eye_m[2];
        Mat4 g_eye_view_from_game[2];
        Mat4 g_eye_proj[2];
        PsyProjection g_draw_proj;

        enum Kind { KindMono, KindCamera, KindScreen, Kind2D, KindReticle, KindTint, KindTintOff, KindPicture };
        Kind g_kind = KindMono;
        bool g_patched_wvp = false;
        uint64_t g_patched_bb = 0;      // billboard registers holding an eye's values

        unsigned g_counts[8] = {};
        unsigned g_frame_index = 0;
        bool g_frame_scene = false, g_last_frame_scene = false;
        bool g_announced = false;

        IDirect3DSurface9* g_main_rt = nullptr;
        Mat4 g_main_eye_proj[2];
        Mat4 g_main_view_to_eye[2];     // game view -> eye view, as the main pass drew
        D3DVIEWPORT9 g_main_vp{};
        unsigned g_main_frame = ~0u;

        // The lock-on reticle ([hud] lock_reticle; docs/findings.md
        // 2026-11-15): a 2D draw with a 64 x 128 DXT5 texture on stage 0,
        // Sprite-style shader (screen position = v0 * c10 + c11, v0 in the
        // game's 640 x 480 2D units). While locked on it is drawn in each
        // eye at the target instead of on the HUD screen: c10 / c11 per eye
        // put the sprite's centre on the target's position in that eye, at
        // the HUD's angular scale.
        unsigned g_tex0_w = 0, g_tex0_h = 0;
        D3DFORMAT g_tex0_fmt = D3DFMT_UNKNOWN;
        bool g_box_ok = false;
        float g_box[4] = {};
        bool g_reticle_eye_ok[2] = {};
        float g_reticle_c[2][2][4] = {};    // [eye][c10, c11]
        bool g_reticle_was_on = false;
        bool g_reticle_this_frame = false;

        bool reticle_candidate()
        {
            return config().hud_lock_reticle && g_rt_stereo && g_rt_w == g_back_w && g_have_vs &&
                   !g_vs.reads_wvp && g_vs.reads_c10 && g_tex0_w == 64 && g_tex0_h == 128 &&
                   g_tex0_fmt == (D3DFORMAT)MAKEFOURCC('D', 'X', 'T', '5') &&
                   g_main_frame == g_frame_index;
        }

        // For a reticle draw whose vertex box was read: the target, and each
        // eye's c10 / c11. False = not locked on (the draw stays as it was).
        bool plan_reticle()
        {
            float pos[3], view[4][4];
            if (!g_box_ok || !game_lock_target(pos, view))
                return false;
            g_reticle_this_frame = true;
            const float h = config().hud_lock_reticle_height * config().world_scale;
            const float w[4] = { pos[0], pos[1] + h, pos[2], 1.0f };
            float v[4];
            for (int c = 0; c < 4; ++c)
                v[c] = w[0] * view[0][c] + w[1] * view[1][c] + w[2] * view[2][c] + w[3] * view[3][c];
            // The camera's view (+0x50) is right-handed (looks down -z: the
            // game builds its projection with D3DXMatrixPerspectiveFovRH);
            // the eye matrices here look down +z.
            v[2] = -v[2];
            const float centre_x = 0.5f * (g_box[0] + g_box[2]);
            const float centre_y = 0.5f * (g_box[1] + g_box[3]);
            // The HUD screen shows the middle 640 units of the 2D across
            // [hud] width_degrees: the same angle per unit here.
            const float unit = tanf(config().hud_width_degrees / 640.0f * 0.01745329f) *
                               config().hud_lock_reticle_scale;
            for (int eye = 0; eye < 2; ++eye)
            {
                const Mat4& to_eye = g_main_view_to_eye[eye];
                const Mat4& proj = g_main_eye_proj[eye];
                float e[4], clip[4];
                for (int c = 0; c < 4; ++c)
                    e[c] = v[0] * to_eye.m[0][c] + v[1] * to_eye.m[1][c] + v[2] * to_eye.m[2][c] +
                           v[3] * to_eye.m[3][c];
                for (int c = 0; c < 4; ++c)
                    clip[c] = e[0] * proj.m[0][c] + e[1] * proj.m[1][c] + e[2] * proj.m[2][c] +
                              e[3] * proj.m[3][c];
                g_reticle_eye_ok[eye] = clip[3] > 1e-3f;
                // Behind the eye: moved far off screen (the draw still runs).
                const float nx = g_reticle_eye_ok[eye] ? clip[0] / clip[3] : 100.0f;
                const float ny = g_reticle_eye_ok[eye] ? clip[1] / clip[3] : 100.0f;
                const float kx = fabsf(proj.m[0][0]) * unit;
                const float ky = fabsf(proj.m[1][1]) * unit;
                float* c10 = g_reticle_c[eye][0];
                float* c11 = g_reticle_c[eye][1];
                // 2D y runs down the screen, clip y up.
                c10[0] = kx;
                c10[1] = -ky;
                c10[2] = g_c[10][2];
                c10[3] = g_c[10][3];
                c11[0] = nx - centre_x * kx;
                c11[1] = ny + centre_y * ky;
                c11[2] = g_c[11][2];
                c11[3] = g_c[11][3];
            }
            if (!g_reticle_was_on)
            {
                g_reticle_was_on = true;
                static unsigned told = 0;
                if (told++ < 10)
                    log("stereo: lock-on reticle in 3D: target %.0f %.0f %.0f, sprite centre %.1f %.1f, "
                        "eyes %d %d",
                        pos[0], pos[1], pos[2], centre_x, centre_y, g_reticle_eye_ok[0] ? 1 : 0,
                        g_reticle_eye_ok[1] ? 1 : 0);
            }
            return true;
        }

        // Full-screen tints (docs/findings.md 2026-11-16): Sprite.vsh quads
        // over exactly the game's 2D screen with a tiny texture on stage 0,
        // coloured by the vertex colours: fades to black (1 x 1 L8, 20..40
        // frames at an area change or a load) and colour flashes (the ground
        // pound's orange: 32 x 32 DXT1, about 20 frames). On the HUD screen
        // they were a floating panel; [hud] screen_fades / screen_flashes
        // send them over the whole view or leave them out. HUD art is never
        // that small and full-screen; the 512 x 512 overlay drawn after a
        // load (bigger than the screen, pulsing) stays on the HUD.
        enum Tint { TintNone, TintFade, TintFlash };
        bool g_tint_box_ok = false;

        // A tint's texture is a small one stretched over the screen: 1 x 1
        // for fades, 32 x 32 DXT1 (FXTextures/orangeblock) for the ground
        // pound's flash. Texture packs enlarge those (Shibanauts HD makes
        // orangeblock 256 x 256, and 507 of the game's textures of 64 x 64
        // or less larger), so with [graphics] texture_pack_compat compressed
        // ones count up to 256. The 512 x 512 A8R8G8B8 overlay after a save
        // load stays out either way (docs/findings.md).
        bool tint_texture_small()
        {
            const bool dxt = g_tex0_fmt == D3DFMT_DXT1 || g_tex0_fmt == D3DFMT_DXT2 || g_tex0_fmt == D3DFMT_DXT3 ||
                             g_tex0_fmt == D3DFMT_DXT4 || g_tex0_fmt == D3DFMT_DXT5;
            const unsigned limit = dxt && config().texture_pack_compat ? 256u : 64u;
            return g_tex0_w >= 1 && g_tex0_w <= limit && g_tex0_h >= 1 && g_tex0_h <= limit;
        }

        bool tint_candidate()
        {
            const Config& c = config();
            return (c.hud_screen_fades != 0 || c.hud_screen_flashes != 0) && g_rt_stereo && g_rt_w == g_back_w &&
                   g_rt_tex_stages == 0 && !g_tex0_is_target && hud_enabled() && g_have_vs && !g_vs.reads_wvp &&
                   g_vs.reads_c10 && tint_texture_small() &&
                   !(fabsf(g_c[10][0]) >= 1.99f && fabsf(g_c[10][1]) >= 1.99f);
        }

        // This draw (its vertex box read): a tint, and which.
        Tint classify_tint()
        {
            if (!g_tint_box_ok || !tint_candidate())
                return TintNone;
            // Screen position = v0 * c10 + c11 (NDC).
            float lo[2], hi[2];
            for (int k = 0; k < 2; ++k)
            {
                const float a = g_box[k] * g_c[10][k] + g_c[11][k];
                const float b = g_box[k + 2] * g_c[10][k] + g_c[11][k];
                lo[k] = a < b ? a : b;
                hi[k] = a < b ? b : a;
            }
            if (lo[0] > -0.98f || lo[1] > -0.98f || hi[0] < 0.98f || hi[1] < 0.98f)
                return TintNone;
            return g_tex0_w == 1 && g_tex0_h == 1 ? TintFade : TintFlash;
        }

        // Test aid: each eye drawn with the game's own matrix (false in builds that ship).
        const bool kPassThrough = false;

        // Per draw: work out the projection and each eye's matrix.
        bool plan_camera()
        {
            PsyProjection p;
            bool clean = false;
            if (!psy_projection_from_regs(g_wvp, &p, &clean))
                return false;
            if (clean)
            {
                if (!g_have_scales || fabsf(p.sy - g_proj.sy) > 1e-3f * g_proj.sy ||
                    fabsf(p.sx - g_proj.sx) > 1e-3f * g_proj.sx)
                {
                    static unsigned told = 0;
                    if (told++ < 8)
                        log("stereo: game projection %.1f x %.1f deg (sx %.4f sy %.4f), near %.3f far %.1f",
                            2.0f * atanf(1.0f / p.sx) * 57.29578f, 2.0f * atanf(1.0f / p.sy) * 57.29578f,
                            p.sx, p.sy, p.near_z, p.far_z);
                }
                g_proj = p;
                g_have_scales = true;
                ++g_clean_this_frame;
            }
            else if (g_have_scales)
            {
                p.sx = g_proj.sx;
                p.sy = g_proj.sy;
            }
            g_draw_proj = p;
            // A sky: drawn round the camera (the model's origin is the camera:
            // no translation in WVP; near 0.1, no far plane). Its eyes get the
            // head's rotation only, no eye offset or head movement, so it is at
            // infinity instead of a dome a few metres away.
            const bool sky = fabsf(g_wvp[3][3]) < 1e-3f && fabsf(g_wvp[0][3]) < 1e-3f && fabsf(g_wvp[1][3]) < 1e-3f;
            const Mat4 p_inv = psy_projection_inverse(p);
            // Start menu near limit: a layer nearer the (zoomed) eyes than the
            // limit gets its eyes moved back, so it is not passed through.
            float forward = 0.0f, near_limit = 0.0f;
            vr_view_forward(&forward, &near_limit);
            const float shift = sky ? 0.0f : psy_near_limit_shift(forward, near_limit, g_wvp[3][3]);
            // Theatre (psy_theatre.h): the game camera's own view, the eyes
            // only moved apart, the game's own vertical field of view at the
            // screen's shape. No head: hud.cpp puts the pictures on a screen
            // in the room.
            TheatreEyes theatre{};
            const bool in_theatre = game_theatre_eyes(&theatre);
            if (in_theatre && theatre.style == kTheatreWindow)
            {
                // Window: the real eyes about the game camera (the anchor),
                // the world warped about it to fit the game's shot to the
                // window, the headset's own lens (hud.cpp cuts the window out).
                const Mat4 warp = theatre_fit_warp(theatre.fit_s);
                for (int eye = 0; eye < 2; ++eye)
                {
                    Mat4 view_to_eye;
                    if (!invert(theatre.eye_to_view[eye], &view_to_eye))
                        return false;
                    view_to_eye = warp * view_to_eye;
                    if (sky)
                        view_to_eye.m[3][0] = view_to_eye.m[3][1] = view_to_eye.m[3][2] = 0.0f;
                    g_eye_view_from_game[eye] = view_to_eye;
                    g_eye_proj[eye] = vr_eye_projection(eye, p.near_z, p.near_z * 1000.0f);
                    g_eye_proj[eye].m[2][2] = p.q;
                    g_eye_proj[eye].m[3][2] = p.q32;
                    g_eye_m[eye] = p_inv * view_to_eye * g_eye_proj[eye];
                }
                return true;
            }
            if (in_theatre)
            {
                for (int eye = 0; eye < 2; ++eye)
                {
                    Mat4 view_to_eye = translation(sky ? 0.0f : -theatre.x_units[eye], 0.0f, 0.0f);
                    g_eye_view_from_game[eye] = view_to_eye;
                    g_eye_proj[eye] = theatre_projection(p.sy, theatre.aspect, theatre.ndc_shift[eye], p.q, p.q32);
                    g_eye_m[eye] = p_inv * view_to_eye * g_eye_proj[eye];
                }
                return true;
            }
            for (int eye = 0; eye < 2; ++eye)
            {
                Mat4 eye_to_view, view_to_eye;
                if (!vr_eye_to_game_view(eye, &eye_to_view))
                    return false;
                eye_to_view.m[3][2] += shift;
                if (!invert(eye_to_view, &view_to_eye))
                    return false;
                if (sky)
                    view_to_eye.m[3][0] = view_to_eye.m[3][1] = view_to_eye.m[3][2] = 0.0f;
                g_eye_view_from_game[eye] = view_to_eye;
                // The eye's field of view with the game's own depth terms, so
                // depth (and the depth buffer) matches the game's exactly.
                g_eye_proj[eye] = vr_eye_projection(eye, p.near_z, p.near_z * 1000.0f);
                g_eye_proj[eye].m[2][2] = p.q;
                g_eye_proj[eye].m[3][2] = p.q32;
                g_eye_m[eye] = p_inv * view_to_eye * g_eye_proj[eye];
                if (kPassThrough)
                    g_eye_m[eye] = Mat4::identity();
            }
            return true;
        }

        void row_times(const float in[4], const Mat4& m, float out[4])
        {
            for (int c = 0; c < 4; ++c)
                out[c] = in[0] * m.m[0][c] + in[1] * m.m[1][c] + in[2] * m.m[2][c] + in[3] * m.m[3][c];
        }
    }

    void stereo_set_layout(unsigned back_width, unsigned back_height)
    {
        g_back_w = back_width;
        g_back_h = back_height;
    }

    bool stereo_active()
    {
        return g_back_w != 0 && g_back_h != 0;
    }

    namespace
    {
        // The frames around a loading screen, in psyvr.log (docs/findings.md
        // 2026-11-24): for each frame its draws other than world geometry
        // (plan, kind, stage-0 texture, target, blend; camera and mono-target
        // draws only counted). The last kFrameRing frames are kept; when the
        // presenting thread changes (a loading screen starts or ends) they
        // are written out, and the next kFrameRing frames after. Equal
        // consecutive lines are not repeated; at most kFrameSumLines lines.
        const unsigned kFrameRing = 90;
        const unsigned kFrameSumLines = 2000;
        const unsigned kFrameSumTokens = 48;
        struct FrameSum
        {
            unsigned frame = 0, draws = 0, world = 0;
            DWORD thread = 0;
            char text[1400] = {};
        };
        FrameSum g_ring[kFrameRing];
        unsigned g_ring_next = 0;
        FrameSum g_fsum;
        size_t g_fsum_len = 0;
        unsigned g_fsum_tokens = 0;
        char g_fsum_last[1400] = {};
        unsigned g_fsum_lines = 0;
        unsigned g_fsum_after = 0;          // frames still to log after a switch
        DWORD g_fsum_thread = 0;

        char kind_letter(int kind)
        {
            switch (kind)
            {
            case KindMono: return 'M';
            case KindCamera: return 'C';
            case KindScreen: return 'S';
            case Kind2D: return '2';
            case KindReticle: return 'R';
            case KindTint: return 'T';
            case KindTintOff: return 't';
            case KindPicture: return 'P';
            }
            return '?';
        }

        void frame_sum_draw(int plan)
        {
            ++g_fsum.draws;
            if (g_kind == KindCamera || g_kind == KindMono)
            {
                ++g_fsum.world;
                return;
            }
            if (g_fsum_tokens >= kFrameSumTokens || g_fsum_len + 64 >= sizeof(g_fsum.text))
                return;
            const int n = sprintf_s(g_fsum.text + g_fsum_len, sizeof(g_fsum.text) - g_fsum_len,
                                    " %d%c:t%ux%u%s>%ux%u%s%s", plan, kind_letter(g_kind), g_tex0_w, g_tex0_h,
                                    g_tex0_is_target ? "rt" : "", g_rt_w, g_rt_h, g_rt_texture ? "tex" : "",
                                    g_blend ? "+b" : "");
            if (n > 0)
            {
                g_fsum_len += (size_t)n;
                ++g_fsum_tokens;
            }
        }

        void frame_sum_log(const FrameSum& f, const char* tag)
        {
            if (f.draws == 0 || g_fsum_lines >= kFrameSumLines || strcmp(f.text, g_fsum_last) == 0)
                return;
            ++g_fsum_lines;
            log("%s frame %u (%u draws, %u world, thread %lu):%s", tag, f.frame, f.draws, f.world, f.thread, f.text);
            strcpy_s(g_fsum_last, f.text);
        }

        void frame_sum_end()
        {
            g_fsum.frame = g_frame_index;
            g_fsum.thread = GetCurrentThreadId();
            if (g_fsum_after)
            {
                --g_fsum_after;
                frame_sum_log(g_fsum, "after");
            }
            g_ring[g_ring_next] = g_fsum;
            g_ring_next = (g_ring_next + 1) % kFrameRing;
            g_fsum = FrameSum{};
            g_fsum_len = 0;
            g_fsum_tokens = 0;
        }

        // At a frame boundary on `thread`: a change of presenting thread
        // writes out the frames before it.
        void frame_sum_thread(DWORD thread)
        {
            if (g_fsum_thread && thread != g_fsum_thread && g_fsum_lines < kFrameSumLines)
            {
                log("frames: presents now come from thread %lu (were %lu); the %u frames before:", thread,
                    g_fsum_thread, kFrameRing);
                g_fsum_last[0] = 0;
                for (unsigned i = 0; i < kFrameRing; ++i)
                    frame_sum_log(g_ring[(g_ring_next + i) % kFrameRing], "before");
                for (FrameSum& f : g_ring)
                    f = FrameSum{};
                g_fsum_after = kFrameRing;
            }
            g_fsum_thread = thread;
        }
    }

    void stereo_new_frame()
    {
        frame_sum_end();
        frame_sum_thread(GetCurrentThreadId());
        ++g_frame_index;
        g_last_frame_scene = g_frame_scene;
        g_frame_scene = false;
        g_clean_this_frame = 0;
        g_drawn_count = 0;
        g_fade_rt = nullptr;
        if (!g_reticle_this_frame && g_reticle_was_on)
        {
            g_reticle_was_on = false;
            static unsigned told = 0;
            if (told++ < 10)
                log("stereo: lock-on reticle in 3D: off");
        }
        g_reticle_this_frame = false;
    }

    bool stereo_last_frame_had_scene()
    {
        return g_last_frame_scene;
    }

    void stereo_note_constants(UINT start, const float* data, UINT count)
    {
        if (!data)
            return;
        for (UINT i = 0; i < count; ++i)
        {
            const UINT reg = start + i;
            const float* v = data + i * 4;
            if (reg >= 6 && reg <= 9)
                memcpy(g_wvp[reg - 6], v, sizeof(float) * 4);
            if (reg < 64)
                memcpy(g_c[reg], v, sizeof(float) * 4);
        }
    }

    void stereo_note_ps_constants(UINT, const float*, UINT) {}

    void stereo_note_render_target(IDirect3DSurface9* rt0)
    {
        D3DSURFACE_DESC d{};
        g_rt_stereo = false;
        if (rt0 && SUCCEEDED(rt0->GetDesc(&d)))
        {
            g_rt_stereo = stereo_active() && d.Width >= 2 && d.Width <= g_back_w &&
                          (unsigned long long)d.Width * g_back_h ==
                              (unsigned long long)d.Height * g_back_w;
            g_rt_w = d.Width;
            g_rt_h = d.Height;
            g_rt_ptr = rt0;
            g_rt_texture = nullptr;
            IDirect3DTexture9* tex = nullptr;
            if (SUCCEEDED(rt0->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&tex))) && tex)
            {
                g_rt_texture = tex;
                tex->Release();
            }
            g_vp = { 0, 0, d.Width, d.Height, 0.0f, 1.0f };
        }
    }

    void stereo_note_viewport(const D3DVIEWPORT9& vp)
    {
        g_vp = vp;
    }

    void stereo_note_alpha_blend(bool enabled)
    {
        g_blend = enabled;
    }

    void stereo_note_vertex_shader_created(IDirect3DVertexShader9* shader, const DWORD* bytecode, bool patched)
    {
        if (!shader)
            return;
        // `bytecode` is the game's; the only patch is psy_patch_screen_coords.
        PsyShaderInfo info = scan_shader(bytecode);
        if (patched)
            info.reads_game_wvp = true;
        AcquireSRWLockExclusive(&g_shader_lock);
        g_shader_info[shader] = info;
        ReleaseSRWLockExclusive(&g_shader_lock);
    }

    namespace
    {
        DWORD g_bytecode[16384];
    }

    void stereo_note_vertex_shader(IDirect3DVertexShader9* shader)
    {
        g_have_vs = shader != nullptr;
        g_vs_ptr = shader;
        g_vs = PsyShaderInfo{};
        if (!shader)
            return;
        AcquireSRWLockShared(&g_shader_lock);
        const auto it = g_shader_info.find(shader);
        const bool known = it != g_shader_info.end();
        if (known)
            g_vs = it->second;
        ReleaseSRWLockShared(&g_shader_lock);
        if (!known)
        {
            UINT size = 0;
            if (SUCCEEDED(shader->GetFunction(nullptr, &size)) && size && size <= sizeof(g_bytecode) &&
                SUCCEEDED(shader->GetFunction(g_bytecode, &size)))
            {
                stereo_note_vertex_shader_created(shader, g_bytecode, false);
                g_vs = scan_shader(g_bytecode);
            }
        }
    }

    void stereo_note_texture(DWORD stage, IDirect3DBaseTexture9* texture)
    {
        if (stage >= 16)
            return;
        bool scene_copy = false, is_rt = false;
        if (stage == 0)
        {
            g_tex0_w = g_tex0_h = 0;
            g_tex0_fmt = D3DFMT_UNKNOWN;
            g_tex0_ptr = texture;
        }
        if (texture && texture->GetType() == D3DRTYPE_TEXTURE)
        {
            D3DSURFACE_DESC d{};
            const bool have = SUCCEEDED(static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &d));
            if (have && stage == 0)
            {
                g_tex0_w = d.Width;
                g_tex0_h = d.Height;
                g_tex0_fmt = d.Format;
            }
            is_rt = have && (d.Usage & D3DUSAGE_RENDERTARGET) != 0;
            if (is_rt)
                scene_copy = g_back_w != 0 && d.Width >= 2 && d.Width <= g_back_w &&
                             (unsigned long long)d.Width * g_back_h ==
                                 (unsigned long long)d.Height * g_back_w;
        }
        if (scene_copy)
            g_scene_copy_stages |= 1u << stage;
        else
            g_scene_copy_stages &= ~(1u << stage);
        g_tex0_is_target = g_scene_copy_stages != 0;
        if (is_rt)
            g_rt_tex_stages |= 1u << stage;
        else
            g_rt_tex_stages &= ~(1u << stage);
    }

    const char* stereo_last_draw_kind()
    {
        switch (g_kind)
        {
        case KindCamera: return g_vs.billboard ? "billboard, per eye" : "camera, per eye";
        case KindScreen: return "screen-space, once across both eyes";
        case Kind2D:     return hud_enabled() ? "2D, to the virtual screen" : "2D, copied into each eye";
        case KindReticle: return "lock-on reticle, per eye at the target";
        case KindTint:   return "full-screen tint, over the whole view";
        case KindTintOff: return "full-screen tint, left out";
        case KindPicture: return "full-screen picture, per eye";
        default:         return "mono target";
        }
    }

    // Sprites need no special path here (billboards are handled as camera
    // draws); these stay for the logic ABI.
    namespace
    {
        // Diagnostic (docs/findings.md 2026-11-24): a draw into the back
        // buffer with a texture of 4 x 4 or less that ends up on the HUD
        // screen; the area change's fades (1 x 1) did, unrecognised as tints.
        bool small_texture_2d()
        {
            return g_rt_stereo && g_rt_w == g_back_w && !g_rt_texture && g_tex0_w >= 1 && g_tex0_w <= 4 &&
                   g_tex0_h >= 1 && g_tex0_h <= 4;
        }

        void log_small_texture_2d(int plan)
        {
            static unsigned lines = 0;
            static uint64_t seen[64] = {};
            if (plan != 2 || lines >= 40 || !small_texture_2d())
                return;
            // Where the box lands: through c10 / c11 (Sprite.vsh) and through
            // the WVP (orthographic 2D).
            float c_lo[2] = {}, c_hi[2] = {}, w_lo[2] = {}, w_hi[2] = {};
            if (g_vbox_ok)
                for (int k = 0; k < 2; ++k)
                {
                    const float a = g_vbox[k] * g_c[10][k] + g_c[11][k];
                    const float b = g_vbox[k + 2] * g_c[10][k] + g_c[11][k];
                    c_lo[k] = a < b ? a : b;
                    c_hi[k] = a < b ? b : a;
                    const float pa = g_wvp[k][0] * g_vbox[0] + g_wvp[k][1] * g_vbox[1] + g_wvp[k][3];
                    const float pb = g_wvp[k][0] * g_vbox[2] + g_wvp[k][1] * g_vbox[3] + g_wvp[k][3];
                    w_lo[k] = pa < pb ? pa : pb;
                    w_hi[k] = pa < pb ? pb : pa;
                }
            const uint64_t key = ((uint64_t)(uintptr_t)g_vs_ptr << 20) ^ ((uint64_t)g_tex0_w << 10) ^ g_tex0_h ^
                                 ((uint64_t)(int)(c_lo[0] * 10.0f) << 40) ^ ((uint64_t)(int)(c_hi[1] * 10.0f) << 48);
            for (unsigned i = 0; i < lines; ++i)
                if (seen[i] == key)
                    return;
            seen[lines++] = key;
            log("2d small texture: tex %ux%u fmt %d, vs %p (known %d, wvp %d, c10 %d), blend %d, box %s "
                "[%.1f %.1f .. %.1f %.1f], c10 [%g %g] c11 [%g %g] -> ndc [%.3f %.3f .. %.3f %.3f], "
                "wvp -> [%.3f %.3f .. %.3f %.3f], tint test %d, hud %d",
                g_tex0_w, g_tex0_h, (int)g_tex0_fmt, g_vs_ptr, g_have_vs, g_vs.reads_wvp, g_vs.reads_c10, g_blend,
                g_vbox_ok ? "read" : "not read", g_vbox[0], g_vbox[1], g_vbox[2], g_vbox[3], g_c[10][0], g_c[10][1],
                g_c[11][0], g_c[11][1], c_lo[0], c_lo[1], c_hi[0], c_hi[1], w_lo[0], w_lo[1], w_hi[0], w_hi[1],
                (int)tint_candidate(), (int)hud_enabled());
        }
    }

    bool stereo_wants_vertex_box()
    {
        g_box_ok = false;
        g_tint_box_ok = false;
        g_vbox_ok = false;
        const bool fade = g_fade_rt && g_rt_ptr == g_fade_rt;
        return reticle_candidate() || tint_candidate() || fade || small_texture_2d();
    }

    void stereo_note_vertex_box(const float* box)
    {
        g_box_ok = box != nullptr;
        g_tint_box_ok = box != nullptr;
        g_vbox_ok = box != nullptr;
        if (box)
        {
            memcpy(g_box, box, sizeof(g_box));
            memcpy(g_vbox, box, sizeof(g_vbox));
        }
    }
    // Water's reflection coordinates through the game camera
    // (psy_patch_screen_coords): the reflection texture is rendered once,
    // through it.
    bool stereo_patch_vertex_shader(const DWORD* in, DWORD* out, size_t capacity, size_t* out_len)
    {
        static_assert(sizeof(DWORD) == sizeof(uint32_t), "token size");
        const bool ok = psy_patch_screen_coords(reinterpret_cast<const uint32_t*>(in),
                                                reinterpret_cast<uint32_t*>(out), capacity, out_len);
        if (ok)
        {
            static unsigned told = 0;
            if (told++ < 4)
                log("stereo: water shader patched: reflection looked up through the game camera (c%u..c%u)",
                    kPsyGameWvpReg, kPsyGameWvpReg + 3);
        }
        return ok;
    }

    // The game's Water.fpo / CheapWater.fpo -> shaders/water_ps.hlsl, which
    // read the game camera's clip position from oT4 (psy_patch_screen_coords)
    // and divide it per pixel.
    bool stereo_patch_pixel_shader(const DWORD* in, DWORD* out, size_t capacity, size_t* out_len)
    {
        if (!in)
            return false;
        auto same = [in](const uint32_t* game, size_t n) {
            for (size_t i = 0; i < n; ++i)
                if (in[i] != game[i])
                    return false;
            return true;
        };
        const BYTE* code = nullptr;
        size_t bytes = 0;
        const char* name = nullptr;
        if (same(kGameWaterPs, sizeof(kGameWaterPs) / 4))
        {
            code = kWaterPs;
            bytes = sizeof(kWaterPs);
            name = "Water";
        }
        else if (same(kGameCheapWaterPs, sizeof(kGameCheapWaterPs) / 4))
        {
            code = kCheapWaterPs;
            bytes = sizeof(kCheapWaterPs);
            name = "CheapWater";
        }
        if (!code || bytes % 4 || bytes / 4 > capacity)
            return false;
        memcpy(out, code, bytes);
        *out_len = bytes / 4;
        log("stereo: %s pixel shader replaced: reflection divided per pixel", name);
        return true;
    }

    namespace
    {
        // A patched shader's c56..c59: the game's WVP, as it set c6..c9.
        void set_game_wvp(IDirect3DDevice9* device)
        {
            if (!g_have_vs || !g_vs.reads_game_wvp)
                return;
            float regs[4][4];
            memcpy(regs, g_wvp, sizeof(regs));
            device->SetVertexShaderConstantF(kPsyGameWvpReg, &regs[0][0], 4);
            // The replacement pixel shaders' texbem matrices (stages 1..3)
            // in c28..c30: BUMPENVMAT00, 01, 10, 11.
            float bem[3][4] = {};
            for (DWORD s = 1; s <= 3; ++s)
                for (DWORD k = 0; k < 4; ++k)
                {
                    DWORD v = 0;
                    if (SUCCEEDED(device->GetTextureStageState(s, (D3DTEXTURESTAGESTATETYPE)(D3DTSS_BUMPENVMAT00 + k), &v)))
                        memcpy(&bem[s - 1][k], &v, 4);
                }
            device->SetPixelShaderConstantF(28, &bem[0][0], 3);
        }
    }

    void stereo_before_single_draw(IDirect3DDevice9* device)
    {
        set_game_wvp(device);
        // A fading object's composite (plan 8): only over where the object
        // is. The shim puts the game's scissor state back after the draw.
        if (g_fade_scissor)
        {
            g_fade_scissor = false;
            device->SetScissorRect(&g_fade_scissor_rect);
            device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
        }
    }

    namespace
    {
        int plan_game_draw();

        // Adds where the camera draw just planned lands on the back buffer
        // (both eyes, through each eye's WVP as stereo_begin_eye sets it) to
        // g_fade_rect. False = it cannot be placed: no vertex box, or a corner
        // at or behind the eye.
        bool fade_rect_add()
        {
            if (!g_vbox_ok || g_kind != KindCamera)
                return false;
            const Mat4 game = psy_wvp_from_regs(g_wvp);
            const float half = (float)(g_rt_w / 2);
            for (int eye = 0; eye < 2; ++eye)
            {
                float r[4][4];
                psy_regs_from_wvp(game * g_eye_m[eye], r);
                const float vx = (float)(g_vp.X / 2) + (float)eye * half, vw = (float)(g_vp.Width / 2);
                const float vy = (float)g_vp.Y, vh = (float)g_vp.Height;
                for (int c = 0; c < 8; ++c)
                {
                    const float p[4] = { g_vbox[(c & 1) ? 2 : 0], g_vbox[(c & 2) ? 3 : 1], g_vbox[(c & 4) ? 5 : 4], 1.0f };
                    float clip[4];
                    for (int i = 0; i < 4; ++i)
                        clip[i] = r[i][0] * p[0] + r[i][1] * p[1] + r[i][2] * p[2] + r[i][3] * p[3];
                    if (!(clip[3] > 1e-3f))
                        return false;
                    const float x = vx + (clip[0] / clip[3] * 0.5f + 0.5f) * vw;
                    const float y = vy + (0.5f - clip[1] / clip[3] * 0.5f) * vh;
                    if (!g_fade_rect_any)
                    {
                        g_fade_rect[0] = g_fade_rect[2] = x;
                        g_fade_rect[1] = g_fade_rect[3] = y;
                        g_fade_rect_any = true;
                    }
                    g_fade_rect[0] = x < g_fade_rect[0] ? x : g_fade_rect[0];
                    g_fade_rect[1] = y < g_fade_rect[1] ? y : g_fade_rect[1];
                    g_fade_rect[2] = x > g_fade_rect[2] ? x : g_fade_rect[2];
                    g_fade_rect[3] = y > g_fade_rect[3] ? y : g_fade_rect[3];
                }
            }
            return true;
        }

        // A fading object's cycle (see g_fade_rt), on top of the plan the
        // draw would get anyway.
        int plan_fade(int plan)
        {
            if (!config().fast_fades || config().debug_skip_fullscreen)
            {
                g_fade_rt = nullptr;
                return plan;
            }
            // The clear: a full-screen opaque Sprite.vsh triangle of
            // colour 0, no texture, into a back-buffer-sized texture target.
            if (plan == 0 && g_fullscreen_sprite && g_rt_texture && !g_blend && g_rt_tex_stages == 0 &&
                g_rt_w == g_back_w && g_rt_h == g_back_h && g_c[2][0] == 0.0f && g_c[2][1] == 0.0f &&
                g_c[2][2] == 0.0f && g_c[2][3] == 0.0f)
            {
                g_fade_rt = g_rt_ptr;
                g_fade_tex = g_rt_texture;
                g_fade_ok = true;
                g_fade_marked = 0;
                g_fade_rect_any = g_fade_rect_all = false;
                return 6;
            }
            if (!g_fade_rt)
                return plan;
            if (g_rt_ptr == g_fade_rt)
            {
                // The object (once per eye). Anything else drawn into the
                // target cannot be marked: the composite is then drawn whole.
                if (plan == 1)
                {
                    ++g_fade_marked;
                    if (!fade_rect_add())
                        g_fade_rect_all = true;
                    return 7;
                }
                g_fade_ok = false;
                return plan;
            }
            // The composite: the target laid over the back buffer.
            if (plan == 0 && g_fullscreen_sprite && g_blend && !g_rt_texture && g_tex0_ptr == g_fade_tex)
            {
                const bool ok = g_fade_ok;
                const unsigned marked = g_fade_marked;
                g_fade_rt = nullptr;
                if (!ok)
                    return plan;
                if (marked == 0)
                    return 3;       // nothing drawn: the target is all 0
                if (g_fade_count++ == 0)
                    log("stereo: a fading object drawn only where it is (its target cleared, the "
                        "composite limited by stencil; [graphics] fast_fades)");
                // A margin for the half-pixel offset and rounding; whole
                // back buffer at most.
                g_fade_scissor = g_fade_rect_any && !g_fade_rect_all;
                if (g_fade_scissor)
                {
                    const float m = 4.0f;
                    const float l = g_fade_rect[0] - m, t = g_fade_rect[1] - m;
                    const float rr = g_fade_rect[2] + m, b = g_fade_rect[3] + m;
                    g_fade_scissor_rect.left = l < 0.0f ? 0 : (LONG)l;
                    g_fade_scissor_rect.top = t < 0.0f ? 0 : (LONG)t;
                    g_fade_scissor_rect.right = rr > (float)g_back_w ? (LONG)g_back_w : (LONG)rr + 1;
                    g_fade_scissor_rect.bottom = b > (float)g_back_h ? (LONG)g_back_h : (LONG)b + 1;
                    if (g_fade_scissor_rect.right <= g_fade_scissor_rect.left ||
                        g_fade_scissor_rect.bottom <= g_fade_scissor_rect.top)
                        return 3;   // off screen: nothing to lay over
                }
                return 8;
            }
            return plan;
        }
    }

    int stereo_plan_draw()
    {
        g_fullscreen_sprite = false;
        const int plan = plan_fade(plan_game_draw());
        frame_sum_draw(plan);
        log_small_texture_2d(plan);
        return plan;
    }

    namespace
    {
    int plan_game_draw()
    {
        if (!g_rt_stereo)
            g_kind = KindMono;
        else if (g_have_vs && g_vs.reads_wvp)
        {
            if (psy_is_orthographic(g_wvp))
                g_kind = g_tex0_is_target ? KindScreen : Kind2D;
            else if (plan_camera())
            {
                g_kind = KindCamera;
            }
            else
            {
                g_kind = KindScreen;
                static unsigned told = 0;
                if (told++ < 6)
                    log("stereo: perspective WVP rejected (vs %p): c6 [%g %g %g %g] c7 [%g %g %g %g] c8 [%g %g %g %g] c9 [%g %g %g %g]",
                        g_vs_ptr, g_wvp[0][0], g_wvp[0][1], g_wvp[0][2], g_wvp[0][3], g_wvp[1][0], g_wvp[1][1],
                        g_wvp[1][2], g_wvp[1][3], g_wvp[2][0], g_wvp[2][1], g_wvp[2][2], g_wvp[2][3], g_wvp[3][0],
                        g_wvp[3][1], g_wvp[3][2], g_wvp[3][3]);
            }
        }
        else
            g_kind = g_tex0_is_target ? KindScreen : Kind2D;

        // Only the full-size target carries the HUD; 2D on smaller
        // scene-shaped targets is an intermediate image.
        if (g_kind == Kind2D && (g_rt_w != g_back_w || g_rt_tex_stages != 0))
            g_kind = KindScreen;
        // The lock-on reticle while locked on: per eye, at the target.
        if (g_kind == Kind2D && g_box_ok && reticle_candidate() && plan_reticle())
        {
            g_box_ok = false;
            g_kind = KindReticle;
            ++g_counts[g_kind];
            return 1;
        }
        g_box_ok = false;
        // A full-screen tint: over the whole view (once across both eyes,
        // as a screen pass) or left out, instead of on the HUD screen.
        const Tint tint = g_kind == Kind2D ? classify_tint() : TintNone;
        g_tint_box_ok = false;
        if (tint != TintNone)
        {
            const int mode = tint == TintFade ? config().hud_screen_fades : config().hud_screen_flashes;
            if (mode != 0)
            {
                first_draw_into_target();
                static unsigned told[2] = {};
                if (told[tint - 1]++ < 3)
                    log("stereo: full-screen %s (%ux%u texture): %s", tint == TintFade ? "fade" : "flash", g_tex0_w,
                        g_tex0_h, mode == 1 ? "over the whole view" : "left out");
                g_kind = mode == 1 ? KindTint : KindTintOff;
                ++g_counts[g_kind];
                return mode == 2 ? 3 : 0;
            }
        }
        // Sprite.vsh (screen position = v0 * c10 + c11) scaled to cover the
        // whole screen (the game's full-screen triangle: c10 (2, -4), c11
        // (-1, 1)): a pass over the scene buffer, not HUD art.
        const bool fullscreen_sprite = g_have_vs && !g_vs.reads_wvp && g_vs.reads_c10 &&
                                       fabsf(g_c[10][0]) >= 1.99f && fabsf(g_c[10][1]) >= 1.99f;
        g_fullscreen_sprite = fullscreen_sprite;
        if (g_kind == Kind2D && fullscreen_sprite)
            g_kind = KindScreen;
        const bool first_into_target = g_rt_stereo && first_draw_into_target();
        // The item wheel (and other menus over a paused game) starts its
        // frame with an opaque full-screen copy of the last scene into the
        // back buffer, before any camera draw; gameplay frames start with a
        // camera draw. That copy is one flat picture across both eyes, stuck
        // to the head, so it is replaced by black (user 2026-11-15).
        if (fullscreen_sprite && first_into_target && !g_blend && !g_frame_scene &&
            g_rt_w == g_back_w && g_rt_tex_stages != 0)
        {
            static unsigned told = 0;
            if (told++ < 4)
                log("stereo: a frozen copy of the scene (menu over a paused game): black instead");
            return 5;
        }
        if (fullscreen_sprite && g_rt_stereo && config().debug_skip_fullscreen &&
            !(first_into_target && !g_blend))
            return 3;
        // A picture over the whole back buffer: the full-screen triangle
        // with an ordinary texture (not a render target, so not a pass over
        // the scene), e.g. the loading screen's 640 x 480 sky (draw log
        // `load_19`, docs/findings.md 2026-11-17). Drawn once across the
        // side-by-side buffer it was split between the eyes, half the sky in
        // each; drawn into each eye's half, both see all of it, flat (in the
        // theatre, on its screen).
        if (fullscreen_sprite && g_kind == KindScreen && g_rt_w == g_back_w && g_rt_h == g_back_h &&
            !g_rt_texture && g_rt_tex_stages == 0 && g_tex0_w > 1 && g_tex0_h > 1)
        {
            g_kind = KindPicture;
            ++g_counts[KindScreen];
            static unsigned told = 0;
            if (told++ < 4)
                log("stereo: a full-screen picture (%ux%u texture): drawn into each eye", g_tex0_w, g_tex0_h);
            return 1;
        }

        ++g_counts[g_kind];
        if (g_kind == KindCamera)
        {
            g_frame_scene = true;
            if (g_rt_w == g_back_w)
            {
                g_main_eye_proj[0] = g_eye_proj[0];
                g_main_eye_proj[1] = g_eye_proj[1];
                g_main_view_to_eye[0] = g_eye_view_from_game[0];
                g_main_view_to_eye[1] = g_eye_view_from_game[1];
                g_main_vp = g_vp;
                g_main_rt = g_rt_ptr;
                g_main_frame = g_frame_index;
            }
        }

        if (g_rt_stereo && !g_announced)
        {
            g_announced = true;
            log("stereo: drawing both eyes side by side into a %ux%u back buffer", g_back_w, g_back_h);
        }
        static unsigned total = 0;
        if (++total == 100000 || total % 2000000 == 0)
            log("stereo: draws so far: %u camera (per eye), %u screen-space, %u 2D, %u mono target; "
                "projection scales %s",
                g_counts[KindCamera], g_counts[KindScreen], g_counts[Kind2D], g_counts[KindMono],
                g_have_scales ? "known" : "NOT known");

        if (g_kind == Kind2D && hud_enabled())
            return 2;
        return (g_kind == KindCamera || g_kind == Kind2D) ? 1 : 0;
    }
    }

    void stereo_begin_eye(IDirect3DDevice9* device, int eye)
    {
        set_game_wvp(device);
        const DWORD half = g_rt_w / 2;
        D3DVIEWPORT9 vp = g_vp;
        vp.X = g_vp.X / 2 + (DWORD)eye * half;
        vp.Width = g_vp.Width / 2;
        device->SetViewport(&vp);

        if (g_kind == KindReticle)
        {
            device->SetVertexShaderConstantF(10, &g_reticle_c[eye][0][0], 2);
            g_patched_bb |= (1ull << 10) | (1ull << 11);
        }
        if (g_kind == KindCamera)
        {
            const Mat4 w = psy_wvp_from_regs(g_wvp) * g_eye_m[eye];
            float regs[4][4];
            psy_regs_from_wvp(w, regs);
            device->SetVertexShaderConstantF(6, &regs[0][0], 4);
            g_patched_wvp = true;
            // Particle corner offsets: clip-space vectors added to the clip
            // position, so the same linear map applies (psy_shader_scan.h).
            for (unsigned r = 0; r < 64; ++r)
                if (g_vs.billboard & (1ull << r))
                {
                    float o[4];
                    row_times(g_c[r], g_eye_m[eye], o);
                    device->SetVertexShaderConstantF(r, o, 1);
                    g_patched_bb |= 1ull << r;
                }
        }
    }

    void stereo_end(IDirect3DDevice9* device)
    {
        if (g_patched_wvp)
        {
            device->SetVertexShaderConstantF(6, &g_wvp[0][0], 4);
            g_patched_wvp = false;
        }
        for (unsigned r = 0; r < 64 && g_patched_bb; ++r)
            if (g_patched_bb & (1ull << r))
            {
                device->SetVertexShaderConstantF(r, g_c[r], 1);
                g_patched_bb &= ~(1ull << r);
            }
        device->SetViewport(&g_vp);
    }

    bool stereo_main_pass(int eye, Mat4* world_to_eye_view, Mat4* eye_proj, D3DVIEWPORT9* eye_vp,
                          IDirect3DSurface9** target)
    {
        if (g_main_frame != g_frame_index || !g_main_rt || eye < 0 || eye > 1)
            return false;
        // The world -> view matrix is not known in Psychonauts (only WVP).
        if (world_to_eye_view)
            *world_to_eye_view = Mat4::identity();
        if (eye_proj)
            *eye_proj = g_main_eye_proj[eye];
        if (eye_vp)
        {
            const DWORD half = g_back_w / 2;
            D3DVIEWPORT9 vp = g_main_vp;
            vp.X = g_main_vp.X / 2 + (DWORD)eye * half;
            vp.Width = g_main_vp.Width / 2;
            *eye_vp = vp;
        }
        if (target)
            *target = g_main_rt;
        return true;
    }
}
