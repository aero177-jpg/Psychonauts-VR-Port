#include "hud.h"
#include "stereo.h"
#include "vr_session.h"
#include "psy_theatre.h"

#include "../proxy/game_psy.h"
#include "../common/config.h"
#include "../common/log.h"

#include <cmath>

namespace psyvr
{
    namespace
    {
        IDirect3DDevice9* g_device = nullptr;
        IDirect3DTexture9* g_texture = nullptr;
        IDirect3DSurface9* g_surface = nullptr;
        unsigned g_w = 0, g_h = 0;               // the HUD texture
        unsigned g_back_w = 0, g_back_h = 0;     // the game's screen (side by side)
        bool g_used_this_frame = false;
        bool g_failed = false;

        // Theatre: the eyes' pictures taken out of the back buffer, and where
        // the screen is in the room (screen space -> room, metres).
        IDirect3DTexture9* g_theatre_tex = nullptr;
        IDirect3DSurface9* g_theatre_surf = nullptr;
        bool g_theatre_failed = false;
        Mat4 g_theatre_to_room = Mat4::identity();         // this frame's, for the HUD
        Mat4 g_theatre_placed_to_room = Mat4::identity();  // hud_theatre_window's
        bool g_theatre_placed = false;
        unsigned g_theatre_placed_opens = 0;
        unsigned g_theatre_placed_recenter = 0;
        float g_theatre_placed_distance = 0.0f;
        bool g_theatre_shown = false;            // this frame's eyes are on the screen

        // [hud] world_locked: where the screen was put in the room (screen
        // space -> room, metres), and when it was last shown.
        Mat4 g_screen_to_room = Mat4::identity();
        bool g_placed = false;
        unsigned g_placed_recenter = 0;
        // The distance it was placed at: a changed [hud] menu_distance puts
        // it in front again, like the settings panel. Left where it was, the
        // screen shrank (its width follows the setting) but stayed far away,
        // and the next menu came back at the full size (user 2026-11-02).
        float g_placed_distance = 0.0f;
        ULONGLONG g_last_shown_ms = 0;
        const ULONGLONG kReplaceAfterMs = 1000;
        // A placed menu that has been this far from where the head looks for
        // this long is put in front again: a safety net for anything that
        // leaves it out of sight (a bad pose when it was placed, the room
        // moved under it), not a follow -- looking around a menu is fine.
        const float kOutOfViewCos = 0.64f;          // about 50 degrees
        const ULONGLONG kOutOfViewMs = 1500;
        ULONGLONG g_out_of_view_since = 0;

        // How far (cosine) the screen's centre is from the head's forward.
        float view_cos(const Mat4& head_to_room, const Mat4& screen_to_room)
        {
            float d[3], len = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                d[k] = screen_to_room.m[3][k] - head_to_room.m[3][k];
                len += d[k] * d[k];
            }
            len = sqrtf(len);
            if (len < 1e-4f)
                return 1.0f;
            return (d[0] * head_to_room.m[2][0] + d[1] * head_to_room.m[2][1] +
                    d[2] * head_to_room.m[2][2]) / len;
        }

        // Saved around a redirected draw.
        IDirect3DSurface9* g_saved_rt = nullptr;
        IDirect3DSurface9* g_saved_ds = nullptr;
        D3DVIEWPORT9 g_saved_vp{};
        DWORD g_saved_z = 0, g_saved_sep = 0, g_saved_srca = 0, g_saved_dsta = 0;

        bool ensure_target()
        {
            if (g_surface)
                return true;
            if (g_failed || !g_device || !g_back_w || !g_back_h)
                return false;
            // The game's screen shape at a readable size: the back buffer is
            // two eyes wide, and the game lays its 2D out across all of it.
            g_w = (g_back_w * 5 / 8) & ~1u;
            g_h = (g_back_h * 5 / 8) & ~1u;
            const HRESULT hr = g_device->CreateTexture(g_w, g_h, 1, D3DUSAGE_RENDERTARGET,
                                                       D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                                       &g_texture, nullptr);
            if (FAILED(hr) || !g_texture ||
                FAILED(g_texture->GetSurfaceLevel(0, &g_surface)))
            {
                log("hud: could not create the %ux%u HUD target (0x%08lX); 2D stays in the eyes",
                    g_w, g_h, hr);
                g_failed = true;
                hud_release();
                return false;
            }
            log("hud: %ux%u virtual screen target", g_w, g_h);
            return true;
        }

