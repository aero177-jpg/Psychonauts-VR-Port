#pragma once
// The headset: OpenXR instance bring-up, the poses of the frame being drawn,
// recentring, and turning those into transforms in the game's view space.
//
// Conventions (the Underworld mod's docs/AUDIT.md 9.4-9.5): Underworld's matrices are row-vector,
// row-major, D3D left-handed view space (x right, y up, z forward). OpenXR is
// right-handed with -z forward, so poses are mirrored in z on the way in.

#include "vr_math.h"
#include "xr_runtime.h"

namespace psyvr
{
    // Before the backend's Direct3DCreate9: the OpenXR instance tells DXVK
    // which Vulkan extensions it needs. False = no VR, the game runs flat.
    bool vr_init();
    bool vr_ready();

    // Per eye, from the runtime. Zero until vr_init succeeded.
    void vr_recommended_eye_size(unsigned* width, unsigned* height);

    // The frame the next game frame will be drawn for (from xrWaitFrame).
    void vr_set_frame(const XrFrame& frame);
    bool vr_have_frame();

    // The current head yaw and position become "straight ahead". Also done
    // automatically on the first valid pose.
    void vr_recenter();
    // How many times larger than life the world is shown: head movement and
    // the eyes' separation in game units are multiplied by it ([vr]
    // world_scale x factor units per metre). 1 normally; the board view
    // (camera_rig.h) sets more, gliding with a view switch.
    void vr_set_scale_factor(float factor);

    // Goes up by one at every recentre.
    unsigned vr_recenter_count();

    // A scripted head pose for tests (the control pipe's `head` command): on
    // top of the tracked head, moved by offset_m (metres in the recentred
    // room: +x right, +y up, +z back) and turned by yaw (about the room's up,
    // + = left), pitch (about the head's own right axis, + = up) and roll
    // (about its forward axis, + = left ear down), in degrees. Everything that
    // reads the head (eyes, HUD, hands in the head frame, god mode) sees it.
    struct HeadScript
    {
        bool on = false;
        float offset_m[3] = {};
        float yaw_deg = 0.0f, pitch_deg = 0.0f, roll_deg = 0.0f;
    };
    void vr_set_head_script(const HeadScript& script);
    HeadScript vr_head_script();

    // Head space -> room space (the recentred origin), row-vector, in metres,
    // left-handed: for things placed in the room rather than in the game.
    bool vr_head_in_room_metres(Mat4* out);

    // Head space -> game view space (the game camera), row-vector, in game
    // units: the head's pose relative to the recentred origin.
    bool vr_head_to_game_view(Mat4* out);

    // A controller's grip pose in the same space as vr_head_to_game_view
    // (hand 0 left, 1 right), so the camera's head composition places it in
    // the world the same way. False while it is not tracked.
    bool vr_hand_to_game_view(int hand, Mat4* out);

    // The same grip pose in room metres (like vr_head_in_room_metres; hand
    // scripts included), for the theatre's god hands.
    bool vr_hand_in_room_metres(int hand, Mat4* out);

    // Horizon lock: a rotation of game view space (row-vector, rotation only)
    // from the game camera's view to the view the eyes are placed in (the same
    // camera with its pitch and roll removed). Identity = the game's own.
    void vr_set_view_correction(const Mat4& game_view_to_leveled);

    // The eyes moved this many game units forward along the game camera's
    // view (the start menu's zoom); 0 = where the game camera is.
    void vr_set_view_forward(float units);

    // Per draw (stereo.cpp): the nearest, in game units, a draw's centre may
    // come to the eyes; a nearer one gets its eyes moved back along the view
    // (start menu: [camera] start_menu_near_limit). 0 = no limit.
    void vr_set_near_limit(float units);
    // The forward move and the near limit now, in game units.
    void vr_view_forward(float* forward_units, float* near_limit_units);

    // God mode (god_seat.h): the eyes sit at a seat away from the game
    // camera. The source gives seat view -> game view (rotation and
    // translation, row-vector) for the camera the game draws with now, so it
    // is asked at draw time, not at Present (the camera moves in between);
    // while it answers, it replaces the view correction and the forward
    // offset, and its `scale` the scale factor (vr_set_scale_factor); when it
    // does not, the factor is 1. nullptr = off.
    typedef bool (*SeatSource)(Mat4* seat_to_view, float* scale);
    void vr_set_view_seat(SeatSource source);

    // Eye space -> game view space, row-vector, in game units: the eye's
    // offset from the head, and the head pose unless the engine has it.
    bool vr_eye_to_game_view(int eye, Mat4* out);

    // The eye's position relative to the head, metres, left-handed
    // (x right, y up, z forward). False before the first frame.
    bool vr_eye_offset_metres(int eye, float out[3]);

    // The widest half-angles of the two eyes' views, radians, all positive:
    // above and below the head's forward, and to either side. False before
    // the first frame.
    bool vr_eye_fov_extent(float* up, float* down, float* side);

    // A D3D left-handed projection for the eye's field of view, with the
    // game's near and far planes.
    Mat4 vr_eye_projection(int eye, float near_z, float far_z);

}
