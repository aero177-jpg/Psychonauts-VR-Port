#include "vr_session.h"

#include "../common/config.h"
#include "../common/log.h"

#include <windows.h>

#include <cmath>

namespace psyvr
{
    namespace
    {
        bool g_ready = false;
        unsigned g_eye_w = 0, g_eye_h = 0;

        bool g_have_frame = false;
        XrEyeView g_eyes[2]{};
        bool g_eyes_valid = false;
        Mat34 g_head{};
        bool g_head_valid = false;
        Mat34 g_hand[2]{};                  // controller grip poses, left, right
        bool g_hand_valid[2] = {};

        // Recentre origin: yaw and position only, so looking down when
        // recentring does not tilt the world.
        float g_origin_yaw = 0.0f;
        float g_origin_pos[3] = {};
        bool g_have_origin = false;
        unsigned g_recenter_count = 0;

        // 3x4 column-vector rigid transforms (OpenXR's convention), composed
        // as a * b.
        Mat34 compose(const Mat34& a, const Mat34& b)
        {
            Mat34 out{};
            for (int r = 0; r < 3; ++r)
            {
                for (int c = 0; c < 4; ++c)
                {
                    float v = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] +
                              a.m[r][2] * b.m[2][c];
                    if (c == 3)
                        v += a.m[r][3];
                    out.m[r][c] = v;
                }
            }
            return out;
        }

        Mat34 identity34()
        {
            Mat34 m{};
            m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.0f;
            return m;
        }

        // Inverse of a rigid 3x4: R^T and -R^T t.
        Mat34 rigid_inverse34(const Mat34& p)
        {
            Mat34 m{};
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    m.m[r][c] = p.m[c][r];
            for (int r = 0; r < 3; ++r)
                m.m[r][3] = -(m.m[r][0] * p.m[0][3] + m.m[r][1] * p.m[1][3] + m.m[r][2] * p.m[2][3]);
            return m;
        }

