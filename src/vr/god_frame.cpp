#include "god_frame.h"

#include "../common/config.h"
#include "../common/log.h"

#include <windows.h>

#include <cmath>
#include <cstring>

namespace psyvr
{
    namespace
    {
        // [god] aim_with_head: the chase update sets the camera's quaternion
        // (+0x20) itself (4CAEF0's look-at FUN_004D1800 is not run for it),
        // so one along the head's gaze from the seat makes the game cull
        // (and draw) for where you look. The cull field of view (+0x30, Lua
        // units) is held at the headset's view plus this on every side: the
        // game's frustum test drops things at its very edge, and [vr]
        // cull_fov_degrees = 110 left a Quest 3 (54 deg down) 1 deg of room
        // at the bottom.
        const float kCullMargin = 10.0f * 0.01745329f;

        void add_write(GodFrameOut& out, unsigned offset, const void* data, unsigned size)
        {
            if (out.write_count >= kGodMaxWrites || size == 0 || size > sizeof(out.writes[0].bytes) ||
                offset + size > kGodWriteSpan)
                return;
            GodWrite& w = out.writes[out.write_count++];
            w.offset = offset;
            w.size = size;
            memcpy(w.bytes, data, size);
        }

        bool read_bytes(const char* camera, unsigned offset, void* out, unsigned size)
        {
            __try
            {
                memcpy(out, camera + offset, size);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // The right stick clicked in (Underworld's board gesture, user
        // 2026-11-15), for the session: Raz's size, e-folds a second at full
        // stick; the seat's height, table metres a second; the classic view's
        // height under horizon lock, metres a second, and its range.
        const float kSizeSpeed = 0.7f;
        const float kLiftSpeed = 0.3f;
        const float kLevelLiftSpeed = 0.8f;
        const float kLevelLiftDown = 1.0f, kLevelLiftUp = 2.0f;

        float shaped(float a)
        {
            a = fabsf(a) < 0.2f ? 0.0f : (a - (a > 0.0f ? 0.2f : -0.2f)) / 0.8f;
            return fminf(fmaxf(a, -1.0f), 1.0f);
        }

        // The settings, with the session's size and lift from the stick
        // (kept within the overlay's ranges).
        GodOptions god_options(const GodSeatState& seat)
        {
            const Config& c = config();
            GodOptions o;
            o.raz_height_cm = fminf(fmaxf(c.god_raz_height_cm * seat.size, 1.0f), 125.0f);
            o.distance = c.god_distance;
            o.height = fminf(fmaxf(c.god_height + seat.lift, -1.0f), 5.0f);
            o.leash = c.god_leash;
            o.leash_height = c.god_leash_height;
            o.world_scale = c.world_scale;
            return o;
        }

        // The right stick: x turns the seat round Raz, y zooms (up = nearer).
        // Clicked in: x = Raz's size (right = bigger), y = the seat's height
        // (up = higher); just let go, neither (the click nudges the stick).
        float stick(const GodFrameIn& in, float dt, GodSeatState& seat)
        {
            if (!in.pad_ok)
                return 0.0f;
            const float rx = shaped(in.stick_x), ry = shaped(in.stick_y);
            if (in.stick_clicked)
            {
                const float hc = config().god_raz_height_cm;
                const float lo = hc > 0.0f ? 1.0f / hc : 1.0f, hi = hc > 0.0f ? 125.0f / hc : 1.0f;
                seat.size = fminf(fmaxf(seat.size * expf(rx * kSizeSpeed * dt), lo), hi);
                const float h = config().god_height;
                seat.lift = fminf(fmaxf(seat.lift + ry * kLiftSpeed * dt, -1.0f - h), 5.0f - h);
                return 0.0f;
            }
            if (in.stick_settling)
                return 0.0f;
            seat.zoom = fminf(fmaxf(seat.zoom * expf(-ry * 1.0f * dt), 0.25f), 4.0f);
            // Right turns the view right: the seat moves to its left.
            return -rx * config().god_turn_speed * 0.01745329f * dt;
        }
    }

    namespace
    {
        // The game camera's heading, level (camera -> world, looking down -Z,
        // +Y up). Looking straight down or up keeps the last heading (in
        // seat.away; seat.have stays false, it is god mode's).
        void level_rot(const float cam_rot[3][3], GodSeatState& seat, float rot[3][3])
        {
            // Forward = R * (0, 0, -1) = -(third column).
            float hx = -cam_rot[0][2], hz = -cam_rot[2][2];
            const float h = sqrtf(hx * hx + hz * hz);
            if (h > 1e-3f)
            {
                seat.away[0] = hx / h;
                seat.away[1] = 0.0f;
                seat.away[2] = hz / h;
            }
            hx = seat.away[0];
            hz = seat.away[2];
            // Columns X = Y x Z, Y = up, Z = -forward.
            const float r[3][3] = { { -hz, 0.0f, -hx }, { 0.0f, 1.0f, 0.0f }, { hx, 0.0f, -hz } };
            memcpy(rot, r, sizeof(r));
        }

        // Classic view under horizon lock: the seat is the game camera's own
        // position, turned to its yaw only, life size.
        void level_seat(const GodFrameIn& in, GodSeatState& seat, GodFrameOut& out)
        {
            level_rot(in.cam_rot, seat, out.rot);
            memcpy(out.pos, in.cam_pos, sizeof(out.pos));
            // The right stick clicked in and pushed up / down raises or
            // lowers the view (its x is the camera's distance, game_psy.cpp).
            if (in.pad_ok && in.stick_clicked)
            {
                const float step = in.dt_ms > 0.0f && in.dt_ms < 100.0f ? in.dt_ms * 0.001f : 0.0f;
                seat.level_lift = fminf(fmaxf(seat.level_lift + shaped(in.stick_y) * kLevelLiftSpeed * step,
                                              -kLevelLiftDown), kLevelLiftUp);
            }
            out.pos[1] += seat.level_lift * config().world_scale;
            out.scale = 1.0f;
            seat.have = false;
        }

        // A plain view: what the draws show without a seat (the game camera,
        // levelled under horizon lock as update_horizon_lock does), so a
        // glide ends exactly where the plain path takes over.
        void plain_view(const GodFrameIn& in, GodSeatState& seat, GodFrameOut& out)
        {
            memcpy(out.pos, in.cam_pos, sizeof(out.pos));
            if (in.lock)
                level_rot(in.cam_rot, seat, out.rot);
            else
                memcpy(out.rot, in.cam_rot, sizeof(out.rot));
            out.scale = 1.0f;
            seat.have = false;
        }

        // The game camera onto the seat so it culls for the headset: at the
        // seat (aim_with_head off) or at the head on it, turned along the
        // gaze, with a cull field of view wide enough for the eyes.
        // `level`: the level seat or first person under horizon lock, which
        // always aim (else the stick's pitch, hidden by the lock, would
        // still steer the culling).
        void camera_writes(const GodFrameIn& in, GodFrameOut& out, bool level)
        {
            if (!level && !config().god_steer_camera)
                return;
            // With aim_with_head the camera sits at the head (on the seat) and is
            // turned along its gaze; else at the seat. The level seat always aims:
            // that is its point.
            float at[3] = { out.pos[0], out.pos[1], out.pos[2] };
            if (!level && !config().god_aim_with_head)
            {
                add_write(out, 0x08, at, 12);
                return;
            }
            float fwd[3] = { 0.0f, 0.0f, 1.0f }, up[3] = { 0.0f, 1.0f, 0.0f }, hp[3] = { 0.0f, 0.0f, 0.0f };
            if (in.head_ok)
                for (int k = 0; k < 3; ++k)
                {
                    fwd[k] = in.head.m[2][k];
                    up[k] = in.head.m[1][k];
                    hp[k] = in.head.m[3][k];
                }
            // Aimed at the middle of the eyes' view (headsets see further down
            // than up), wide enough for all of it.
            float cull_fov = 0.0f;
            if (in.fov_ok)
            {
                const float t = god_cull_pitch_offset(in.fov_up, in.fov_down);
                const float c = cosf(t), sn = sinf(t);
                for (int k = 0; k < 3; ++k)
                {
                    const float f = fwd[k], u = up[k];
                    fwd[k] = c * f + sn * u;
                    up[k] = c * u - sn * f;
                }
                int wh[2] = {};
                // 4CAEF0: width 640 means 4:3, else width / height.
                const float aspect = !read_bytes(in.camera, 0x40, wh, 8) ? 0.0f
                                     : wh[0] == 640                       ? 4.0f / 3.0f
                                     : wh[1] > 0                          ? (float)wh[0] / (float)wh[1]
                                                                          : 0.0f;
                if (aspect > 0.1f)
                    cull_fov = god_cull_fov_lua(in.fov_up, in.fov_down, in.fov_side, kCullMargin, aspect);
            }
            // 89 deg pitch at most; 1000 units of reach.
            float aim_target[3], aim_quat[4];
            god_head_aim(out.pos, out.rot, fwd, up, hp, config().world_scale * out.scale, 1.5533f, 1000.0f, at,
                         aim_target);
            god_look_quat(at, aim_target, aim_quat);
            add_write(out, 0x08, at, 12);
            add_write(out, 0x20, aim_quat, 16);
            float fov = 0.0f;
            if (cull_fov > 0.0f && read_bytes(in.camera, 0x30, &fov, 4) && fov > 1.0f && fov < cull_fov)
                add_write(out, 0x30, &cull_fov, 4);
        }

        // The view switch's glide ([controls] view_transition_seconds; as
        // the Underworld mod's): a change of view (god, classic, first
        // person) starts from what the eyes were shown last frame and eases
        // to the new view's live pose, so it follows Raz meanwhile. A switch
        // in the middle of a glide starts from where it is.
        void glide(const GodFrameIn& in, GodSeatState& seat, GodFrameOut& out)
        {
            const int kind = in.view * 2 + (in.first_person ? 1 : 0);
            const float secs = config().controls.view_transition_seconds;
            if (seat.view_kind >= 0 && kind != seat.view_kind)
            {
                if (secs > 0.0f)
                {
                    memcpy(seat.from_pos, seat.last_pos, sizeof(seat.from_pos));
                    memcpy(seat.from_rot, seat.last_rot, sizeof(seat.from_rot));
                    seat.from_scale = seat.last_scale;
                    seat.glide = 0.0f;
                }
                log("god: view %d%s -> %d%s, %s", seat.view_kind / 2, (seat.view_kind & 1) ? " (first person)" : "",
                    in.view, in.first_person ? " (first person)" : "", secs > 0.0f ? "glide" : "cut");
            }
            // A view reset glides too, from what the eyes were shown.
            if (in.reset && seat.view_kind >= 0 && kind == seat.view_kind && secs > 0.0f)
            {
                memcpy(seat.from_pos, seat.last_pos, sizeof(seat.from_pos));
                memcpy(seat.from_rot, seat.last_rot, sizeof(seat.from_rot));
                seat.from_scale = seat.last_scale;
                seat.glide = 0.0f;
            }
            seat.view_kind = kind;
            if (seat.glide < 1.0f)
            {
                const float dt = in.real_dt > 0.0f && in.real_dt < 0.1f ? in.real_dt : 0.0f;
                seat.glide = secs > 0.0f ? fminf(seat.glide + dt / secs, 1.0f) : 1.0f;
                if (seat.glide < 1.0f)
                {
                    float p[3], r[3][3], sc;
                    god_view_blend(seat.from_pos, seat.from_rot, seat.from_scale, out.pos, out.rot, out.scale,
                                   god_smoothstep(seat.glide), p, r, &sc);
                    memcpy(out.pos, p, sizeof(p));
                    memcpy(out.rot, r, sizeof(r));
                    out.scale = sc;
                    out.seat = true;
                }
            }
            memcpy(seat.last_pos, out.pos, sizeof(seat.last_pos));
            memcpy(seat.last_rot, out.rot, sizeof(seat.last_rot));
            seat.last_scale = out.scale;
        }
    }

    void god_frame(const GodFrameIn& in, GodSeatState& seat, GodFrameOut& out)
    {
        out.write_count = 0;
        out.seat = false;
        const bool level = in.view == kGodViewLevel;
        // A recentre (both stick clicks) is also the full reset of the
        // stick's session values, as in the Underworld mod.
        if (in.relatch && (seat.zoom != 1.0f || seat.size != 1.0f || seat.lift != 0.0f || seat.level_lift != 0.0f))
        {
            seat.zoom = seat.size = 1.0f;
            seat.lift = seat.level_lift = 0.0f;
            log("god: recentre: stick zoom, size and height reset");
        }
        // A view reset: the heights the stick clicked in added go back (not
        // the zoom or Raz's size); god mode's seat swings behind Raz, its
        // centre back on him.
        if (in.reset)
        {
            seat.lift = seat.level_lift = 0.0f;
            if (in.view == kGodViewGod && in.raz_fwd_ok)
            {
                const float fx = in.raz_fwd[0], fz = in.raz_fwd[2];
                const float h = sqrtf(fx * fx + fz * fz);
                if (h > 1e-3f)
                {
                    seat.away[0] = -fx / h;
                    seat.away[1] = 0.0f;
                    seat.away[2] = -fz / h;
                }
                if (seat.have)
                    for (int k = 0; k < 3; ++k)
                        seat.anchor[k] = in.raz[k];
            }
            log("god: view reset (view %d): behind Raz, height reset", in.view);
        }
        // One line when a click-and-push ends, with where it left things.
        static bool adjusting = false;
        if (adjusting && !in.stick_clicked)
        {
            if (level)
                log("god: classic view height %+.2f m", seat.level_lift);
            else if (in.view == kGodViewGod)
                log("god: Raz %.1f cm, seat height %.2f table m", fminf(fmaxf(config().god_raz_height_cm * seat.size, 1.0f), 125.0f),
                    config().god_height + seat.lift);
        }
        adjusting = in.view != kGodViewPlain && in.pad_ok && in.stick_clicked &&
                    (adjusting || shaped(in.stick_x) != 0.0f || shaped(in.stick_y) != 0.0f);
        if (in.view == kGodViewPlain)
        {
            plain_view(in, seat, out);
            // First person under horizon lock: the levelled camera becomes a
            // seat and the game camera is aimed along the head, so the
            // culling follows the eyes, not the stick's hidden pitch.
            if (in.first_person && in.lock)
            {
                out.seat = true;
                camera_writes(in, out, true);
            }
        }
        else if (level)
        {
            level_seat(in, seat, out);
            out.seat = true;
            camera_writes(in, out, true);
        }
        else
        {
            // dt is in milliseconds (18 at 60 fps, measured).
            const float step = in.dt_ms > 0.0f && in.dt_ms < 100.0f ? in.dt_ms * 0.001f : 0.0f;
            const float turn = stick(in, step, seat);
            const GodOptions o = god_options(seat);
            const bool was = seat.have;
            god_seat_update(seat, in.raz, in.cam_rot, turn, in.relatch, o);
            out.scale = god_seat_factor(o);
            god_seat_pose(seat, o, out.pos, out.rot);
            if (!was)
                log("god: seat on (Raz at %.0f %.0f %.0f, seat %.0f %.0f %.0f, x%.1f)", in.raz[0], in.raz[1],
                    in.raz[2], out.pos[0], out.pos[1], out.pos[2], out.scale);
            out.seat = true;
            camera_writes(in, out, false);
        }
        // After the writes: the game culls for the view being glided to.
        glide(in, seat, out);
    }
}
