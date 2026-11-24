// Included by mathtest.cpp: src/vr/god_seat.*
#include "../vr/god_seat.h"

static void test_god_seat()
{
    using namespace psyvr;
    GodOptions o;
    CHECK(near(god_seat_factor(o), 125.0f / 15.0f));
    const float table = o.world_scale * god_seat_factor(o);

    // A camera at yaw 0 looks down -Z: the seat lies along +Z.
    float ident[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    float away[3];
    god_seat_direction(ident, away);
    CHECK(near(away[0], 0) && near(away[1], 0) && near(away[2], 1));

    // Leash: inside the radius nothing moves; outside it follows to the edge.
    {
        float a[3] = { 0, 0, 0 };
        const float r1[3] = { 30, 5, 0 };
        god_seat_leash(a, r1, 50, 10);
        CHECK(near(a[0], 0) && near(a[1], 0) && near(a[2], 0));
        const float r2[3] = { 80, 25, 0 };
        god_seat_leash(a, r2, 50, 10);
        CHECK(near(a[0], 30) && near(a[1], 15) && near(a[2], 0));
    }

    // First update: centre on Raz, direction from the camera; the seat is
    // behind and above him, level, looking back along -away.
    {
        GodSeatState s;
        const float raz[3] = { 100, 20, -50 };
        god_seat_update(s, raz, ident, 0, false, o);
        CHECK(s.have && near(s.anchor[0], 100) && near(s.anchor[2], -50));
        float p[3], r[3][3];
        god_seat_pose(s, o, p, r);
        CHECK(near(p[0], 100));
        CHECK(near(p[1], 20 + o.focus_height * o.world_scale + o.height * table));
        CHECK(near(p[2], -50 + o.distance * table));
        // Forward (-Z column) points back toward -Z, i.e. at Raz's side.
        CHECK(near(-r[2][2], -1) && near(r[1][1], 1));

        // A turn swings the seat to its right, round Raz.
        GodSeatState t = s;
        god_seat_update(t, raz, ident, 0.1f, false, o);
        float right[3] = { r[0][0], r[1][0], r[2][0] };
        float q[3], qr[3][3];
        god_seat_pose(t, o, q, qr);
        CHECK((q[0] - p[0]) * right[0] + (q[2] - p[2]) * right[2] > 0.0f);

        // A far jump re-latches on Raz.
        const float far_[3] = { 100 + 10 * table, 20, -50 };
        god_seat_update(s, far_, ident, 0, false, o);
        CHECK(near(s.anchor[0], far_[0]));
    }

    // Seat == camera: identity. Seat 100 units behind (+Z world) a yaw-0
    // camera: a seat-view point at the origin is 100 units back (-z in D3D).
    {
        const float c[3] = { 5, 6, 7 };
        Mat4 m = god_seat_to_game_view(c, ident, c, ident);
        const Mat4 id = Mat4::identity();
        bool same = true;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                same = same && near(m.m[i][j], id.m[i][j]);
        CHECK(same);
        const float sp[3] = { 5, 6, 107 };
        m = god_seat_to_game_view(sp, ident, c, ident);
        CHECK(near(m.m[3][0], 0) && near(m.m[3][1], 0) && near(m.m[3][2], -100));
        // A seat turned 90 deg (looking down -X): its D3D forward (0,0,1)
        // lands on the game camera's left (-x).
        float yaw90[3][3];
        const float q[4] = { 0, sinf(0.7853982f), 0, cosf(0.7853982f) };
        god_quat_to_rot(q, yaw90);
        m = god_seat_to_game_view(c, yaw90, c, ident);
        CHECK(near(m.m[2][0], -1) && near(m.m[2][2], 0, 1e-3f));
    }

    // Head aim: straight ahead from the seat at yaw 0 looks down -Z; the
    // head's position is scaled and turned with the seat.
    {
        const float sp[3] = { 10, 20, 30 };
        const float fwd[3] = { 0, 0, 1 }, at0[3] = { 0, 0, 0 };
        float eye[3], tgt[3];
        god_head_aim(sp, ident, fwd, nullptr, at0, 100, 1.3f, 50, eye, tgt);
        CHECK(near(eye[0], 10) && near(eye[1], 20) && near(eye[2], 30));
        CHECK(near(tgt[0], 10) && near(tgt[1], 20) && near(tgt[2], -20));
        // Head 0.1 m right and 0.2 m forward (room); looking right.
        const float pos[3] = { 0.1f, 0, 0.2f }, right[3] = { 1, 0, 0 };
        god_head_aim(sp, ident, right, nullptr, pos, 100, 1.3f, 50, eye, tgt);
        CHECK(near(eye[0], 20) && near(eye[2], 10));
        CHECK(near(tgt[0], 70) && near(tgt[2], 10));
        // The seat turned 90 deg (looking down -X): ahead is -X.
        float yaw90[3][3];
        const float q[4] = { 0, sinf(0.7853982f), 0, cosf(0.7853982f) };
        god_quat_to_rot(q, yaw90);
        god_head_aim(sp, yaw90, fwd, nullptr, at0, 100, 1.3f, 50, eye, tgt);
        CHECK(near(tgt[0], -40, 1e-3f) && near(tgt[2], 30, 1e-3f));
        // Straight up is held at the pitch limit.
        const float up[3] = { 0, 1, 0 };
        god_head_aim(sp, ident, up, nullptr, at0, 100, 1.3f, 50, eye, tgt);
        CHECK(near(tgt[1] - 20, 50 * sinf(1.3f), 1e-3f) && near(tgt[2] - 30, -50 * cosf(1.3f), 1e-3f));
        // Looking almost straight down, turned 30 deg right: the heading
        // comes from the head's up vector, not from the tiny horizontal part
        // of its forward (which here points the other way).
        const float a = 0.5235988f;
        const float down_f[3] = { -0.001f * sinf(a), -1.0f, -0.001f * cosf(a) };
        const float down_u[3] = { sinf(a), 0.001f, cosf(a) };
        god_head_aim(sp, ident, down_f, down_u, at0, 100, 1.55f, 50, eye, tgt);
        CHECK(near(tgt[1] - 20, -50 * sinf(1.55f), 1e-3f));
        CHECK(near(tgt[0] - 10, 50 * cosf(1.55f) * sinf(a), 1e-3f) &&
              near(tgt[2] - 30, -50 * cosf(1.55f) * cosf(a), 1e-3f));
        // Level, the up vector changes nothing.
        const float up_level[3] = { 0, 1, 0 };
        god_head_aim(sp, ident, right, up_level, at0, 100, 1.3f, 50, eye, tgt);
        CHECK(near(tgt[0], 60) && near(tgt[1], 20) && near(tgt[2], 30));
    }

    // Cull field of view: 55 deg up and down plus 10 margin = 65 deg a half
    // -> 260 Lua units; Quest 3 (44 up, 54 down, 54 sideways) at 16:9 ->
    // 49 + 10 = 59 deg -> 236; a narrow 1:1 frustum grows for wide sides.
    {
        const float d = 0.01745329f;
        CHECK(near(god_cull_fov_lua(55 * d, 55 * d, 55 * d, 10 * d, 16.0f / 9.0f), 260, 0.05f));
        CHECK(near(god_cull_fov_lua(44 * d, 54 * d, 54 * d, 10 * d, 16.0f / 9.0f), 236, 0.05f));
        CHECK(near(god_cull_fov_lua(30 * d, 30 * d, 80 * d, 0, 1.0f), 320, 0.05f));
        CHECK(near(god_cull_pitch_offset(44 * d, 54 * d), -5 * d));
    }

    // Look quaternion: its forward (-Z column) points from -> to, level X.
    {
        const float from[3] = { 1, 2, 3 };
        const float tos[4][3] = { { 1, 2, -10 }, { 20, -5, 3 }, { -4, 9, 40 }, { 1, -30, 2 } };
        for (const auto& to : tos)
        {
            float q[4], r[3][3];
            god_look_quat(from, to, q);
            god_quat_to_rot(q, r);
            float d[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
            const float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            CHECK(near(-r[0][2], d[0] / n, 1e-3f) && near(-r[1][2], d[1] / n, 1e-3f) &&
                  near(-r[2][2], d[2] / n, 1e-3f));
            CHECK(near(r[1][0], 0, 1e-3f));     // no roll
        }
    }

    // View glide: the ends are the poses, half way is half the yaw, half the
    // way in position, and the geometric mean of the scales; the easing.
    {
        const float d = 0.01745329f;
        const float p0[3] = { 0, 0, 0 }, p1[3] = { 100, 50, -20 };
        float r0[3][3], r1[3][3];
        const float q0[4] = { 0, 0, 0, 1 };
        const float q1[4] = { 0, sinf(45 * d), 0, cosf(45 * d) };     // yaw 90
        god_quat_to_rot(q0, r0);
        god_quat_to_rot(q1, r1);
        float p[3], r[3][3], s;
        god_view_blend(p0, r0, 8.0f, p1, r1, 1.0f, 0.0f, p, r, &s);
        CHECK(near(p[0], 0) && near(s, 8.0f) && near(r[0][0], 1, 1e-4f) && near(r[0][2], 0, 1e-4f));
        god_view_blend(p0, r0, 8.0f, p1, r1, 1.0f, 1.0f, p, r, &s);
        CHECK(near(p[0], 100, 1e-3f) && near(p[1], 50, 1e-3f) && near(s, 1.0f, 1e-4f));
        CHECK(near(r[0][2], r1[0][2], 1e-4f) && near(r[2][0], r1[2][0], 1e-4f));
        god_view_blend(p0, r0, 8.0f, p1, r1, 1.0f, 0.5f, p, r, &s);
        CHECK(near(p[0], 50, 1e-3f) && near(p[2], -10, 1e-3f) && near(s, sqrtf(8.0f), 1e-3f));
        CHECK(near(r[0][0], cosf(45 * d), 1e-4f) && near(r[0][2], sinf(45 * d), 1e-4f) && near(r[1][1], 1, 1e-4f));
        // The shorter way round: q and -q are the same rotation.
        const float q1n[4] = { -q1[0], -q1[1], -q1[2], -q1[3] };
        float r1n[3][3];
        god_quat_to_rot(q1n, r1n);
        god_view_blend(p0, r0, 1.0f, p1, r1n, 1.0f, 0.5f, p, r, &s);
        CHECK(near(r[0][0], cosf(45 * d), 1e-4f));
        CHECK(near(god_smoothstep(0), 0) && near(god_smoothstep(0.5f), 0.5f) && near(god_smoothstep(1), 1) &&
              near(god_smoothstep(2), 1) && near(god_smoothstep(0.25f), 0.15625f));
    }
}
