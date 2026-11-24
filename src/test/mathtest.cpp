// Offline checks of the conversions the VR layer depends on (src/vr/vr_math).
// No game, no OpenXR runtime, no GPU. Run with test.bat; exits non-zero on
// the first failure so it can sit in a script.

#include "../vr/vr_math.h"
#include "../vr/pad_script.h"
#include "../vr/psy_stereo_math.h"
#include "../vr/psy_shader_scan.h"
#include "../vr/psy_theatre.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace psyvr;

namespace
{
    int g_checks = 0, g_failed = 0;

    void check(bool ok, const char* what, const char* file, int line)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failed;
            std::printf("FAIL  %s  (%s:%d)\n", what, file, line);
        }
    }
#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)

    bool near(float a, float b, float eps = 1e-4f)
    {
        return std::fabs(a - b) <= eps * (1.0f + std::fabs(b));
    }

    // A pose: yaw (about y), pitch (about x) and roll (about z), applied
    // roll, then pitch, then yaw, like a head; translation in metres.
    // 3x4 column-vector, right-handed (OpenXR's layout).
    void make_pose(float yaw, float pitch, float roll, float tx, float ty, float tz, float p[3][4])
    {
        const float cy = std::cos(yaw), sy = std::sin(yaw);
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        const float cr = std::cos(roll), sr = std::sin(roll);
        const float Ry[3][3] = { { cy, 0, sy }, { 0, 1, 0 }, { -sy, 0, cy } };
        const float Rx[3][3] = { { 1, 0, 0 }, { 0, cp, -sp }, { 0, sp, cp } };
        const float Rz[3][3] = { { cr, -sr, 0 }, { sr, cr, 0 }, { 0, 0, 1 } };
        float t[3][3], r[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
            {
                t[i][j] = Rx[i][0] * Rz[0][j] + Rx[i][1] * Rz[1][j] + Rx[i][2] * Rz[2][j];
            }
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r[i][j] = Ry[i][0] * t[0][j] + Ry[i][1] * t[1][j] + Ry[i][2] * t[2][j];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                p[i][j] = r[i][j];
        p[0][3] = tx; p[1][3] = ty; p[2][3] = tz;
    }

    void apply_pose(const float p[3][4], const float v[3], float out[3])
    {
        for (int i = 0; i < 3; ++i)
            out[i] = p[i][0] * v[0] + p[i][1] * v[1] + p[i][2] * v[2] + p[i][3];
    }

    // a * b for 3x4 column-vector rigid transforms.
    void compose(const float a[3][4], const float b[3][4], float out[3][4])
    {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
            {
                float v = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
                if (c == 3)
                    v += a[r][3];
                out[r][c] = v;
            }
    }

    void row_vector_apply(const Mat4& m, const float v[3], float out[3])
    {
        for (int c = 0; c < 3; ++c)
            out[c] = v[0] * m.m[0][c] + v[1] * m.m[1][c] + v[2] * m.m[2][c] + m.m[3][c];
    }

    void test_invert()
    {
        Mat4 a = Mat4::identity();
        a.m[0][0] = 0.8f; a.m[0][1] = -0.6f; a.m[1][0] = 0.6f; a.m[1][1] = 0.8f;
        a.m[3][0] = 3.0f; a.m[3][1] = -2.0f; a.m[3][2] = 7.5f;
        Mat4 inv{};
        CHECK(invert(a, &inv));
        const Mat4 id = a * inv;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                CHECK(near(id.m[r][c], r == c ? 1.0f : 0.0f));

        Mat4 singular{};                       // all zeros
        Mat4 out = Mat4::identity();
        CHECK(!invert(singular, &out));
    }

    // The defining property of game_from_xr_pose: mirror(pose * p) ==
    // mirror(p) * game, for any pose and point, with the world scale applied
    // to the translation only.
    void test_pose_mirror()
    {
        const float scale = 100.0f;
        const float poses[][6] = {
            { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
            { 0.5f, 0.0f, 0.0f, 0.0f, 1.7f, 0.0f },
            { -1.2f, 0.3f, 0.1f, 0.25f, 1.6f, -0.4f },
            { 2.9f, -0.7f, 0.5f, -1.0f, 0.0f, 2.0f },
        };
        const float points[][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0.3f, -0.2f, 0.9f } };
        for (const auto& pp : poses)
        {
            float pose[3][4];
            make_pose(pp[0], pp[1], pp[2], pp[3], pp[4], pp[5], pose);
            const Mat4 g = game_from_xr_pose(pose, scale);

            // a proper rotation: determinant +1, rows orthonormal
            const float det =
                g.m[0][0] * (g.m[1][1] * g.m[2][2] - g.m[1][2] * g.m[2][1]) -
                g.m[0][1] * (g.m[1][0] * g.m[2][2] - g.m[1][2] * g.m[2][0]) +
                g.m[0][2] * (g.m[1][0] * g.m[2][1] - g.m[1][1] * g.m[2][0]);
            CHECK(near(det, 1.0f));
            CHECK(near(g.m[3][3], 1.0f));

            for (const auto& p : points)
            {
                float world[3];
                apply_pose(pose, p, world);             // OpenXR world point
                const float mirrored_local[3] = { p[0], p[1], -p[2] };
                float game_world[3];
                row_vector_apply(g, mirrored_local, game_world);
                // translation was scaled, so compare with the scale applied
                // to the world point's translation part only: reconstruct
                // by scaling (world - R p) by `scale`.
                float rotated[3];
                float pure_rot[3][4];
                std::memcpy(pure_rot, pose, sizeof(pure_rot));
                pure_rot[0][3] = pure_rot[1][3] = pure_rot[2][3] = 0.0f;
                apply_pose(pure_rot, p, rotated);
                const float want[3] = {
                    rotated[0] + pose[0][3] * scale,
                    rotated[1] + pose[1][3] * scale,
                    -(rotated[2] + pose[2][3] * scale) };
                CHECK(near(game_world[0], want[0]));
                CHECK(near(game_world[1], want[1]));
                CHECK(near(game_world[2], want[2]));
                (void)world;
            }
        }

        // Concrete: no rotation, a head 1.7 m up and 0.5 m forward (-z in
        // OpenXR) is 170 up and 50 forward (+z) in the game's space.
        float pose[3][4];
        make_pose(0, 0, 0, 0.0f, 1.7f, -0.5f, pose);
        const Mat4 g = game_from_xr_pose(pose, 100.0f);
        CHECK(near(g.m[3][0], 0.0f));
        CHECK(near(g.m[3][1], 170.0f));
        CHECK(near(g.m[3][2], 50.0f));
    }

    // Recentring: whatever the head's yaw and position, after the origin is
    // taken from it the head is at the origin looking forward; and a later
    // turn of +10 degrees reads as +10, not -10.
    void test_recentre()
    {
        float head[3][4];
        make_pose(0.8f, 0.0f, 0.0f, 0.4f, 1.6f, -0.3f, head);
        const float yaw = xr_pose_yaw(head);
        CHECK(near(yaw, 0.8f));

        const float pos[3] = { head[0][3], head[1][3], head[2][3] };
        float inv[3][4], rel[3][4];
        xr_recentre_inverse(yaw, pos, inv);
        compose(inv, head, rel);
        // the whole position goes to the origin (the game camera is at
        // Lara's head, so the head's own height is not added on top)
        CHECK(near(rel[0][3], 0.0f, 1e-3f));
        CHECK(near(rel[1][3], 0.0f, 1e-3f));
        CHECK(near(rel[2][3], 0.0f, 1e-3f));
        CHECK(near(xr_pose_yaw(rel), 0.0f, 1e-3f));

        // the head turns 10 degrees from there
        float later[3][4];
        make_pose(0.8f + 0.1745329f, 0.0f, 0.0f, 0.4f, 1.6f, -0.3f, later);
        compose(inv, later, rel);
        CHECK(near(xr_pose_yaw(rel), 0.1745329f, 1e-3f));
    }

    void test_camera_attached()
    {
        using namespace psyvr;
        CHECK(is_camera_attached(16.03f, 16.0f));       // the underwater quad
        CHECK(is_camera_attached(16.0f, 16.0f));        // the camcorder quad
        CHECK(!is_camera_attached(616.0f, 16.0f));      // the nearest particle seen
        CHECK(!is_camera_attached(2400.0f, 16.0f));
        CHECK(!is_camera_attached(16.5f, 16.0f));       // 3% past near
        CHECK(!is_camera_attached(NAN, 16.0f));
        CHECK(!is_camera_attached(16.0f, NAN));
        CHECK(!is_camera_attached(16.0f, 0.0f));
        CHECK(!is_camera_attached(16.0f, -1.0f));
        CHECK(!is_camera_attached(0.0f, 16.0f));
        CHECK(!is_camera_attached(-5.0f, 16.0f));
    }

    void test_projection()
    {
        test_camera_attached();
        // 90 degree symmetric: tangents +-1.
        const Mat4 p = d3d_projection(-1.0f, 1.0f, -1.0f, 1.0f, 10.0f, 1000.0f);
        CHECK(near(p.m[0][0], 1.0f));
        CHECK(near(p.m[1][1], 1.0f));
        CHECK(near(p.m[2][3], 1.0f));
        CHECK(near(p.m[3][3], 0.0f));

        auto project = [&](const Mat4& m, float x, float y, float z, float out[3]) {
            const float cx = x * m.m[0][0] + y * m.m[1][0] + z * m.m[2][0] + m.m[3][0];
            const float cy = x * m.m[0][1] + y * m.m[1][1] + z * m.m[2][1] + m.m[3][1];
            const float cz = x * m.m[0][2] + y * m.m[1][2] + z * m.m[2][2] + m.m[3][2];
            const float cw = x * m.m[0][3] + y * m.m[1][3] + z * m.m[2][3] + m.m[3][3];
            out[0] = cx / cw; out[1] = cy / cw; out[2] = cz / cw;
        };
        float o[3];
        project(p, 0, 0, 10.0f, o);   CHECK(near(o[2], 0.0f));      // near plane -> 0
        project(p, 0, 0, 1000.0f, o); CHECK(near(o[2], 1.0f));      // far plane -> 1
        project(p, 50, 0, 50.0f, o);  CHECK(near(o[0], 1.0f));      // on the right edge
        project(p, 0, -50, 50.0f, o); CHECK(near(o[1], -1.0f));     // on the bottom edge

        // Asymmetric (a headset's eye): left 1.4, right 0.8, down 1.4, up 1.0.
        const Mat4 q = d3d_projection(-1.4f, 0.8f, -1.4f, 1.0f, 0.1f, 100.0f);
        project(q, -1.4f * 5, 0, 5.0f, o); CHECK(near(o[0], -1.0f));   // left edge
        project(q, 0.8f * 5, 0, 5.0f, o);  CHECK(near(o[0], 1.0f));    // right edge
        project(q, 0, 1.0f * 5, 5.0f, o);  CHECK(near(o[1], 1.0f));    // top edge
        project(q, 0, -1.4f * 5, 5.0f, o); CHECK(near(o[1], -1.0f));   // bottom edge
    }
}

