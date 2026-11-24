#pragma once
// The game's 2D -- HUD, menus, loading screens -- on a virtual screen.
//
// Underworld lays its 2D out for the whole (side-by-side) back buffer, so
// drawn straight into it each eye got a squeezed half or a squeezed copy.
// Instead those draws (stereo.cpp's "2D" kind) are redirected into an
// offscreen texture shaped like the game's screen, and at Present that
// texture is drawn into both eyes as a flat screen floating in front of the
// head, each eye seeing it from its own position so it sits at a real depth.

#include <d3d9.h>

#include "vr_math.h"

namespace psyvr
{
    // Device lifetime (the real device). Release before Reset.
    void hud_device_created(IDirect3DDevice9* device, unsigned back_w, unsigned back_h);
    void hud_release();

    bool hud_enabled();
    // Which screen the 2D went to last frame: true = the menu screen (no 3D
    // behind it: menus, loading), false = the gameplay HUD. For the overlay.
    bool hud_showing_menu();

    // Issues one game draw into the HUD texture instead of the current
    // target. `draw` performs the draw on the real device.
    template <typename Draw>
    HRESULT hud_draw(IDirect3DDevice9* device, Draw draw);

    // At Present, before the frame is submitted: the HUD into both eyes, if
    // anything was drawn into it this frame. `paused`: a pause menu over a
    // frozen scene, world-locked like the menus without one.
    void hud_composite(IDirect3DDevice9* device, IDirect3DSurface9* back_buffer,
                       bool paused = false);

    // Theatre (game_theatre_showing, psy_theatre.h): at Present, before
    // hud_composite, the eyes' pictures (drawn from the theatre's virtual
    // eyes) are taken out of the back buffer, the eyes cleared to the dark
    // room, and each picture laid on the screen fixed in the room, seen from
    // that eye. The screen is placed in front of the head when the theatre
    // opens and at a recentre. Nothing on a frame without 3D (menus).
    void hud_theatre_screen(IDirect3DDevice9* device, IDirect3DSurface9* back_buffer);

    // Level, facing the way the head faces (head space -> room, metres),
    // `distance` metres ahead of it: where a panel is put when it appears.
    Mat4 hud_place_in_front(const Mat4& head_to_room, float distance);

    // The same, `distance` metres from the head at `yaw_left` radians to the
    // left of the head's yaw and `pitch_up` radians above level, turned to
    // face the head (the settings overlay's place). 0, 0 = hud_place_in_front.
    Mat4 hud_place_round_head(const Mat4& head_to_room, float distance, float yaw_left, float pitch_up);

    // A flat panel in the room, drawn into both eyes of the side-by-side
    // `back_buffer`, each eye seeing it from its own position: `texture`
    // (premultiplied alpha) spans [u0, u1] x [0, 1] of itself onto a
    // 2*half_w x 2*half_h metre quad, its centre and facing given by
    // `panel_to_room` (+z away from the viewer). `head_to_room` is the head
    // pose the eyes are offset from; identity with panel_to_room in head
    // space gives a head-locked panel. `clear_eyes` blacks the eyes first
    // (a frame the game drew nothing 3D into). Device state is restored.
    void hud_draw_panel(IDirect3DDevice9* device, IDirect3DSurface9* back_buffer,
                        IDirect3DTexture9* texture, const Mat4& panel_to_room,
                        const Mat4& head_to_room, float half_w, float half_h,
                        float u0, float u1, bool clear_eyes);

    // The same with each eye's own [u0, u1] of the texture (eye 0 left), and
    // `opaque`: the texture's alpha ignored (no blending). `screen_uv`: the
    // texture is a copy of the back buffer and each pixel of the quad shows
    // the same pixel of it (u0 / u1 unused): a cut-out.
    void hud_draw_panel_eyes(IDirect3DDevice9* device, IDirect3DSurface9* back_buffer,
                             IDirect3DTexture9* texture, const Mat4& panel_to_room,
                             const Mat4& head_to_room, float half_w, float half_h,
                             const float u0[2], const float u1[2], bool clear_eyes, bool opaque,
                             bool screen_uv = false);

    // The theatre's window (screen space -> room, metres; +z away from the
    // viewer): placed in front of the head when the theatre opens, at a
    // recentre and at a new distance. False before a head pose.
    bool hud_theatre_window(Mat4* window_to_room);

    // Implementation of hud_draw.
    bool hud_begin_draw(IDirect3DDevice9* device);
    void hud_end_draw(IDirect3DDevice9* device);

    template <typename Draw>
    HRESULT hud_draw(IDirect3DDevice9* device, Draw draw)
    {
        if (!hud_begin_draw(device))
            return draw();
        const HRESULT hr = draw();
        hud_end_draw(device);
        return hr;
    }
}
