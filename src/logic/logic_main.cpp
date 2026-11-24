// The logic DLL's side of the boundary (common/logic_abi.h): the host
// services stereo.cpp links against, forwarded to the shim, and the table of
// the stereo_* and god_* functions handed back to it.

#include "../common/logic_abi.h"
#include "../vr/stereo.h"
#include "../vr/vr_session.h"
#include "../proxy/game_psy.h"

#include <cstdarg>
#include <cstdio>

namespace
{
    const psyvr::LogicHost* g_host = nullptr;
}

namespace psyvr
{
    // What stereo.cpp calls, answered by the shim.
    const Config& config()
    {
        return *g_host->config();
    }

    void log(const char* fmt, ...)
    {
        char text[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(text, sizeof(text), fmt, args);
        va_end(args);
        text[sizeof(text) - 1] = 0;
        g_host->log(text);
    }

    bool hud_enabled()
    {
        return g_host->hud_enabled();
    }

    bool vr_eye_to_game_view(int eye, Mat4* out)
    {
        return g_host->vr_eye_to_game_view(eye, out);
    }

    void vr_view_forward(float* forward_units, float* near_limit_units)
    {
        g_host->vr_view_forward(forward_units, near_limit_units);
    }

    bool game_lock_target(float pos[3], float view[4][4])
    {
        return g_host->lock_target(pos, view);
    }

    bool game_theatre_eyes(TheatreEyes* out)
    {
        return g_host->theatre_eyes(out);
    }

    Mat4 vr_eye_projection(int eye, float near_z, float far_z)
    {
        Mat4 m = Mat4::identity();
        g_host->vr_eye_projection(eye, near_z, far_z, &m);
        return m;
    }

    namespace
    {
        void note_viewport(const D3DVIEWPORT9* vp)
        {
            stereo_note_viewport(*vp);
        }

        void god_frame_api(const GodFrameIn* in, GodSeatState* seat, GodFrameOut* out)
        {
            god_frame(*in, *seat, *out);
        }

        void god_seat_to_view(const float sp[3], const float sr[3][3], const float cp[3], const float cr[3][3],
                              Mat4* out)
        {
            *out = god_seat_to_game_view(sp, sr, cp, cr);
        }
    }
}

extern "C" bool psyvr_logic_create(const psyvr::LogicHost* host, psyvr::LogicApi* api)
{
    using namespace psyvr;
    if (!host || !api || !host->log)
        return false;
    if (host->abi_version != kLogicAbiVersion || host->config_size != sizeof(Config) ||
        host->mat4_size != sizeof(Mat4) ||
        host->god_size != sizeof(GodSeatState) + sizeof(GodFrameIn) + sizeof(GodFrameOut))
    {
        char text[200];
        snprintf(text, sizeof(text),
                 "logic: host does not match (abi %u/%u, Config %u/%u bytes, Mat4 %u/%u bytes, god %u/%u bytes)",
                 host->abi_version, kLogicAbiVersion, host->config_size, (unsigned)sizeof(Config),
                 host->mat4_size, (unsigned)sizeof(Mat4), host->god_size,
                 (unsigned)(sizeof(GodSeatState) + sizeof(GodFrameIn) + sizeof(GodFrameOut)));
        host->log(text);
        return false;
    }
    g_host = host;
    // Which build is running, so a reload can be told from a stale copy.
    host->log("logic: built " __DATE__ " " __TIME__);

    api->abi_version = kLogicAbiVersion;
    api->set_layout = stereo_set_layout;
    api->active = stereo_active;
    api->new_frame = stereo_new_frame;
    api->last_frame_had_scene = stereo_last_frame_had_scene;
    api->note_constants = stereo_note_constants;
    api->note_ps_constants = stereo_note_ps_constants;
    api->note_render_target = stereo_note_render_target;
    api->note_viewport = note_viewport;
    api->note_vertex_shader = stereo_note_vertex_shader;
    api->note_alpha_blend = stereo_note_alpha_blend;
    api->note_vertex_shader_created = stereo_note_vertex_shader_created;
    api->patch_vertex_shader = stereo_patch_vertex_shader;
    api->patch_pixel_shader = stereo_patch_pixel_shader;
    api->before_single_draw = stereo_before_single_draw;
    api->note_texture = stereo_note_texture;
    api->wants_vertex_box = stereo_wants_vertex_box;
    api->note_vertex_box = stereo_note_vertex_box;
    api->main_pass = stereo_main_pass;
    api->last_draw_kind = stereo_last_draw_kind;
    api->plan_draw = stereo_plan_draw;
    api->begin_eye = stereo_begin_eye;
    api->end = stereo_end;
    api->god_frame = god_frame_api;
    api->god_seat_to_view = god_seat_to_view;
    return true;
}

extern "C" void psyvr_logic_destroy()
{
    g_host = nullptr;
}
