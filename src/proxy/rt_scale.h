#pragma once
// Larger water reflection targets (4 times the game's; docs/findings.md
// 2026-11-16). The game renders the water's reflection into a square 512 x 512
// render-target texture (with a 512 x 512 depth buffer) through its own
// projection, which VR widens to ~150 deg for culling, and copies it into a
// 256 x 256 one that the water samples. At that size thin things in the
// reflection flicker as the view turns. In VR these square targets, and
// depth buffers of those sizes, are created `scale` times larger; while one
// is bound, viewports, scissor rectangles and StretchRect rectangles are
// scaled to match (and GetViewport scaled back). Everything that samples them
// uses 0..1 texture coordinates, so nothing else changes.

#include <d3d9.h>

namespace psyvr
{
    // The glow's last target (sized to the headset; docs/findings.md 2026-11-16):
    // a fixed 320 x 240 render target the glow, blurred at half the back
    // buffer's size, is reduced into and then laid over the screen. At VR
    // sizes that reduction is 4:1 or more and point-samples the glow into
    // stripes; it is created larger the same way.
    //
    // The factor for a texture or depth buffer the game creates (1 = as
    // asked); back_width = the back buffer's width (both eyes).
    unsigned rt_scale_for_texture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
                                  UINT back_width);
    unsigned rt_scale_for_depth(UINT width, UINT height, UINT back_width);
    // After CreateTexture: remembers (or forgets) the texture's factor.
    void rt_scale_note_texture(IDirect3DTexture9* texture, unsigned scale);
    // SetRenderTarget(0, ...): the factor in force until the next one.
    void rt_scale_note_target(IDirect3DSurface9* rt0);
    unsigned rt_scale_current();
    // A surface's factor (a level of a scaled texture), 1 otherwise.
    unsigned rt_scale_of(IDirect3DSurface9* surface);
}