        struct Vertex
        {
            float x, y, z, rhw;
            float u, v;
        };
        const DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_TEX1;
    }

    Mat4 hud_place_in_front(const Mat4& head_to_room, float distance)
    {
        const float fx = head_to_room.m[2][0], fz = head_to_room.m[2][2];
        const float len = sqrtf(fx * fx + fz * fz);
        const float s = len > 1e-4f ? fx / len : 0.0f, c = len > 1e-4f ? fz / len : 1.0f;
        Mat4 m = Mat4::identity();
        m.m[0][0] = c;  m.m[0][2] = -s;
        m.m[2][0] = s;  m.m[2][2] = c;
        m.m[3][0] = head_to_room.m[3][0] + s * distance;
        m.m[3][1] = head_to_room.m[3][1];
        m.m[3][2] = head_to_room.m[3][2] + c * distance;
        return m;
    }

    Mat4 hud_place_round_head(const Mat4& head_to_room, float distance, float yaw_left, float pitch_up)
    {
        // The level place straight ahead, then the direction turned: yaw to
        // the left (the room is left-handed, +x to the right, so a turn to the
        // right is positive), pitch up. The panel's z points along it, its x
        // stays level and its y is the third axis: it faces the head.
        const Mat4 base = hud_place_in_front(head_to_room, 0.0f);
        const float fx = base.m[2][0], fz = base.m[2][2];
        const float fy = atan2f(fx, fz) - yaw_left;
        const float sy = sinf(fy), cy = cosf(fy), sp = sinf(pitch_up), cp = cosf(pitch_up);
        Mat4 m = Mat4::identity();
        m.m[0][0] = cy;         m.m[0][1] = 0.0f;   m.m[0][2] = -sy;
        m.m[1][0] = -sy * sp;   m.m[1][1] = cp;     m.m[1][2] = -cy * sp;
        m.m[2][0] = sy * cp;    m.m[2][1] = sp;     m.m[2][2] = cy * cp;
        m.m[3][0] = base.m[3][0] + m.m[2][0] * distance;
        m.m[3][1] = base.m[3][1] + m.m[2][1] * distance;
        m.m[3][2] = base.m[3][2] + m.m[2][2] * distance;
        return m;
    }

    void hud_device_created(IDirect3DDevice9* device, unsigned back_w, unsigned back_h)
    {
        if (device != g_device)
            hud_release();
        g_device = device;
        g_back_w = back_w;
        g_back_h = back_h;
        g_failed = false;
    }

    void hud_release()
    {
        if (g_surface)
            g_surface->Release();
        if (g_texture)
            g_texture->Release();
        g_surface = nullptr;
        g_texture = nullptr;
        if (g_theatre_surf)
            g_theatre_surf->Release();
        if (g_theatre_tex)
            g_theatre_tex->Release();
        g_theatre_surf = nullptr;
        g_theatre_tex = nullptr;
        g_theatre_failed = false;
    }

    namespace { bool g_last_menu = false; }

    bool hud_showing_menu()
    {
        return g_last_menu;
    }

    bool hud_enabled()
    {
        return config().hud_enabled && stereo_active();
    }

    bool hud_begin_draw(IDirect3DDevice9* device)
    {
        if (!hud_enabled() || !ensure_target())
            return false;

        device->GetRenderTarget(0, &g_saved_rt);
        device->GetDepthStencilSurface(&g_saved_ds);
        device->GetViewport(&g_saved_vp);
        device->SetRenderTarget(0, g_surface);
        // The game's depth buffer is bigger than this target, which D3D9
        // allows, but the HUD should not be depth tested against the world.
        device->SetDepthStencilSurface(nullptr);
        if (!g_used_this_frame)
        {
            g_used_this_frame = true;
            device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
        }
        // The game's viewport is in back-buffer pixels; scale it onto the HUD.
        D3DVIEWPORT9 vp = g_saved_vp;
        vp.X = g_saved_vp.X * g_w / g_back_w;
        vp.Y = g_saved_vp.Y * g_h / g_back_h;
        vp.Width = g_saved_vp.Width * g_w / g_back_w;
        vp.Height = g_saved_vp.Height * g_h / g_back_h;
        device->SetViewport(&vp);

        // Colour blends as the game asked; alpha accumulates coverage
        // (one, inverse source alpha), so the result is premultiplied and
        // composites cleanly over the world.
        device->GetRenderState(D3DRS_ZENABLE, &g_saved_z);
        device->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &g_saved_sep);
        device->GetRenderState(D3DRS_SRCBLENDALPHA, &g_saved_srca);
        device->GetRenderState(D3DRS_DESTBLENDALPHA, &g_saved_dsta);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
        return true;
    }

    void hud_end_draw(IDirect3DDevice9* device)
    {
        device->SetRenderState(D3DRS_ZENABLE, g_saved_z);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, g_saved_sep);
        device->SetRenderState(D3DRS_SRCBLENDALPHA, g_saved_srca);
        device->SetRenderState(D3DRS_DESTBLENDALPHA, g_saved_dsta);
        device->SetRenderTarget(0, g_saved_rt);
        device->SetDepthStencilSurface(g_saved_ds);
        device->SetViewport(&g_saved_vp);
        if (g_saved_rt)
            g_saved_rt->Release();
        if (g_saved_ds)
            g_saved_ds->Release();
        g_saved_rt = nullptr;
        g_saved_ds = nullptr;
    }

    void hud_theatre_screen(IDirect3DDevice9* device, IDirect3DSurface9* back_buffer)
    {
        g_theatre_shown = false;
        const Config& c = config();
        D3DSURFACE_DESC desc{};
        Mat4 head_to_room;
        if (!game_theatre_showing() || !stereo_active() || !stereo_last_frame_had_scene() || !device ||
            !back_buffer || FAILED(back_buffer->GetDesc(&desc)) || desc.Width < 2 ||
            !vr_head_in_room_metres(&head_to_room))
            return;
        // A copy of the back buffer to sample from (resolved if multisampled).
        if (!g_theatre_tex && !g_theatre_failed)
        {
            const HRESULT hr = device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format,
                                                     D3DPOOL_DEFAULT, &g_theatre_tex, nullptr);
            if (FAILED(hr) || !g_theatre_tex || FAILED(g_theatre_tex->GetSurfaceLevel(0, &g_theatre_surf)))
            {
                log("theatre: could not create the %ux%u screen texture (0x%08lX); the game stays all round you",
                    desc.Width, desc.Height, hr);
                if (g_theatre_tex)
                    g_theatre_tex->Release();
                g_theatre_tex = nullptr;
                g_theatre_failed = true;
            }
            else
                log("theatre: %ux%u screen texture", desc.Width, desc.Height);
        }
        if (!g_theatre_surf || FAILED(device->StretchRect(back_buffer, nullptr, g_theatre_surf, nullptr, D3DTEXF_NONE)))
            return;

        const TheatreWindowSize w = theatre_window_size(c.theatre_distance, c.theatre_width_degrees, c.theatre_aspect);
        if (!hud_theatre_window(&g_theatre_to_room))
            return;
        const float u0[2] = { 0.0f, 0.5f }, u1[2] = { 0.5f, 1.0f };
        IDirect3DSurface9* saved_rt = nullptr;
        device->GetRenderTarget(0, &saved_rt);
        DWORD scissor = FALSE;
        device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderTarget(0, back_buffer);
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(12, 12, 14), 1.0f, 0);
        device->SetRenderTarget(0, saved_rt);
        if (saved_rt)
            saved_rt->Release();
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, scissor);
        // Screen: each eye's picture on the quad. Window: each eye's own
        // picture (drawn from that eye, game_theatre_eyes) where the window
        // is in it, sampled at the same pixels: the window cut out exactly.
        hud_draw_panel_eyes(device, back_buffer, g_theatre_tex, g_theatre_to_room, head_to_room, w.half_w, w.half_h,
                            u0, u1, false, true, c.theatre_style == kTheatreWindow);
        g_theatre_shown = true;
    }

    bool hud_theatre_window(Mat4* window_to_room)
    {
        // In front of the head (level, at its height) when the theatre opens,
        // at a recentre or a new distance; then left where it is.
        const Config& c = config();
        const TheatreWindowSize w = theatre_window_size(c.theatre_distance, c.theatre_width_degrees, c.theatre_aspect);
        Mat4 head_to_room;
        if (!g_theatre_placed || g_theatre_placed_opens != game_theatre_open_count() ||
            g_theatre_placed_recenter != vr_recenter_count() || g_theatre_placed_distance != w.distance)
        {
            if (!vr_head_in_room_metres(&head_to_room))
                return false;
            g_theatre_placed_to_room = hud_place_in_front(head_to_room, w.distance);
            g_theatre_placed = true;
            g_theatre_placed_opens = game_theatre_open_count();
            g_theatre_placed_recenter = vr_recenter_count();
            g_theatre_placed_distance = w.distance;
            log("theatre: placed %.2f m ahead, %.2f x %.2f m", w.distance, 2.0f * w.half_w, 2.0f * w.half_h);
        }
        *window_to_room = g_theatre_placed_to_room;
        return true;
    }

    void hud_composite(IDirect3DDevice9* device, IDirect3DSurface9* back_buffer, bool paused)
    {
        const bool used = g_used_this_frame;
        g_used_this_frame = false;
        if (!used || !g_texture || !back_buffer)
            return;

        // A flat screen `distance` metres away, `width_degrees` wide (as seen
        // from where it was placed), with the game screen's aspect. World
        // locked: put in front of the head when the 2D appears after a gap
        // (a menu, a loading screen) or after a recentre, then left there.
        // Otherwise it stays in front of the head.
        // Menus and loading screens (2D with no 3D scene behind it) have
        // their own distance, width and shape ([hud] menu_*); the gameplay
        // HUD uses distance / width_degrees.
        // A pause menu draws over its frozen scene, so it counts as a menu
        // here (user 2026-10-18: head-locked, it was hard to use).
        const bool menu = !stereo_last_frame_had_scene() || paused;
        g_last_menu = menu;
        const float distance = menu ? config().hud_menu_distance : config().hud_distance;
        const float width_degrees = menu ? config().hud_menu_width_degrees
                                         : config().hud_width_degrees;
        // [hud] menu_aspect: only the middle of the game's screen, at that
        // shape. The game lays its 2D across the whole two-eye back buffer
        // (about 2.35:1 on a Quest 3), centring its menus; the sides are
        // background.
        const float screen_aspect = (float)g_w / (float)g_h;
        float shown_aspect = screen_aspect;
        // Psychonauts centres its 640x480 2D in the wider screen (the HUD's
        // Sprite.vsh scale is 2/877 x -2/480 at 2560x1400): only that middle
        // part is shown, so the panel's width goes to the HUD itself.
        const float want_aspect = menu ? config().hud_menu_aspect : config().hud_aspect;
        if (want_aspect > 0.0f && want_aspect < screen_aspect)
            shown_aspect = want_aspect;
        const float u0 = 0.5f - 0.5f * shown_aspect / screen_aspect;
        const float u1 = 1.0f - u0;

        Mat4 head_to_room = Mat4::identity();
        Mat4 screen_to_room = translation(0.0f, 0.0f, distance);
        // Only screens without a 3D scene behind them (menus, loading): over
        // gameplay the 2D includes the reticle, which marks where the camera
        // -- the head -- points, so it has to move with the head.
        if (config().hud_world_locked && menu && vr_head_in_room_metres(&head_to_room))
        {
            const ULONGLONG now = GetTickCount64();
            bool place = !g_placed || now - g_last_shown_ms > kReplaceAfterMs ||
                         g_placed_recenter != vr_recenter_count() ||
                         g_placed_distance != distance;
            if (!place && view_cos(head_to_room, g_screen_to_room) < kOutOfViewCos)
            {
                if (!g_out_of_view_since)
                    g_out_of_view_since = now;
                else if (now - g_out_of_view_since > kOutOfViewMs)
                {
                    place = true;
                    static unsigned told = 0;
                    if (told++ < 20)
                        log("hud: the menu was out of view for %.1f s; put in front again",
                            kOutOfViewMs / 1000.0);
                }
            }
            else
                g_out_of_view_since = 0;
            if (place)
            {
                g_screen_to_room = hud_place_in_front(head_to_room, distance);
                g_placed = true;
                g_placed_recenter = vr_recenter_count();
                g_placed_distance = distance;
                g_out_of_view_since = 0;
            }
            g_last_shown_ms = now;
            screen_to_room = g_screen_to_room;
        }
        else if (config().hud_body_locked && !menu && vr_head_in_room_metres(&head_to_room))
        {
            // [hud] body_locked: straight ahead of the room's forward (+z,
            // set by a recentre), level, at the head's height and `distance`
            // ahead of the head's position: it moves with the head but does
            // not turn with it.
            g_placed = false;
            g_out_of_view_since = 0;
            screen_to_room = translation(head_to_room.m[3][0], head_to_room.m[3][1],
                                         head_to_room.m[3][2] + distance);
        }
        else
        {
            head_to_room = Mat4::identity();
            g_placed = false;
            g_out_of_view_since = 0;
        }
        float half_w = distance * tanf(width_degrees * 0.5f * 0.01745329f);
        float half_h = half_w / shown_aspect;
        // Theatre: the HUD on the screen (1 cm in front of it), fixed in the
        // room with it. The game's whole 2D screen, never cropped (it anchors
        // parts of its HUD, the power slots and the brain marks, to the
        // edges of its wide screen, and a crop to the window's shape cut them
        // off at 4:3), fitted inside the window: as tall as it where the
        // window is at least as wide as the 2D screen, otherwise as wide as
        // it (smaller, centred), so it follows the screen's shape.
        float hud_u0 = u0, hud_u1 = u1;
        if (g_theatre_shown && !menu && vr_head_in_room_metres(&head_to_room))
        {
            const TheatreWindowSize w = theatre_window_size(config().theatre_distance,
                                                            config().theatre_width_degrees, config().theatre_aspect);
            screen_to_room = g_theatre_to_room;
            for (int k = 0; k < 3; ++k)
                screen_to_room.m[3][k] -= 0.01f * g_theatre_to_room.m[2][k];
            hud_u0 = 0.0f;
            hud_u1 = 1.0f;
            if (config().theatre_aspect >= screen_aspect)
            {
                half_h = w.half_h;
                half_w = half_h * screen_aspect;
            }
            else
            {
                half_w = w.half_w;
                half_h = half_w / screen_aspect;
            }
        }
        // A menu or loading screen: all of the game's drawing went to the
        // HUD texture, so the eyes still hold whatever was there before.
        hud_draw_panel(device, back_buffer, g_texture, screen_to_room, head_to_room,
                       half_w, half_h, hud_u0, hud_u1, !stereo_last_frame_had_scene());
    }

    void hud_draw_panel(IDirect3DDevice9* device, IDirect3DSurface9* back_buffer,
                        IDirect3DTexture9* texture, const Mat4& panel_to_room,
                        const Mat4& head_to_room, float half_w, float half_h,
                        float u0, float u1, bool clear_eyes)
    {
        const float eu0[2] = { u0, u0 }, eu1[2] = { u1, u1 };
        hud_draw_panel_eyes(device, back_buffer, texture, panel_to_room, head_to_room, half_w, half_h, eu0, eu1,
                            clear_eyes, false);
    }

    void hud_draw_panel_eyes(IDirect3DDevice9* device, IDirect3DSurface9* back_buffer,
                             IDirect3DTexture9* texture, const Mat4& panel_to_room,
                             const Mat4& head_to_room, float half_w, float half_h,
                             const float u0[2], const float u1[2], bool clear_eyes, bool opaque,
                             bool screen_uv)
    {
        D3DSURFACE_DESC desc{};
        if (!device || !back_buffer || !texture ||
            FAILED(back_buffer->GetDesc(&desc)) || desc.Width < 2 || !desc.Height)
            return;
        const unsigned eye_w = desc.Width / 2, eye_h = desc.Height;

        IDirect3DStateBlock9* saved = nullptr;
        if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved)
            return;
        // A state block does not hold the render target or the depth buffer.
        // Psychonauts binds its depth buffer once and never again: left null,
        // the next frame's world drew without depth and the sky covered it.
        IDirect3DSurface9* saved_rt = nullptr;
        IDirect3DSurface9* saved_ds = nullptr;
        device->GetRenderTarget(0, &saved_rt);
        device->GetDepthStencilSurface(&saved_ds);

        device->SetRenderTarget(0, back_buffer);
        device->SetDepthStencilSurface(nullptr);
        D3DVIEWPORT9 vp{ 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
        device->SetViewport(&vp);
        if (clear_eyes)
            device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(kFvf);
        device->SetTexture(0, texture);
        for (DWORD s = 1; s < 8; ++s)
            device->SetTexture(s, nullptr);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        // Each eye's quad is kept to its half (pre-transformed vertices are
        // not clipped, and a screen seen at an angle reaches past the edge).
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, opaque ? FALSE : TRUE);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);           // premultiplied
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);

        for (int eye = 0; eye < 2; ++eye)
        {
            float off[3] = { 0, 0, 0 };
            vr_eye_offset_metres(eye, off);
            const Mat4 proj = vr_eye_projection(eye, 0.05f, 100.0f);
            Mat4 room_to_eye;
            if (!invert(translation(off[0], off[1], off[2]) * head_to_room, &room_to_eye))
                continue;
            const Mat4 screen_to_eye = panel_to_room * room_to_eye;
            // Corners in screen space (left-handed metres), seen from the eye.
            const float cx[4] = { -half_w, half_w, -half_w, half_w };
            const float cy[4] = { half_h, half_h, -half_h, -half_h };
            const float cu[4] = { u0[eye], u1[eye], u0[eye], u1[eye] };
            const float cv[4] = { 0, 0, 1, 1 };
            Vertex v[4];
            bool behind = false;
            for (int i = 0; i < 4; ++i)
            {
                const float* r0 = screen_to_eye.m[0];
                const float* r1 = screen_to_eye.m[1];
                const float* r3 = screen_to_eye.m[3];
                const float x = cx[i] * r0[0] + cy[i] * r1[0] + r3[0];
                const float y = cx[i] * r0[1] + cy[i] * r1[1] + r3[1];
                const float z = cx[i] * r0[2] + cy[i] * r1[2] + r3[2];
                // Pre-transformed vertices are not clipped against the near
                // plane: a corner behind the eye would fold the quad over.
                if (z < 0.05f)
                    behind = true;
                const float clip_x = x * proj.m[0][0] + z * proj.m[2][0];
                const float clip_y = y * proj.m[1][1] + z * proj.m[2][1];
                const float w = z;
                v[i].x = (float)(eye * eye_w) + (clip_x / w * 0.5f + 0.5f) * eye_w - 0.5f;
                v[i].y = (0.5f - clip_y / w * 0.5f) * eye_h - 0.5f;
                v[i].z = 0.0f;
                v[i].rhw = 1.0f / w;
                v[i].u = cu[i];
                v[i].v = cv[i];
                if (screen_uv)
                {
                    // The texture is the back buffer: each corner samples the
                    // pixel it lands on, interpolated straight across the
                    // screen (rhw 1: no perspective).
                    v[i].u = (v[i].x + 0.5f) / (float)desc.Width;
                    v[i].v = (v[i].y + 0.5f) / (float)desc.Height;
                    v[i].rhw = 1.0f;
                }
            }
            const RECT half{ (LONG)(eye * eye_w), 0, (LONG)((eye + 1) * eye_w), (LONG)eye_h };
            device->SetScissorRect(&half);
            if (!behind)
                device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
        }

        device->SetRenderTarget(0, saved_rt);
        device->SetDepthStencilSurface(saved_ds);
        if (saved_rt)
            saved_rt->Release();
        if (saved_ds)
            saved_ds->Release();
        saved->Apply();     // after SetRenderTarget, which resets the viewport and scissor
        saved->Release();
    }
}
