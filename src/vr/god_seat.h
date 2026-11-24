#pragma once
// God mode's seat: the world shown small (Raz a few centimetres tall), seen
// from a seat above and behind him that follows him on a leash. Ported from
// the Underworld mod's board view (its src/vr/camera_rig: camera_rig_board,
// camera_rig_leash), redone for Psychonauts' world.
//
// Conventions (docs/findings.md): world right-handed, +Y up, game units
// ([vr] world_scale a metre). A camera is its world position plus a rotation
// R (3x3, column vectors: camera -> world) whose camera looks down -Z with +Y
// up, as the game's camera quaternion is. The stereo works in D3D view space
// (x right, y up, z forward), row-vector: view = S * camera, S = diag(1, 1, -1).
//
// "Table metres" are metres at the shown scale: a table metre is
// world_scale x god_seat_factor game units.
//
// No config() and no game memory here, so src/test/mathtest.cpp builds it:
// game_psy.cpp reads Raz and the camera and passes them in.

#include "vr_math.h"

namespace psyvr
{
    struct GodOptions
    {
        float raz_height_cm = 15.0f;    // Raz's apparent height
        float distance = 0.6f;          // seat, table metres back from the centre
        float height = 0.35f;           // and up
        float leash = 0.35f;            // table metres Raz goes from the centre before it follows
        float leash_height = 0.1f;      // table metres up or down before its height follows
        float focus_height = 0.7f;      // real metres above the centre the seat looks along to
        float world_scale = 100.0f;     // game units per real metre
    };

    // The running state, kept by the caller between frames.
    struct GodSeatState
    {
        bool have = false;
        float anchor[3] = {};           // the leashed centre (on the floor, under Raz's feet height)
        float away[3] = { 0.0f, 0.0f, 1.0f };   // the seat's direction from it, horizontal unit
        float zoom = 1.0f;              // factor on distance and height (the right stick)
        // The right stick clicked in (god_frame.cpp), for the session; a
        // recentre puts them back.
        float size = 1.0f;              // factor on Raz's apparent height (x)
        float lift = 0.0f;              // table metres added to the seat's height (y)
        float level_lift = 0.0f;        // classic view under horizon lock: real metres up (y)
        // The view switch's glide (god_frame.cpp, god_view_blend). view_kind
        // is the last frame's view (-1 = none: the next is a cut); a change
        // starts a glide from `last_*` (what the eyes were shown) to the new
        // view, `glide` running 0 -> 1 (1 = none).
        int view_kind = -1;
        float glide = 1.0f;
        float from_pos[3] = {};
        float from_rot[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        float from_scale = 1.0f;
        float last_pos[3] = {};
        float last_rot[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        float last_scale = 1.0f;
    };

    // How many times larger than life the world is shown at this scale: Raz's
    // real 1.25 m over raz_height_cm.
    float god_seat_factor(const GodOptions& o);

    // The seat's direction from a camera: its forward reversed and made
    // level, unit. +Z when it has no heading (looking straight down).
    void god_seat_direction(const float cam_rot[3][3], float away[3]);

    // Leash (Underworld camera_rig_leash, y up): `anchor` moves toward `raz`
    // horizontally only as far as needed to be within radius_units of him;
    // its height follows him at once with height_units <= 0, else only past
    // that band (pulled to its edge). Radius <= 0 puts it on him.
    void god_seat_leash(float anchor[3], const float raz[3], float radius_units, float height_units);

    // One frame. The first frame (or a jump of more than 3 table metres: a
    // teleport, a load) puts the centre on Raz and takes the direction from
    // the camera; after that the leash pulls the centre, and `turn` (radians,
    // + = the seat moves to its right) swings the seat round Raz himself, so
    // he stays put in the view. `relatch` (a recentre) puts the centre back
    // on him.
    void god_seat_update(GodSeatState& s, const float raz[3], const float cam_rot[3][3],
                         float turn, bool relatch, const GodOptions& o);

    // The seat: from the centre at focus height, back along `away` by
    // distance and up by height (table metres x zoom); level, looking back
    // along -away. rot is camera -> world (the game's convention).
    void god_seat_pose(const GodSeatState& s, const GodOptions& o, float pos[3], float rot[3][3]);

    // The point the seat looks along to (centre + focus height), world.
    void god_seat_focus(const GodSeatState& s, const GodOptions& o, float out[3]);

    // Seat view -> game view, D3D view space, row-vector: what replaces the
    // horizon lock's correction when the eyes sit at the seat. A point at
    // `seat_view` (seat's D3D view space) times this lands in the game
    // camera's D3D view space.
    Mat4 god_seat_to_game_view(const float seat_pos[3], const float seat_rot[3][3],
                               const float cam_pos[3], const float cam_rot[3][3]);

    // Where the game camera goes so it culls for the headset rather than for
    // the line to Raz: the head's pose relative to the recentred origin
    // (room space, left-handed, metres: its forward and its position) put on
    // the seat. eye = seat + head position x units_per_metre; target = eye +
    // reach along the head's forward, its pitch held within +-max_pitch
    // (radians) so the game's look-at never points straight up or down.
    // head_up_room (may be null) steadies the heading when the forward is
    // near vertical (as steady_yaw_of: its horizontal part, weighted by the
    // forward's vertical part squared), so looking at the feet does not
    // swing the heading with small head turns.
    void god_head_aim(const float seat_pos[3], const float seat_rot[3][3], const float head_forward_room[3],
                      const float head_up_room[3], const float head_pos_room[3], float units_per_metre,
                      float max_pitch, float reach, float eye[3], float target[3]);

    // The game camera's cull field of view for the headset, in Lua units (2 x
    // the vertical angle, degrees) as camera + 0x30 holds it: the eyes' view
    // (half-angles in radians: up, down, to the side) plus `margin` (radians)
    // on every side, for a camera aimed at the middle of up and down
    // (god_cull_pitch_offset) with the game's `aspect` (width / height).
    float god_cull_fov_lua(float up, float down, float side, float margin, float aspect);

    // How far (radians, + = up) to tilt the cull camera off the head's
    // forward so it aims at the middle of the eyes' view (Quest 3: 44 deg up,
    // 54 down -> 5 deg down).
    inline float god_cull_pitch_offset(float up, float down) { return 0.5f * (up - down); }

    // The game camera's quaternion (x, y, z, w; camera -> world, looking down
    // -Z, +Y up, no roll) looking from `from` to `to`. Not straight up/down.
    void god_look_quat(const float from[3], const float to[3], float q[4]);

    // Rotation (camera -> world) of a unit quaternion x, y, z, w, column vectors.
    void god_quat_to_rot(const float q[4], float rot[3][3]);

    // 3t^2 - 2t^3 on t clamped to 0..1: the glide's easing.
    float god_smoothstep(float t);

    // The view switch's glide (as the Underworld mod's): a pose between
    // (p0, r0, s0) at t = 0 and (p1, r1, s1) at t = 1. Position straight,
    // rotation along the shorter arc (slerp), scale evenly in its logarithm
    // (so 8x -> 1x passes 2.8x half way, as it looks). t is not eased here.
    void god_view_blend(const float p0[3], const float r0[3][3], float s0, const float p1[3],
                        const float r1[3][3], float s1, float t, float p[3], float r[3][3], float* s);
}
