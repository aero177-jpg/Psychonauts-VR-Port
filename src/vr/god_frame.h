#pragma once
// God mode's per-frame camera logic (docs/findings.md), in psyvr_logic.dll so
// it can be changed with a hot reload (proxy/logic_host.h).
//
// The shim (proxy/game_psy.cpp) owns the chase-update hook, the seat state
// (it outlives a reload) and the memory writes: after the game's own chase
// update it gathers what the logic needs into GodFrameIn and calls
// god_frame. The logic answers with the seat for the draws and a list of
// writes into the camera; the shim applies them, keeping the bytes they
// replaced, and puts those back (last first) before the camera's next chase
// update, so the game's camera logic never sees them. Any camera field can
// be tried this way without a relaunch.
//
// The logic may read the camera (with __try); it never writes game memory.
//
// The same path serves classic view under horizon lock (kGodViewLevel):
// there the seat is the game camera's own position with only its yaw, life
// size, so the eyes see the levelled view the lock shows, and the camera the
// game culls with is aimed along the head from it as in god mode. Without it
// the game culls with its own camera, which pitches down as it orbits up
// while the eyes stay level (spots classic_orbit_high*, docs/findings.md).
//
// And every other view of the chase camera (kGodViewPlain: classic without
// horizon lock, the first-person look), so a change of view glides: the
// eyes move from where they were to the new view over [controls]
// view_transition_seconds (GodSeatState::glide). The seat answers for the
// draws while a glide runs (GodFrameOut::seat); a plain view on its own
// leaves the draws to the game's camera as before.

#include "god_seat.h"

namespace psyvr
{
    // The view the chase camera is shown with (GodFrameIn::view).
    enum GodView
    {
        kGodViewPlain = 0,          // the game camera itself (levelled when `lock`), no seat, no writes
        kGodViewGod = 1,            // god mode's seat
        kGodViewLevel = 2,          // classic view under horizon lock: the seat is the game camera
                                    // itself, turned level (its yaw only); the camera is aimed
    };

    struct GodFrameIn
    {
        char* camera;               // the chase camera, just updated by the game
        int view;                   // GodView
        bool first_person;          // the camera is in the first-person look (sub-mode 2)
        bool lock;                  // horizon lock applies (a plain view is levelled)
        float real_dt;              // seconds since the last call, wall clock (the glide)
        float cam_pos[3];           // the camera's own position (+0x08)
        float dt_ms;                // the chase update's dt (milliseconds)
        float raz[3];               // Raz's position
        float cam_rot[3][3];        // the camera's rotation (god_seat.h), from its own +0x20
        bool pad_ok;                // the right stick as the game last read it, and the overlay closed
        float stick_x, stick_y;     // the right stick, -1..1, up positive (vr_pad.h VrRightStick)
        bool stick_clicked;         // its click is down: the stick adjusts rather than turns
        bool stick_settling;        // the click was let go under 200 ms ago
        bool relatch;               // the view was recentred since the last call
        // A view reset (a tap of the right stick click): the view behind Raz
        // at its own height, gliding there. The shim has already turned the
        // game's chase camera behind him (classic); god mode's seat turns
        // here. Zoom and Raz's size stay.
        bool reset;
        bool raz_fwd_ok;
        float raz_fwd[3];           // Raz's forward (his body matrix's third row), world
        bool head_ok;               // a head pose
        Mat4 head;                  // vr_head_in_room_metres
        bool fov_ok;                // vr_eye_fov_extent
        float fov_up, fov_down, fov_side;
    };

    struct GodWrite
    {
        unsigned offset;            // from the camera, below kGodWriteSpan
        unsigned size;              // 1..sizeof(bytes)
        unsigned char bytes[64];
    };
    const unsigned kGodMaxWrites = 16;
    const unsigned kGodWriteSpan = 0x1000;

    struct GodFrameOut
    {
        bool seat;                  // the draws use the seat below (else the game camera's view)
        float pos[3];               // the seat, for the draws
        float rot[3][3];
        float scale;
        unsigned write_count;
        GodWrite writes[kGodMaxWrites];
    };

    // `seat` is the shim's, kept across frames and reloads.
    void god_frame(const GodFrameIn& in, GodSeatState& seat, GodFrameOut& out);
}