namespace
{
    void test_pad_script()
    {
        std::vector<PadStep> steps;
        std::vector<std::string> errors;
        pad_script_parse("# walk\r\n2 ly=1\r\n\r\n1\r\n1.5 RX=-0.5 a Up lt=1 # turn\r\n", steps, errors);
        CHECK(errors.empty());
        CHECK(steps.size() == 3);
        CHECK(steps[0].seconds == 2.0f && steps[0].set == PadSetLY && steps[0].ly == 1.0f);
        CHECK(steps[0].text == "ly=1");
        CHECK(steps[1].set == 0 && steps[1].buttons == 0);          // a pause
        CHECK(steps[2].set == (PadSetRX | PadSetLT) && steps[2].rx == -0.5f);
        CHECK(steps[2].buttons == (0x1000 | 0x0001));               // A, d-pad up
        CHECK(near(pad_script_seconds(steps), 4.5f));

        // Bad lines are reported and skipped, the good ones kept.
        steps.clear();
        errors.clear();
        pad_script_parse("x ly=1\n0 a\n601\n1 ly=2\n1 lt=-1\n1 jump\n1 zz=1\n1 ly=\n3 b\n", steps, errors);
        CHECK(errors.size() == 8);
        CHECK(steps.size() == 1 && steps[0].buttons == 0x2000);

        // The step limit.
        std::string many;
        for (int i = 0; i < 250; ++i) many += "1\n";
        steps.clear();
        errors.clear();
        pad_script_parse(many, steps, errors);
        CHECK(steps.size() == kPadScriptMaxSteps && errors.size() == 1);
    }
}