        // Inverse of the recentre origin (vr_math.h: xr_recentre_inverse).
        Mat34 origin_inverse()
        {
            Mat34 m{};
            xr_recentre_inverse(g_origin_yaw, g_origin_pos, m.m);
            return m;
        }
    }

    bool vr_init()
    {
        if (g_ready)
            return true;
        if (!config().vr_enabled)
        {
            log("vr: off in psyvr.ini ([vr] enabled = 0)");
            return false;
        }
        if (!xr_instance_init())
        {
            log("vr: %s -- the game runs flat", xr_last_error());
            return false;
        }
        if (!xr_recommended_size(&g_eye_w, &g_eye_h) || !g_eye_w || !g_eye_h)
        {
            log("vr: the runtime gave no view size -- the game runs flat");
            return false;
        }
        const unsigned asked_w = g_eye_w, asked_h = g_eye_h;
        const float scale = config().resolution_scale;
        g_eye_w = (unsigned)(asked_w * scale + 0.5f) & ~1u;
        g_eye_h = (unsigned)(asked_h * scale + 0.5f) & ~1u;
        g_ready = true;
        log("vr: %s, %ux%u an eye (the runtime asks for %ux%u; [vr] resolution_scale %.2f)",
            xr_system_name(), g_eye_w, g_eye_h, asked_w, asked_h, scale);
        return true;
    }

    bool vr_ready()
    {
        return g_ready;
    }

    void vr_recommended_eye_size(unsigned* width, unsigned* height)
    {
        if (width) *width = g_ready ? g_eye_w : 0;
        if (height) *height = g_ready ? g_eye_h : 0;
    }

    void vr_set_frame(const XrFrame& frame)
    {
        if (frame.eyes_valid)
        {
            g_eyes[0] = frame.eyes[0];
            g_eyes[1] = frame.eyes[1];
            g_eyes_valid = true;
        }
        const TrackedPose& hmd = frame.poses[kPoseHmd];
        // Each eye relative to the head from the LOCAL-space poses: the
        // VIEW-space answer is room coordinates on OpenXR-Simulator (eyes
        // 1.7 m above the head), which put the HUD and the stereo camera
        // off by the head height.
        if (frame.eyes_valid && frame.eye_in_local_valid && hmd.bPoseIsValid)
        {
            const Mat34 head_inv = rigid_inverse34(hmd.mDeviceToAbsoluteTracking);
            for (int i = 0; i < 2; ++i)
                g_eyes[i].eye_to_head = compose(head_inv, frame.eye_in_local[i]);
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                log("vr: eye offsets from the head %.3f %.3f %.3f / %.3f %.3f %.3f m (the "
                    "runtime's VIEW-space answer was %.3f %.3f %.3f)",
                    g_eyes[0].eye_to_head.m[0][3], g_eyes[0].eye_to_head.m[1][3],
                    g_eyes[0].eye_to_head.m[2][3], g_eyes[1].eye_to_head.m[0][3],
                    g_eyes[1].eye_to_head.m[1][3], g_eyes[1].eye_to_head.m[2][3],
                    frame.eyes[0].eye_to_head.m[0][3], frame.eyes[0].eye_to_head.m[1][3],
                    frame.eyes[0].eye_to_head.m[2][3]);
            }
        }
        for (int h = 0; h < 2; ++h)
        {
            const TrackedPose& p = frame.poses[h == 0 ? kPoseLeft : kPoseRight];
            g_hand_valid[h] = p.bPoseIsValid;
            if (p.bPoseIsValid)
                g_hand[h] = p.mDeviceToAbsoluteTracking;
        }
        if (hmd.bPoseIsValid)
        {
            // Where the room is anchored (the recentre origin, world-locked
            // menus) is set from one pose and kept, so a placeholder pose at
            // start-up, or the runtime moving its space afterwards, would
            // leave menus metres away (seen on Virtual Desktop: the start-up
            // screens far below the view). So:
            // - the first recentre waits for the session to be visible and
            //   the head actually tracked (or, for a runtime that never says
            //   "tracked", valid for 2 s while visible);
            // - the runtime's own recentre (LOCAL moved) recentres here too;
            // - so does a jump of the head no body can make in one frame
            //   (half a metre): tracking starting up or re-localising.
            const char* why = nullptr;
            static unsigned seen_space_changes = 0;
            static ULONGLONG visible_since = 0;
            const ULONGLONG now = GetTickCount64();
            if (!xr_session_visible())
                visible_since = 0;
            else if (!visible_since)
                visible_since = now;
            if (!g_have_origin)
            {
                if (visible_since && hmd.bPoseIsTracked)
                    why = "first tracked head pose";
                else if (visible_since && now - visible_since > 2000)
                    why = "head pose valid for 2 s (the runtime never says tracked)";
                seen_space_changes = xr_local_space_changes();
            }
            else if (xr_local_space_changes() != seen_space_changes)
            {
                seen_space_changes = xr_local_space_changes();
                why = "the runtime moved its space";
            }
            else if (g_head_valid)
            {
                float jump = 0.0f;
                for (int i = 0; i < 3; ++i)
                {
                    const float d = hmd.mDeviceToAbsoluteTracking.m[i][3] - g_head.m[i][3];
                    jump += d * d;
                }
                if (jump > 0.25f)
                    why = "the head jumped over half a metre in one frame";
            }
            g_head = hmd.mDeviceToAbsoluteTracking;
            g_head_valid = true;
            if (why)
            {
                log("vr: recentre: %s", why);
                vr_recenter();
            }
        }
        g_have_frame = g_eyes_valid;
    }

    bool vr_have_frame()
    {
        return g_have_frame;
    }

    void vr_recenter()
    {
        if (!g_head_valid)
            return;
        // Yaw of the head's forward (-z) about y: for a pure yaw R_y(a), the
        // third column is (sin a, 0, cos a).
        g_origin_yaw = xr_pose_yaw(g_head.m);
        for (int i = 0; i < 3; ++i)
            g_origin_pos[i] = g_head.m[i][3];
        g_have_origin = true;
        ++g_recenter_count;
        log("vr: recentred (yaw %.1f deg, head at %.2f %.2f %.2f m)",
            g_origin_yaw * 57.29578f, g_origin_pos[0], g_origin_pos[1], g_origin_pos[2]);
    }

    namespace
    {
        Mat4 g_leveled_to_view = Mat4::identity();   // inverse of the view correction
        float g_view_forward = 0.0f;                 // game units (vr_set_view_forward)
        float g_near_limit = 0.0f;                   // game units (vr_set_near_limit)
        float g_scale_factor = 1.0f;

        // vr_math.h: game_from_xr_pose, at the configured world scale times
        // the view's factor (board: the world made larger than life).
        Mat4 to_game(const Mat34& p)
        {
            return game_from_xr_pose(p.m, config().world_scale * g_scale_factor);
        }
    }

    void vr_set_scale_factor(float factor)
    {
        g_scale_factor = factor > 0.0f ? factor : 1.0f;
    }

    namespace
    {
        HeadScript g_head_script;

        // A rotation by `deg` about axis 0 (x), 1 (y) or 2 (z), OpenXR axes.
        Mat34 axis_rotation(int axis, float deg)
        {
            const float a = deg * 0.01745329f, c = cosf(a), s = sinf(a);
            Mat34 m = identity34();
            const int i = (axis + 1) % 3, j = (axis + 2) % 3;
            m.m[i][i] = c;  m.m[i][j] = -s;
            m.m[j][i] = s;  m.m[j][j] = c;
            return m;
        }

        // The head script (vr_set_head_script) applied to a tracking-space
        // head pose. The offset is in the recentred room, so it is turned by
        // the recentre yaw into tracking space; yaw turns about the up axis
        // (the same in both), pitch and roll about the head's own axes.
        Mat34 apply_head_script(const Mat34& pose)
        {
            const HeadScript& s = g_head_script;
            if (!s.on)
                return pose;
            const Mat34 origin = rigid_inverse34(origin_inverse());
            Mat34 rot = pose;
            for (int i = 0; i < 3; ++i)
                rot.m[i][3] = 0.0f;
            rot = compose(axis_rotation(1, s.yaw_deg),
                          compose(rot, compose(axis_rotation(0, s.pitch_deg),
                                               axis_rotation(2, s.roll_deg))));
            for (int i = 0; i < 3; ++i)
                rot.m[i][3] = pose.m[i][3] + origin.m[i][0] * s.offset_m[0] +
                              origin.m[i][1] * s.offset_m[1] + origin.m[i][2] * s.offset_m[2];
            return rot;
        }

        // [debug] head_yaw_test: a fixed extra yaw on the head, so the head
        // path can be checked against a runtime whose head never moves.
        // [debug] head_sway_test: a sideways position sway of the head (cm,
        // along the head's own right axis), once every two seconds.
        // Then the head script, if one is on.
        Mat34 debug_yaw(const Mat34& tracked)
        {
            const Mat34 pose = apply_head_script(tracked);
            Mat34 head = pose;
            if (config().debug_head_sway != 0.0f)
            {
                const float t = (float)(GetTickCount64() % 2000) * 0.001f;
                const float d = config().debug_head_sway * 0.01f *
                                sinf(6.283185f * 0.5f * t);
                for (int i = 0; i < 3; ++i)
                    head.m[i][3] += d * pose.m[i][0];
            }
            float deg = config().debug_head_yaw;
            if (config().debug_head_wobble != 0.0f)
                deg += config().debug_head_wobble *
                       sinf((float)(GetTickCount64() % 1000) * 0.006283185f);
            if (deg == 0.0f)
                return head;
            const float a = deg * 0.01745329f, c = cosf(a), s = sinf(a);
            Mat34 y{};
            y.m[0][0] = c;  y.m[0][2] = s;
            y.m[1][1] = 1.0f;
            y.m[2][0] = -s; y.m[2][2] = c;
            for (int i = 0; i < 3; ++i)
                y.m[i][3] = head.m[i][3];
            Mat34 r = head;
            for (int i = 0; i < 3; ++i)
                r.m[i][3] = 0.0f;
            Mat34 out = compose(y, r);
            return out;
        }
    }

    bool vr_head_to_game_view(Mat4* out)
    {
        if (!out)
            return false;
        if (!g_head_valid || !g_have_origin)
        {
            *out = Mat4::identity();
            return false;
        }
        *out = to_game(compose(origin_inverse(), debug_yaw(g_head)));
        return true;
    }

    unsigned vr_recenter_count()
    {
        return g_recenter_count;
    }

    void vr_set_head_script(const HeadScript& script)
    {
        g_head_script = script;
    }

    HeadScript vr_head_script()
    {
        return g_head_script;
    }

    bool vr_head_in_room_metres(Mat4* out)
    {
        if (!out || !g_head_valid || !g_have_origin)
            return false;
        *out = game_from_xr_pose(compose(origin_inverse(), debug_yaw(g_head)).m, 1.0f);
        return true;
    }

    namespace
    {
        // A controller's grip pose relative to the recentred origin (OpenXR
        // axes, metres), from the controller.
        bool hand_pose_in_origin(int hand, Mat34* out)
        {
            if (hand < 0 || hand > 1 || !g_have_origin)
                return false;
            if (!g_hand_valid[hand])
                return false;
            *out = compose(origin_inverse(), debug_yaw(g_hand[hand]));
            return true;
        }
    }

    bool vr_hand_to_game_view(int hand, Mat4* out)
    {
        Mat34 p;
        if (!out || !hand_pose_in_origin(hand, &p))
            return false;
        *out = to_game(p);
        return true;
    }

    bool vr_hand_in_room_metres(int hand, Mat4* out)
    {
        Mat34 p;
        if (!out || !hand_pose_in_origin(hand, &p))
            return false;
        *out = game_from_xr_pose(p.m, 1.0f);
        return true;
    }

    void vr_set_view_correction(const Mat4& game_view_to_leveled)
    {
        // A rotation: its inverse is the transpose.
        Mat4 t = Mat4::identity();
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                t.m[r][c] = game_view_to_leveled.m[c][r];
        g_leveled_to_view = t;
    }

    void vr_set_view_forward(float units)
    {
        g_view_forward = units;
    }

    void vr_set_near_limit(float units)
    {
        g_near_limit = units;
    }

    void vr_view_forward(float* forward_units, float* near_limit_units)
    {
        if (forward_units)
            *forward_units = g_view_forward;
        if (near_limit_units)
            *near_limit_units = g_near_limit;
    }

    namespace
    {
        SeatSource g_seat_source = nullptr;
    }

    void vr_set_view_seat(SeatSource source)
    {
        g_seat_source = source;
    }

    bool vr_eye_to_game_view(int eye, Mat4* out)
    {
        if (!out || !g_eyes_valid || eye < 0 || eye > 1)
            return false;
        // The seat first: it sets the scale the eye and head are read at.
        Mat4 seat_to_view;
        float seat_scale = 1.0f;
        const bool seat = g_seat_source && g_seat_source(&seat_to_view, &seat_scale);
        if (g_seat_source)
            vr_set_scale_factor(seat ? seat_scale : 1.0f);
        const Mat4 eye_to_head = to_game(g_eyes[eye].eye_to_head);
        Mat4 head = Mat4::identity();
        vr_head_to_game_view(&head);
        if (seat)
        {
            *out = eye_to_head * head * seat_to_view;
            return true;
        }
        *out = eye_to_head * head * g_leveled_to_view;
        out->m[3][2] += g_view_forward;     // D3D view space: +z is forward
        return true;
    }

    bool vr_eye_fov_extent(float* up, float* down, float* side)
    {
        if (!g_eyes_valid)
            return false;
        float u = 0.0f, d = 0.0f, s = 0.0f;
        for (const XrEyeView& e : g_eyes)
        {
            // "top" is the down edge (negative), "bottom" the up edge.
            u = fmaxf(u, atanf(e.tan_bottom));
            d = fmaxf(d, atanf(-e.tan_top));
            s = fmaxf(s, fmaxf(atanf(-e.tan_left), atanf(e.tan_right)));
        }
        if (up) *up = u;
        if (down) *down = d;
        if (side) *side = s;
        return u > 0.0f && d > 0.0f && s > 0.0f;
    }

    bool vr_eye_offset_metres(int eye, float out[3])
    {
        if (!g_eyes_valid || eye < 0 || eye > 1)
            return false;
        const Mat34& p = g_eyes[eye].eye_to_head;
        out[0] = p.m[0][3];
        out[1] = p.m[1][3];
        out[2] = -p.m[2][3];
        return true;
    }

    Mat4 vr_eye_projection(int eye, float near_z, float far_z)
    {
        // xr_runtime's naming: tan_top is the *down* edge, tan_bottom the up.
        const XrEyeView& e = g_eyes[eye < 0 || eye > 1 ? 0 : eye];
        return d3d_projection(e.tan_left, e.tan_right, e.tan_top, e.tan_bottom, near_z, far_z);
    }
}
