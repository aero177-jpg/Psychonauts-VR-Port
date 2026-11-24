#include "god_seat.h"

#include <cmath>

namespace psyvr
{
    namespace
    {
        const float kRazMetres = 1.25f;     // his height, 125 units (docs/findings.md)

        // A rotation matrix's unit quaternion (x, y, z, w), Shepperd's method.
        void rot_to_quat(const float m[3][3], float q[4])
        {
            const float tr = m[0][0] + m[1][1] + m[2][2];
            if (tr > 0.0f)
            {
                const float s = sqrtf(tr + 1.0f) * 2.0f;
                q[3] = 0.25f * s;
                q[0] = (m[2][1] - m[1][2]) / s;
                q[1] = (m[0][2] - m[2][0]) / s;
                q[2] = (m[1][0] - m[0][1]) / s;
            }
            else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
            {
                const float s = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
                q[3] = (m[2][1] - m[1][2]) / s;
                q[0] = 0.25f * s;
                q[1] = (m[0][1] + m[1][0]) / s;
                q[2] = (m[0][2] + m[2][0]) / s;
            }
            else if (m[1][1] > m[2][2])
            {
                const float s = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
                q[3] = (m[0][2] - m[2][0]) / s;
                q[0] = (m[0][1] + m[1][0]) / s;
                q[1] = 0.25f * s;
                q[2] = (m[1][2] + m[2][1]) / s;
            }
            else
            {
                const float s = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
                q[3] = (m[1][0] - m[0][1]) / s;
                q[0] = (m[0][2] + m[2][0]) / s;
                q[1] = (m[1][2] + m[2][1]) / s;
                q[2] = 0.25f * s;
            }
        }
    }

    float god_seat_factor(const GodOptions& o)
    {
        const float cm = o.raz_height_cm > 0.5f ? o.raz_height_cm : 0.5f;
        return kRazMetres * 100.0f / cm;
    }

    void god_seat_direction(const float r[3][3], float away[3])
    {
        // Forward = R * (0, 0, -1) = -(column 2); away = -forward, level.
        away[0] = r[0][2];
        away[1] = 0.0f;
        away[2] = r[2][2];
        const float len = sqrtf(away[0] * away[0] + away[2] * away[2]);
        if (len > 1e-4f)
        {
            away[0] /= len;
            away[2] /= len;
        }
        else
        {
            away[0] = 0.0f;
            away[2] = 1.0f;
        }
    }

    void god_seat_leash(float anchor[3], const float raz[3], float radius_units, float height_units)
    {
        const float dx = raz[0] - anchor[0], dz = raz[2] - anchor[2];
        const float dist = sqrtf(dx * dx + dz * dz);
        if (radius_units <= 0.0f)
        {
            anchor[0] = raz[0];
            anchor[2] = raz[2];
        }
        else if (dist > radius_units)
        {
            const float f = (dist - radius_units) / dist;
            anchor[0] += dx * f;
            anchor[2] += dz * f;
        }
        const float dy = raz[1] - anchor[1];
        if (height_units <= 0.0f)
            anchor[1] = raz[1];
        else if (dy > height_units)
            anchor[1] = raz[1] - height_units;
        else if (dy < -height_units)
            anchor[1] = raz[1] + height_units;
    }

    void god_seat_update(GodSeatState& s, const float raz[3], const float cam_rot[3][3],
                         float turn, bool relatch, const GodOptions& o)
    {
        const float table = o.world_scale * god_seat_factor(o);
        if (s.have)
        {
            const float dx = raz[0] - s.anchor[0], dy = raz[1] - s.anchor[1], dz = raz[2] - s.anchor[2];
            const float far_ = 3.0f * table;
            if (!(dx * dx + dy * dy + dz * dz <= far_ * far_))
                s.have = false;
        }
        if (!s.have)
        {
            for (int k = 0; k < 3; ++k)
                s.anchor[k] = raz[k];
            god_seat_direction(cam_rot, s.away);
            s.have = true;
            return;
        }
        god_seat_leash(s.anchor, raz, o.leash * table, o.leash_height * table);
        if (turn != 0.0f && turn == turn)
        {
            // About +Y, taking away = (ax, 0, az) toward the seat's right
            // (az, 0, -ax) (god_seat_pose's X column); the centre turns about
            // Raz with it, so the leash is unchanged.
            const float c = cosf(turn), sn = sinf(turn);
            const float ax = s.away[0], az = s.away[2];
            s.away[0] = ax * c + az * sn;
            s.away[2] = az * c - ax * sn;
            const float dx = s.anchor[0] - raz[0], dz = s.anchor[2] - raz[2];
            s.anchor[0] = raz[0] + dx * c + dz * sn;
            s.anchor[2] = raz[2] + dz * c - dx * sn;
        }
        if (relatch)
            for (int k = 0; k < 3; ++k)
                s.anchor[k] = raz[k];
    }