// Psychonauts: recover the projection from c6..c9 alone, and the
    // eye matrix with an identity eye gives back the game's own WVP.
    void test_psy_near_limit()
    {
        // Start menu at world scale 500: eyes 750 ahead, limit 1000.
        CHECK(near(psy_near_limit_shift(750.0f, 1000.0f, 2703.0f), 0.0f));      // the brain: as zoomed
        CHECK(near(psy_near_limit_shift(750.0f, 1000.0f, 1349.0f), -401.0f));   // a doodle: kept 1000 away
        CHECK(near(psy_near_limit_shift(750.0f, 1000.0f, 869.0f), -881.0f));    // a credit bubble: behind the camera
        CHECK(near(psy_near_limit_shift(750.0f, 1000.0f, 1.0f), -1749.0f));     // just ahead of the camera
        CHECK(near(psy_near_limit_shift(750.0f, 1000.0f, 0.0f), 0.0f));         // drawn round the camera (sky)
        CHECK(near(psy_near_limit_shift(750.0f, 0.0f, 869.0f), 0.0f));          // off
        CHECK(near(psy_near_limit_shift(0.0f, 1000.0f, 5000.0f), 0.0f));        // no zoom, far
    }

    void test_psy_projection()
    {
        PsyProjection p;
        p.sx = 1.2f; p.sy = 2.1f; p.q = 1.0005f; p.near_z = 10.0f;
        p.q32 = -p.q * p.near_z;
        const Mat4 proj = psy_projection_matrix(p);
        Mat4 world = yaw_rotation(0.7f);                 // rigid, uniform scale 3
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                world.m[r][c] *= 3.0f;
        world.m[3][0] = 120.0f; world.m[3][1] = -40.0f; world.m[3][2] = 900.0f;
        Mat4 view = yaw_rotation(-0.2f);
        view.m[3][0] = 5.0f; view.m[3][1] = 2.0f; view.m[3][2] = 300.0f;
        const Mat4 wvp = world * view * proj;
        float regs[4][4];
        psy_regs_from_wvp(wvp, regs);
        PsyProjection got;
        bool clean = false;
        CHECK(psy_projection_from_regs(regs, &got, &clean));
        CHECK(clean);
        CHECK(near(got.sx, p.sx) && near(got.sy, p.sy));
        CHECK(fabsf(got.near_z - p.near_z) < 1e-2f);
        CHECK(!psy_is_orthographic(regs));
        const Mat4 back = wvp * psy_projection_inverse(got) * psy_projection_matrix(got);
        bool same = true;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                same = same && fabsf(back.m[r][c] - wvp.m[r][c]) < 1e-2f * (1.0f + fabsf(wvp.m[r][c]));
        CHECK(same);
        // Non-uniform world scale: not clean, depth terms still right.
        world.m[0][0] *= 2.0f; world.m[0][2] *= 2.0f;
        psy_regs_from_wvp(world * view * proj, regs);
        CHECK(psy_projection_from_regs(regs, &got, &clean));
        CHECK(!clean);
        CHECK(fabsf(got.near_z - p.near_z) < 1e-2f);
        // Main scene shape: q < 1 (no far plane), near 10.
        {
            PsyProjection m;
            m.sx = 1.53773f; m.sy = 2.0503f; m.q = 0.990198f; m.near_z = 10.0f; m.q32 = -m.q * 10.0f;
            psy_regs_from_wvp(view * psy_projection_matrix(m), regs);
            CHECK(psy_projection_from_regs(regs, &got, &clean));
            CHECK(fabsf(got.near_z - 10.0f) < 1e-2f && got.far_z > 1e20f);
        }
        // Orthographic (HUD): w constant.
        Mat4 ortho = Mat4::identity();
        ortho.m[0][0] = 2.0f / 640.0f; ortho.m[1][1] = -2.0f / 480.0f;
        psy_regs_from_wvp(ortho, regs);
        CHECK(psy_is_orthographic(regs));
    }
