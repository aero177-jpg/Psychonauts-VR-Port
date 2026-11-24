#pragma once
// The settings overlay: a Dear ImGui window on the desktop mirror, opened
// and closed with Insert. Each change is saved to psyvr.ini and applied by
// config_reload_live() at the next frame boundary, so the settings it covers
// no longer need the game closed and the file edited.
//
// On the monitor it is drawn onto the back buffer after the frame has gone to
// the headset. With VR up it is also rendered into a texture before the
// submit and shown in both eyes as a panel placed in the room ([overlay]),
// and the VR controllers navigate it. Mouse, keyboard and the game's pad go
// to it, not the game, while it is open.

#include <d3d9.h>

namespace psyvr
{
    // At Present, after hud_composite and before the frame is submitted to
    // the headset: if open, builds this frame's ImGui frame and draws it as
    // the panel into both eyes of `back_buffer`. Does nothing while closed.
    void overlay_vr_frame(IDirect3DSwapChain9* chain, IDirect3DSurface9* back_buffer);
    // Once a frame, just before the real Present: the desktop mirror (the
    // same frame as the headset's, if overlay_vr_frame built one).
    void overlay_frame(IDirect3DSwapChain9* chain);
    // The overlay's device objects do not survive a Reset.
    void overlay_before_reset();
    // It holds a reference on the device; drop it before the device goes.
    void overlay_device_closing(IDirect3DDevice9* real_device);
    bool overlay_open();
    // Opens or closes it as the Insert key does (the pad's Start hold, vr_pad);
    // false if the overlay is not up yet.
    bool overlay_request_open(bool open);
}