    void god_seat_focus(const GodSeatState& s, const GodOptions& o, float out[3])
    {
        out[0] = s.anchor[0];
        out[1] = s.anchor[1] + o.focus_height * o.world_scale;
        out[2] = s.anchor[2];
    }

    void god_seat_pose(const GodSeatState& s, const GodOptions& o, float pos[3], float rot[3][3])
    {
        const float table = o.world_scale * god_seat_factor(o);
        const float zm = s.zoom > 0.0f ? s.zoom : 1.0f;
        float at[3];
        god_seat_focus(s, o, at);
        pos[0] = at[0] + s.away[0] * o.distance * zm * table;
        pos[1] = at[1] + o.height * zm * table;
        pos[2] = at[2] + s.away[2] * o.distance * zm * table;
        // Level, looking along -away: columns Z (backward) = away, Y = up,
        // X = Y x Z = (zz, 0, -zx).
        const float zx = s.away[0], zz = s.away[2];
        rot[0][0] = zz;   rot[0][1] = 0.0f; rot[0][2] = zx;
        rot[1][0] = 0.0f; rot[1][1] = 1.0f; rot[1][2] = 0.0f;
        rot[2][0] = -zx;  rot[2][1] = 0.0f; rot[2][2] = zz;
    }

    Mat4 god_seat_to_game_view(const float sp[3], const float sr[3][3],
                               const float cp[3], const float cr[3][3])
    {
        // v_game = S R^T Rs S v_seat + S R^T (sp - cp), column vectors; the
        // row-vector matrix is its transpose. S flips z.
        float a[3][3];      // R^T Rs
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                a[i][j] = cr[0][i] * sr[0][j] + cr[1][i] * sr[1][j] + cr[2][i] * sr[2][j];
        const float d[3] = { sp[0] - cp[0], sp[1] - cp[1], sp[2] - cp[2] };
        float t[3];         // R^T d
        for (int i = 0; i < 3; ++i)
            t[i] = cr[0][i] * d[0] + cr[1][i] * d[1] + cr[2][i] * d[2];
        Mat4 m = Mat4::identity();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
            {
                const float sign = ((i == 2) != (j == 2)) ? -1.0f : 1.0f;
                m.m[j][i] = sign * a[i][j];
            }
        m.m[3][0] = t[0];
        m.m[3][1] = t[1];
        m.m[3][2] = -t[2];
        return m;
    }

    void god_head_aim(const float sp[3], const float sr[3][3], const float hf[3], const float hu[3],
                      const float hp[3], float units_per_metre, float max_pitch, float reach, float eye[3],
                      float target[3])
    {
        // Room (left-handed, +z forward) -> seat space (the game's, -z
        // forward): flip z. Then the seat's rotation takes it to the world.
        float f[3] = { hf[0], hf[1], -hf[2] };
        float pitch = atan2f(f[1], sqrtf(f[0] * f[0] + f[2] * f[2]));
        pitch = fminf(fmaxf(pitch, -max_pitch), max_pitch);
        // The heading: the forward's horizontal part, steadied by the up
        // vector's as the forward nears vertical (looking down, the top of
        // the head points the way you face; looking up, away from it).
        if (hu)
        {
            const float w = f[1] * fabsf(f[1]);
            f[0] -= w * hu[0];
            f[2] -= w * -hu[2];
        }
        const float h = sqrtf(f[0] * f[0] + f[2] * f[2]);
        if (h > 1e-4f)
        {
            f[0] /= h;
            f[2] /= h;
        }
        else
        {
            f[0] = 0.0f;
            f[2] = -1.0f;
        }
        const float c = cosf(pitch);
        f[0] *= c;
        f[1] = sinf(pitch);
        f[2] *= c;
        const float p[3] = { hp[0] * units_per_metre, hp[1] * units_per_metre, -hp[2] * units_per_metre };
        for (int i = 0; i < 3; ++i)
        {
            eye[i] = sp[i] + sr[i][0] * p[0] + sr[i][1] * p[1] + sr[i][2] * p[2];
            target[i] = eye[i] + reach * (sr[i][0] * f[0] + sr[i][1] * f[1] + sr[i][2] * f[2]);
        }
    }