// Psychonauts shader scan: billboard constants followed to oPos.
    void test_psy_shader_scan()
    {
        auto reg = [](uint32_t type, uint32_t num) {
            return 0x80000000u | ((type & 7u) << 28) | (((type >> 3) & 3u) << 11) | num;
        };
        const uint32_t T = 0, V = 1, C = 2, O = 4;
        // ParticleBB (PC): m4x4 r0,v0,c6; mad r7,v13,c10,r0; mad r7,v13,c14,r7; mov r4,r7; add oPos,r4,c50
        const uint32_t bb[] = { 0xFFFE0101u,
            0x14, reg(T, 0), reg(V, 0), reg(C, 6),
            0x04, reg(T, 7), reg(V, 13), reg(C, 10), reg(T, 0),
            0x04, reg(T, 7), reg(V, 13), reg(C, 14), reg(T, 7),
            0x01, reg(T, 4), reg(T, 7),
            0x02, reg(O, 0), reg(T, 4), reg(C, 50),
            0x0000FFFFu };
        PsyShaderInfo i = psy_scan_shader(bb);
        CHECK(i.known && i.reads_wvp && i.billboard == ((1ull << 10) | (1ull << 14)));
        // ZSprite: corners c24/c25; c10 (view-proj) and c18 used elsewhere.
        const uint32_t zs[] = { 0xFFFE0101u,
            0x14, reg(T, 0), reg(V, 0), reg(C, 6),
            0x04, reg(T, 0), reg(V, 13), reg(C, 24), reg(T, 0),
            0x04, reg(T, 4), reg(V, 13), reg(C, 25), reg(T, 0),
            0x02, reg(O, 0), reg(T, 4), reg(C, 50),
            0x14, reg(T, 1), reg(T, 5), reg(C, 10),
            0x04, reg(T, 5), reg(T, 5), reg(C, 18), reg(T, 9),
            0x0000FFFFu };
        i = psy_scan_shader(zs);
        CHECK(i.billboard == ((1ull << 24) | (1ull << 25)));
        CHECK(i.reads_c10);
        // CheapWater: c15 from the clip position, but not to oPos.
        const uint32_t cw[] = { 0xFFFE0101u,
            0x14, reg(T, 7), reg(V, 0), reg(C, 10),
            0x14, reg(T, 0), reg(V, 0), reg(C, 6),
            0x01, reg(O, 0), reg(T, 0),
            0x04, reg(T, 1), reg(T, 0), reg(C, 15), reg(T, 0),
            0x0000FFFFu };
        i = psy_scan_shader(cw);
        CHECK(i.reads_wvp && i.billboard == 0);
        // Plain geometry: no billboard.
        const uint32_t geo[] = { 0xFFFE0101u, 0x14, reg(T, 4), reg(V, 0), reg(C, 6),
                                 0x02, reg(O, 0), reg(T, 4), reg(C, 50), 0x0000FFFFu };
        i = psy_scan_shader(geo);
        CHECK(i.reads_wvp && i.billboard == 0);

        // Water (PC): the reflection coordinates are moved onto the game's
        // WVP (c56), oPos stays on c6.
        const uint32_t water[] = { 0xFFFE0101u,
            0x14, reg(T, 7), reg(V, 0), reg(C, 10),
            0x14, reg(T, 0), reg(V, 0), reg(C, 6),
            0x01, reg(T, 4), reg(T, 0),
            0x02, reg(O, 0), reg(T, 4), reg(C, 50),
            0x04, reg(T, 1), reg(T, 0) | 0x00FF0000u, reg(C, 15), reg(T, 0),
            0x05, reg(T, 2), reg(T, 1), reg(C, 15),
            0x0000FFFFu };
        uint32_t out[64] = {};
        size_t len = 0;
        CHECK(psy_patch_screen_coords(water, out, 64, &len));
        const uint32_t want[] = { 0xFFFE0101u,
            0x14, reg(T, 7), reg(V, 0), reg(C, 10),
            0x14, reg(T, 0), reg(V, 0), reg(C, 6),
            0x14, reg(T, 3), reg(V, 0), reg(C, 56),
            0x01, 0xE00F0004u, 0x80E40003u,                 // mov oT4, r3
            0x01, reg(T, 4), reg(T, 0),
            0x02, reg(O, 0), reg(T, 4), reg(C, 50),
            0x04, reg(T, 1), reg(T, 3) | 0x00FF0000u, reg(C, 15), reg(T, 3),
            0x05, reg(T, 2), reg(T, 1), reg(C, 15),
            0x0000FFFFu };
        CHECK(len == sizeof(want) / 4 && memcmp(out, want, sizeof(want)) == 0);
        i = psy_scan_shader(out);
        CHECK(i.reads_game_wvp && i.reads_wvp && i.billboard == 0);
        // Not water: no change; too little room: no change.
        CHECK(!psy_patch_screen_coords(bb, out, 64, &len));
        CHECK(!psy_patch_screen_coords(geo, out, 64, &len));
        CHECK(!psy_patch_screen_coords(water, out, 10, &len));
        // oT4 already written: no change.
        uint32_t water_t4[sizeof(water) / 4 + 3];
        memcpy(water_t4, water, sizeof(water) - 4);
        const uint32_t t4[] = { 0x01, 0xE00F0004u, reg(T, 7), 0x0000FFFFu };
        memcpy(water_t4 + sizeof(water) / 4 - 1, t4, sizeof(t4));
        CHECK(!psy_patch_screen_coords(water_t4, out, 64, &len));
    }
