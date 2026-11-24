#pragma once
// The boundary between the shim (d3d9.dll) and the logic DLL (psyvr_logic.dll).
//
// The shim owns everything that must survive a rebuild: the D3D device and
// every resource, the OpenXR session, the config, the log, the game hooks.
// The logic DLL owns the draw classification and shader patching
// (stereo.cpp) and god mode's per-frame camera logic (god_frame.cpp,
// god_seat.cpp; the shim keeps the hook, the seat state and the camera
// writes) and nothing else, so it can be rebuilt and swapped
// while the game runs (proxy/logic_host.h). It never owns a D3D resource that
// outlives it; what it knows of the device it is told through the stereo_*
// calls, and the shim replays that state into a freshly loaded copy.
//
// Function pointers only, plain data, no STL, no exceptions and no heap
// ownership crossing the boundary. Bump kLogicAbiVersion whenever LogicHost
// or LogicApi changes, or the layout of Config, Mat4, GodSeatState or
// god_frame.h's structs does: the logic DLL refuses a host it does not match.
// Strings the logic returns (stereo_last_draw_kind) are valid only until the
// next reload.

#include "config.h"
#include "../vr/god_frame.h"
#include "../vr/psy_theatre.h"
#include "../vr/vr_math.h"

#include <d3d9.h>

#include <cstddef>

namespace psyvr
{
    const unsigned kLogicAbiVersion = 26;       // 26: Config without stereo, head_tracking, water_reflection_scale, glow_detail

    // The shim -> the logic.
    struct LogicHost
    {
        unsigned abi_version;
        unsigned config_size;       // sizeof(Config)
        unsigned mat4_size;         // sizeof(Mat4)
        unsigned god_size;          // sizeof(GodSeatState) + sizeof(GodFrameIn) + sizeof(GodFrameOut)
        void (*log)(const char* text);      // already formatted
        const Config* (*config)();
        bool (*hud_enabled)();
        bool (*vr_eye_to_game_view)(int eye, Mat4* out);
        void (*vr_eye_projection)(int eye, float near_z, float far_z, Mat4* out);
        void (*vr_view_forward)(float* forward_units, float* near_limit_units);
        // game_psy.h: game_lock_target (lock-on held: the target and the
        // camera's world -> view).
        bool (*lock_target)(float pos[3], float view[4][4]);
        // game_psy.h: game_theatre_eyes (the theatre shows: its virtual
        // eyes, psy_theatre.h).
        bool (*theatre_eyes)(TheatreEyes* out);
    };

    // The logic -> the shim: stereo.h's functions, one each.
    struct LogicApi
    {
        unsigned abi_version;
        void (*set_layout)(unsigned back_width, unsigned back_height);
        bool (*active)();
        void (*new_frame)();
        bool (*last_frame_had_scene)();
        void (*note_constants)(UINT start, const float* data, UINT count);
        void (*note_ps_constants)(UINT start, const float* data, UINT count);
        void (*note_render_target)(IDirect3DSurface9* rt0);
        void (*note_viewport)(const D3DVIEWPORT9* vp);
        void (*note_vertex_shader)(IDirect3DVertexShader9* shader);
        void (*note_alpha_blend)(bool enabled);
        void (*note_vertex_shader_created)(IDirect3DVertexShader9* shader, const DWORD* bytecode,
                                           bool patched);
        bool (*patch_vertex_shader)(const DWORD* in, DWORD* out, size_t capacity, size_t* out_len);
        bool (*patch_pixel_shader)(const DWORD* in, DWORD* out, size_t capacity, size_t* out_len);
        void (*before_single_draw)(IDirect3DDevice9* device);
        void (*note_texture)(DWORD stage, IDirect3DBaseTexture9* texture);
        bool (*wants_vertex_box)();
        void (*note_vertex_box)(const float* box);
        bool (*main_pass)(int eye, Mat4* world_to_eye_view, Mat4* eye_proj,
                          D3DVIEWPORT9* eye_vp, IDirect3DSurface9** target);
        const char* (*last_draw_kind)();
        int (*plan_draw)();
        void (*begin_eye)(IDirect3DDevice9* device, int eye);
        void (*end)(IDirect3DDevice9* device);
        // God mode (god_frame.h): after the game's chase update, and the
        // seat against the game's camera at draw time (god_seat_to_game_view).
        void (*god_frame)(const GodFrameIn* in, GodSeatState* seat, GodFrameOut* out);
        void (*god_seat_to_view)(const float seat_pos[3], const float seat_rot[3][3],
                                 const float cam_pos[3], const float cam_rot[3][3], Mat4* out);
    };
}

// Exported by psyvr_logic.dll (src/logic/psyvr_logic.def). Create returns
// false, after logging why through host->log, if the host does not match.
extern "C"
{
    typedef bool (*PsyvrLogicCreate)(const psyvr::LogicHost* host, psyvr::LogicApi* api);
    typedef void (*PsyvrLogicDestroy)();
    bool psyvr_logic_create(const psyvr::LogicHost* host, psyvr::LogicApi* api);
    void psyvr_logic_destroy();
}