    float god_cull_fov_lua(float up, float down, float side, float margin, float aspect)
    {
        // Aimed at the middle of up and down, the view reaches half their sum
        // above and below; sideways the frustum is aspect x as wide (in
        // tangents), so the vertical angle may need to grow for the sides.
        float v = 0.5f * (up + down) + margin;
        const float s = side + margin;
        if (aspect > 0.1f && s < 1.55f)
            v = fmaxf(v, atanf(tanf(s) / aspect));
        v = fminf(v, 1.5533f);                  // 89 deg
        return 4.0f * v * 57.29578f;
    }

    void god_look_quat(const float from[3], const float to[3], float q[4])
    {
        // Columns: X right, Y up, Z backward (= -forward).
        float z[3] = { from[0] - to[0], from[1] - to[1], from[2] - to[2] };
        float n = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
        if (n < 1e-6f)
        {
            q[0] = q[1] = q[2] = 0.0f;
            q[3] = 1.0f;
            return;
        }
        for (float& c : z)
            c /= n;
        float x[3] = { z[2], 0.0f, -z[0] };        // (0, 1, 0) x Z
        n = sqrtf(x[0] * x[0] + x[2] * x[2]);
        if (n < 1e-6f)
        {
            x[0] = 1.0f;
            x[2] = 0.0f;
        }
        else
        {
            x[0] /= n;
            x[2] /= n;
        }
        const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
        const float m[3][3] = { { x[0], y[0], z[0] }, { x[1], y[1], z[1] }, { x[2], y[2], z[2] } };
        rot_to_quat(m, q);
    }

    void god_quat_to_rot(const float q[4], float m[3][3])
    {
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y - z * w);     m[0][2] = 2 * (x * z + y * w);
        m[1][0] = 2 * (x * y + z * w);     m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z - x * w);
        m[2][0] = 2 * (x * z - y * w);     m[2][1] = 2 * (y * z + x * w);     m[2][2] = 1 - 2 * (x * x + y * y);
    }

    float god_smoothstep(float t)
    {
        t = fminf(fmaxf(t, 0.0f), 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    void god_view_blend(const float p0[3], const float r0[3][3], float s0, const float p1[3],
                        const float r1[3][3], float s1, float t, float p[3], float r[3][3], float* s)
    {
        t = fminf(fmaxf(t, 0.0f), 1.0f);
        for (int k = 0; k < 3; ++k)
            p[k] = p0[k] + (p1[k] - p0[k]) * t;
        *s = (s0 > 0.0f && s1 > 0.0f) ? s0 * powf(s1 / s0, t) : s0 + (s1 - s0) * t;
        float a[4], b[4];
        rot_to_quat(r0, a);
        rot_to_quat(r1, b);
        float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        if (d < 0.0f)
        {
            for (float& c : b)
                c = -c;
            d = -d;
        }
        float wa, wb;
        if (d > 0.9995f)
        {
            wa = 1.0f - t;      // nearly the same: straight, normalised below
            wb = t;
        }
        else
        {
            const float th = acosf(fminf(d, 1.0f)), sn = sinf(th);
            wa = sinf((1.0f - t) * th) / sn;
            wb = sinf(t * th) / sn;
        }
        float q[4];
        float n = 0.0f;
        for (int k = 0; k < 4; ++k)
        {
            q[k] = wa * a[k] + wb * b[k];
            n += q[k] * q[k];
        }
        n = sqrtf(n);
        for (float& c : q)
            c /= n;
        god_quat_to_rot(q, r);
    }
}