#include "dpad_gesture_tests.inc"
#include "god_seat_tests.inc"

namespace
{
    // Theatre (psy_theatre.h): where a point straight ahead of the game camera
    // at `z_units` lands on the screen for one eye, metres from its centre.
    float theatre_on_screen(const TheatreEyes& t, int eye, float sy, float z_units, float half_w)
    {
        const Mat4 proj = theatre_projection(sy, t.aspect, t.ndc_shift[eye], 1.0f, -1.0f);
        const Mat4 view = translation(-t.x_units[eye], 0.0f, 0.0f);
        const Mat4 m = view * proj;
        // The point (0, 0, z, 1) through view and projection (row vectors).
        const float cx = z_units * m.m[2][0] + m.m[3][0];
        const float cw = z_units * m.m[2][3] + m.m[3][3];
        return cx / cw * half_w;
    }

    void test_theatre()
    {
        CHECK(!theatre_wanted(0, true, false));
        CHECK(theatre_wanted(1, true, false));
        CHECK(!theatre_wanted(1, false, false));
        CHECK(!theatre_wanted(1, true, true));      // the start menu's camera
        CHECK(theatre_wanted(2, false, false));
        CHECK(theatre_wanted(2, false, true));

        const float ipd = 0.064f, ws = 100.0f, depth = 1.0f;
        const TheatreWindowSize w = theatre_window_size(2.5f, 70.0f, 1.78f);
        const float x[2] = { -ipd * 0.5f, ipd * 0.5f };
        const TheatreEyes t = theatre_eyes(x, ws, depth, w.half_w, 1.78f);
        CHECK(near(t.x_units[0], -3.2f) && near(t.x_units[1], 3.2f));
        CHECK(t.ndc_shift[0] < 0.0f && near(t.ndc_shift[0], -t.ndc_shift[1]));
        // The screen as tall as the game's view: sy = distance / half height.
        const float sy = w.distance / w.half_h;
        // Infinity: one eye separation apart on the screen, uncrossed.
        const float inf_l = theatre_on_screen(t, 0, sy, 1e9f, w.half_w);
        const float inf_r = theatre_on_screen(t, 1, sy, 1e9f, w.half_w);
        CHECK(near(inf_l, -ipd * 0.5f, 1e-3f) && near(inf_r, ipd * 0.5f, 1e-3f));
        // Life size: a point at the screen's distance is on the screen (no
        // disparity), and one twice as far looks twice as far.
        const float d = w.distance;
        CHECK(near(theatre_on_screen(t, 0, sy, d * ws, w.half_w), theatre_on_screen(t, 1, sy, d * ws, w.half_w),
                   1e-4f));
        const float disp = theatre_on_screen(t, 1, sy, 2.0f * d * ws, w.half_w) -
                           theatre_on_screen(t, 0, sy, 2.0f * d * ws, w.half_w);
        CHECK(near(d * ipd / (ipd - disp), 2.0f * d, 1e-3f));
        // Window: from the anchor, the game's frame edge lands on the
        // window's edge after the warp, at any depth; depth is kept.
        {
            const float game_sy = 1.0f / tanf(26.0f * 0.01745329f);    // 52 deg
            const float fs = theatre_fit_scale(w.half_h, w.distance, game_sy);
            const Mat4 warp = theatre_fit_warp(fs);
            const float z = 700.0f, y_edge = z / game_sy;               // on the game's top edge
            const float y_w = y_edge * warp.m[1][1], z_w = z * warp.m[2][2];
            CHECK(near(y_w / z_w, w.half_h / w.distance, 1e-4f));
            CHECK(near(z_w, z));
            CHECK(near(theatre_fit_scale(1.0f, 0.0f, 2.0f), 1.0f));     // bad input
            CHECK(near(theatre_fit_scale(100.0f, 1.0f, 2.0f), 10.0f));  // clamped
        }
        // Depth 0: flat on the screen, infinity included.
        const TheatreEyes flat = theatre_eyes(x, ws, 0.0f, w.half_w, 1.78f);
        CHECK(near(theatre_on_screen(flat, 0, sy, 1e9f, w.half_w), 0.0f, 1e-4f));
        CHECK(near(theatre_on_screen(flat, 1, sy, 300.0f, w.half_w), 0.0f, 1e-4f));
        // Depth 2: infinity no further apart than the eyes (no divergence).
        const TheatreEyes deep = theatre_eyes(x, ws, 2.0f, w.half_w, 1.78f);
        CHECK(near(deep.x_units[1], 6.4f));
        CHECK(near(theatre_on_screen(deep, 1, sy, 1e9f, w.half_w) - theatre_on_screen(deep, 0, sy, 1e9f, w.half_w),
                   ipd, 1e-3f));
    }
}

int main()
{
    test_theatre();
    test_pad_script();
    test_dpad_gesture();
    test_god_seat();
    test_invert();
    test_pose_mirror();
    test_recentre();
    test_projection();
    test_psy_projection();
    test_psy_near_limit();
    test_psy_shader_scan();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
