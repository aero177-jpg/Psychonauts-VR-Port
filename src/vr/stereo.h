#pragma once
// Same-frame side-by-side stereo for Psychonauts.
//
// The back buffer is created with both eyes side by side; the game sizes its
// scene targets from it, so it renders a side-by-side frame without being
// asked to. What changes is only what each draw looks through. How each draw
// is classified (what its vertex shader reads, c6 = the WVP) is described at
// the top of stereo.cpp and in docs/shaders.md.
//
// This module lives in psyvr_logic.dll (logic_abi.h); the shim calls it
// through the forwarders in proxy/logic_host.cpp.

#include "hud.h"
#include "vr_math.h"

#include <d3d9.h>

namespace psyvr
{
    // From CreateDevice/Reset: the side-by-side back buffer size. Zero turns
    // stereo off.
    void stereo_set_layout(unsigned back_width, unsigned back_height);
    bool stereo_active();

    // Once a frame: the head moved, so every pass is worked out afresh.
    void stereo_new_frame();
    // After stereo_new_frame: whether the frame just finished drew any 3D
    // (menus and loading screens draw only 2D, and nothing clears the eyes).
    bool stereo_last_frame_had_scene();

    // State the device wrapper forwards here as the game sets it.
    void stereo_note_constants(UINT start, const float* data, UINT count);
    // Pixel shader constants (c1, the camera position in lit shaders).
    void stereo_note_ps_constants(UINT start, const float* data, UINT count);
    void stereo_note_render_target(IDirect3DSurface9* rt0);
    void stereo_note_viewport(const D3DVIEWPORT9& vp);
    void stereo_note_vertex_shader(IDirect3DVertexShader9* shader);
    // D3DRS_ALPHABLENDENABLE, for recognising the bloom composite.
    void stereo_note_alpha_blend(bool enabled);
    // From CreateVertexShader: reads which constants the shader uses.
    // `patched`: created from stereo_patch_vertex_shader's output.
    void stereo_note_vertex_shader_created(IDirect3DVertexShader9* shader, const DWORD* bytecode,
                                           bool patched);
    // Before CreateVertexShader: Water.vsh / CheapWater.vsh are patched to
    // pass their clip position on for the replacement water pixel shaders
    // (psy_patch_screen_coords, psy_shader_scan.h). False = create from the
    // game's own bytecode.
    bool stereo_patch_vertex_shader(const DWORD* in, DWORD* out, size_t capacity, size_t* out_len);
    // Before CreatePixelShader: the game's water pixel shaders are replaced
    // by ones that divide the reflection lookup per pixel (shaders/water_ps.hlsl).
    bool stereo_patch_pixel_shader(const DWORD* in, DWORD* out, size_t capacity, size_t* out_len);
    // Before a draw the game's way (plan 0 or 2): a patched shader gets the
    // identity in c91..c95 (the game's own output).
    void stereo_before_single_draw(IDirect3DDevice9* device);
    // Every stage: a scene copy on any of them makes a non-camera draw a
    // screen-space pass rather than 2D.
    void stereo_note_texture(DWORD stage, IDirect3DBaseTexture9* texture);

    // Before a draw: whether the plan needs the draw's vertex positions
    // (the lock-on reticle, to find its centre; a fading object, for its
    // place on screen), and then their bounds, box = (min x, min y, max x,
    // max y, min z, max z) as the game sent them (null = unreadable). Used by
    // the next stereo_plan_draw only.
    bool stereo_wants_vertex_box();
    void stereo_note_vertex_box(const float* box);

    // This frame's full-size scene pass, as of its last world camera draw:
    // an eye's world -> eye view and projection (the game's depth range),
    // its viewport within the eye's half, and the target (not AddRef'd;
    // valid this frame). False before such a draw this frame.
    bool stereo_main_pass(int eye, Mat4* world_to_eye_view, Mat4* eye_proj,
                          D3DVIEWPORT9* eye_vp, IDirect3DSurface9** target);

    // How the last draw was handled, for traces.
    const char* stereo_last_draw_kind();

    // 0 = draw once as the game asked; 1 = once per eye; 2 = into the HUD
    // texture (hud.h); 3 = not at all; 5 = not drawn, the target cleared to
    // black instead (a menu's frozen copy of the scene, stereo.cpp). (4 was
    // the old theatre's scissored pass.) Decided per draw.
    // A fading object ([graphics] fast_fades; stereo.cpp, "Fading objects"):
    // 6 = its target's full-screen clear, done as a Clear; 7 = the object,
    // once per eye into its target and again into the back buffer's stencil;
    // 8 = its full-screen composite, drawn once where that stencil is set.
    int stereo_plan_draw();
    void stereo_begin_eye(IDirect3DDevice9* device, int eye);
    void stereo_end(IDirect3DDevice9* device);

    // The shim's side of plans 6..8 (proxy_device.cpp). A fading object's
    // target is cleared (6); each object draw is repeated into the back
    // buffer's depth-stencil with colour writes off, setting a stencil bit
    // (7; false = not marked); the composite is drawn only where the bit is
    // set, and clears it (8; begin returns whether the stencil test is on,
    // and saves the stencil and scissor state that end puts back).
    void stereo_fade_clear(IDirect3DDevice9* device);
    bool stereo_fade_mark_begin(IDirect3DDevice9* device);
    void stereo_fade_mark_end(IDirect3DDevice9* device);
    bool stereo_fade_composite_begin(IDirect3DDevice9* device);
    void stereo_fade_composite_end(IDirect3DDevice9* device);
    // Before Reset and when the device goes: drops the depth-stencil held.
    void stereo_fade_release();

    // Draws go through here. `draw` issues the game's draw on the real device.
    template <typename Draw>
    HRESULT stereo_draw(IDirect3DDevice9* device, Draw draw)
    {
        const int plan = stereo_plan_draw();
        if (plan == 0)
        {
            stereo_before_single_draw(device);
            return draw();
        }
        if (plan == 3)
            return D3D_OK;      // left out ([debug] skip_fullscreen_passes)
        if (plan == 6)
        {
            stereo_fade_clear(device);
            return D3D_OK;
        }
        if (plan == 8)
        {
            // The logic scissors it to the object (stereo_before_single_draw);
            // begin / end keep the game's stencil and scissor state.
            stereo_fade_composite_begin(device);
            stereo_before_single_draw(device);
            const HRESULT r = draw();
            stereo_fade_composite_end(device);
            return r;
        }
        if (plan == 5)
        {
            // Black where the game would lay a flat copy of the scene.
            device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
            return D3D_OK;
        }
        if (plan == 2)
        {
            stereo_before_single_draw(device);
            return hud_draw(device, draw);
        }
        HRESULT hr = D3D_OK;
        for (int eye = 0; eye < 2; ++eye)
        {
            stereo_begin_eye(device, eye);
            const HRESULT r = draw();
            if (FAILED(r))
                hr = r;
        }
        stereo_end(device);
        if (plan == 7 && stereo_fade_mark_begin(device))
        {
            for (int eye = 0; eye < 2; ++eye)
            {
                stereo_begin_eye(device, eye);
                draw();
            }
            stereo_end(device);
            stereo_fade_mark_end(device);
        }
        return hr;
    }
}
