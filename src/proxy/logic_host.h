#pragma once
// The shim's side of the logic DLL (common/logic_abi.h).
//
// stereo.h's functions are implemented here as forwarders into psyvr_logic.dll,
// which holds stereo.cpp, and so is god mode's
// per-frame logic (god_frame.h, called from game_psy.cpp). Callers do not know the
// difference. With no logic loaded the forwarders do nothing (stereo off).
//
// [debug] hot_reload = 1 makes the DLL swappable while the game runs: it is
// loaded from a temporary copy (so the build can overwrite the original),
// polled every 30 frames, and a rebuilt one is loaded, given the state this
// side mirrored (the layout, shaders, bindings, constants), and swapped in at
// the frame boundary. Limitation: shaders are patched when they are created,
// so a change to the patching code only applies to shaders created after the
// reload.

#include "../vr/god_frame.h"

namespace psyvr
{
    // Once, early (Direct3DCreate9), before the device exists.
    void logic_load_initial();
    // At the frame boundary, just after stereo_new_frame().
    void logic_poll_frame_boundary();
    // At unload: the logic's temporary copy is removed.
    void logic_unload();

    // God mode's logic (god_frame.h), forwarded; false with no logic loaded.
    bool logic_god_frame(const GodFrameIn& in, GodSeatState& seat, GodFrameOut& out);
    bool logic_god_seat_to_view(const float seat_pos[3], const float seat_rot[3][3], const float cam_pos[3],
                                const float cam_rot[3][3], Mat4* out);
}
