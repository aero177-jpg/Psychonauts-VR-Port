#include "logic_host.h"

#include "../common/config.h"
#include "../common/log.h"
#include "../common/logic_abi.h"
#include "game_psy.h"
#include "../vr/hud.h"
#include "../vr/stereo.h"
#include "../vr/vr_session.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace psyvr
{
    namespace
    {
        // The loaded logic. Forwarders hold the lock shared; a swap holds it
        // exclusive.
        SRWLOCK g_lock = SRWLOCK_INIT;
        LogicApi g_api{};
        bool g_loaded = false;
        HMODULE g_module = nullptr;
        PsyvrLogicDestroy g_destroy = nullptr;
        std::wstring g_temp_path;       // the loaded copy in %TEMP%, or empty (loaded in place)

        struct Shared
        {
            Shared() { AcquireSRWLockShared(&g_lock); }
            ~Shared() { ReleaseSRWLockShared(&g_lock); }
        };

        // What the logic is told, mirrored here to rebuild a fresh copy. Raw
        // pointers: nothing is AddRef'd (a held target would block Reset).
        struct ShaderRec
        {
            std::vector<DWORD> code;
            bool patched = false;
        };
        SRWLOCK g_mirror_lock = SRWLOCK_INIT;   // the shader table (any thread creates)
        std::unordered_map<IDirect3DVertexShader9*, ShaderRec> g_shaders;
        unsigned g_back_w = 0, g_back_h = 0;
        IDirect3DVertexShader9* g_vs = nullptr;
        IDirect3DSurface9* g_rt0 = nullptr;
        D3DVIEWPORT9 g_vp{ 0, 0, 0, 0, 0.0f, 1.0f };
        bool g_blend = false;
        IDirect3DBaseTexture9* g_tex[16] = {};
        float g_vs_c[256][4] = {};
        float g_ps_c[224][4] = {};

        // Length of a vertex shader in tokens, through the end token (0x0000FFFF);
        // 0 if malformed or longer than `limit`.
        size_t shader_tokens(const DWORD* code, size_t limit)
        {
            if (!code || (code[0] & 0xFFFF0000u) != 0xFFFE0000u)
                return 0;
            size_t i = 1;
            while (i < limit)
            {
                const DWORD t = code[i];
                if (t == 0x0000FFFFu)
                    return i + 1;
                if ((t & 0xFFFFu) == 0xFFFEu)
                    i += 1 + ((t >> 16) & 0x7FFFu);     // comment
                else
                    i += 1 + ((t >> 24) & 0xFu);        // instruction: operand count
            }
            return 0;
        }

        // Host services for the logic.
        void host_log(const char* text)
        {
            log("%s", text);
        }
        const Config* host_config()
        {
            return &config();
        }
        bool host_hud_enabled()
        {
            return hud_enabled();
        }
        bool host_eye_to_game_view(int eye, Mat4* out)
        {
            return vr_eye_to_game_view(eye, out);
        }
        void host_eye_projection(int eye, float near_z, float far_z, Mat4* out)
        {
            *out = vr_eye_projection(eye, near_z, far_z);
        }

        // The logic keeps the pointer, so this outlives every module.
        const LogicHost* host()
        {
            static LogicHost h{};
            h.abi_version = kLogicAbiVersion;
            h.config_size = (unsigned)sizeof(Config);
            h.mat4_size = (unsigned)sizeof(Mat4);
            h.god_size = (unsigned)(sizeof(GodSeatState) + sizeof(GodFrameIn) + sizeof(GodFrameOut));
            h.log = host_log;
            h.config = host_config;
            h.hud_enabled = host_hud_enabled;
            h.vr_eye_to_game_view = host_eye_to_game_view;
            h.vr_eye_projection = host_eye_projection;
            h.vr_view_forward = vr_view_forward;
            h.lock_target = game_lock_target;
            h.theatre_eyes = game_theatre_eyes;
            return &h;
        }

        struct Candidate
        {
            HMODULE module = nullptr;
            PsyvrLogicDestroy destroy = nullptr;
            LogicApi api{};
        };

        // LoadLibrary `path` and create the logic; null = ok, else the reason.
        const char* open_logic(const std::wstring& path, Candidate* out)
        {
            HMODULE m = LoadLibraryW(path.c_str());
            if (!m)
                return "LoadLibrary failed";
            const PsyvrLogicCreate create =
                reinterpret_cast<PsyvrLogicCreate>(GetProcAddress(m, "psyvr_logic_create"));
            const PsyvrLogicDestroy destroy =
                reinterpret_cast<PsyvrLogicDestroy>(GetProcAddress(m, "psyvr_logic_destroy"));
            if (!create || !destroy)
            {
                FreeLibrary(m);
                return "exports missing";
            }
            LogicApi api{};
            if (!create(host(), &api))
            {
                FreeLibrary(m);
                return "create refused (see above)";
            }
            if (api.abi_version != kLogicAbiVersion)
            {
                destroy();
                FreeLibrary(m);
                return "api version mismatch";
            }
            out->module = m;
            out->destroy = destroy;
            out->api = api;
            return nullptr;
        }

        std::wstring logic_path()
        {
            return std::wstring(exe_dir()) + L"psyvr_logic.dll";
        }

        unsigned g_copies = 0;
        std::wstring temp_copy_name()
        {
            wchar_t dir[MAX_PATH]{};
            GetTempPathW(MAX_PATH, dir);
            wchar_t name[64]{};
            swprintf_s(name, L"psyvr_logic_%lu_%u.dll", GetCurrentProcessId(), ++g_copies);
            return std::wstring(dir) + name;
        }

        struct Stamp
        {
            FILETIME written{};
            unsigned long long size = 0;
            bool valid = false;
            bool operator==(const Stamp& o) const
            {
                return valid == o.valid && written.dwLowDateTime == o.written.dwLowDateTime &&
                       written.dwHighDateTime == o.written.dwHighDateTime && size == o.size;
            }
        };
        Stamp read_stamp(const std::wstring& path)
        {
            Stamp s;
            WIN32_FILE_ATTRIBUTE_DATA d{};
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d))
            {
                s.written = d.ftLastWriteTime;
                s.size = ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
                s.valid = true;
            }
            return s;
        }
        Stamp g_loaded_stamp, g_pending_stamp;  // what is loaded; a change seen once
        unsigned g_polls = 0;

        // The mirrored object pointers may have been released since the game
        // last set them; a dead one must not take the replay down.
        void replay_surface(const LogicApi& api, IDirect3DSurface9* rt)
        {
            __try { api.note_render_target(rt); }
            __except (EXCEPTION_EXECUTE_HANDLER) { api.note_render_target(nullptr); }
        }
        void replay_texture(const LogicApi& api, DWORD stage, IDirect3DBaseTexture9* t)
        {
            __try { api.note_texture(stage, t); }
            __except (EXCEPTION_EXECUTE_HANDLER) { api.note_texture(stage, nullptr); }
        }

        // Gives a fresh logic the state the old one had; returns the shader count.
        size_t replay_mirror(const LogicApi& api)
        {
            api.set_layout(g_back_w, g_back_h);
            size_t shaders = 0;
            AcquireSRWLockShared(&g_mirror_lock);
            for (const auto& kv : g_shaders)
            {
                if (kv.second.code.empty())
                    continue;       // bytecode not readable at creation
                api.note_vertex_shader_created(kv.first, kv.second.code.data(), kv.second.patched);
                ++shaders;
            }
            ReleaseSRWLockShared(&g_mirror_lock);
            api.note_vertex_shader(g_vs);
            replay_surface(api, g_rt0);
            api.note_viewport(&g_vp);
            api.note_alpha_blend(g_blend);
            for (DWORD s = 0; s < 16; ++s)
                replay_texture(api, s, g_tex[s]);
            api.note_constants(0, &g_vs_c[0][0], 256);
            api.note_ps_constants(0, &g_ps_c[0][0], 224);
            api.new_frame();
            return shaders;
        }

        void delete_temp(const std::wstring& path)
        {
            if (!path.empty())
                DeleteFileW(path.c_str());
        }

        // Removes copies left by earlier runs (a crash skips the cleanup at
        // exit). One still loaded by a running game fails to delete: harmless.
        void clean_stale_copies()
        {
            wchar_t dir[MAX_PATH]{};
            GetTempPathW(MAX_PATH, dir);
            wchar_t pattern[MAX_PATH + 64]{};
            swprintf_s(pattern, L"%spsyvr_logic_*.dll", dir);
            WIN32_FIND_DATAW f{};
            HANDLE h = FindFirstFileW(pattern, &f);
            if (h == INVALID_HANDLE_VALUE)
                return;
            do
            {
                DeleteFileW((std::wstring(dir) + f.cFileName).c_str());
            } while (FindNextFileW(h, &f));
            FindClose(h);
        }
    }

    void logic_load_initial()
    {
        if (g_loaded)
            return;
        const std::wstring source = logic_path();
        std::wstring path = source;
        std::wstring temp;
        if (config().debug_hot_reload)
        {
            // A copy, so the build can overwrite the original while it runs.
            clean_stale_copies();
            temp = temp_copy_name();
            if (!CopyFileW(source.c_str(), temp.c_str(), FALSE))
            {
                log("logic: load failed: could not copy %S", source.c_str());
                return;
            }
            path = temp;
        }
        Candidate c;
        const char* why = open_logic(path, &c);
        if (why)
        {
            log("logic: load failed: %s (%S)", why, path.c_str());
            delete_temp(temp);
            return;
        }
        AcquireSRWLockExclusive(&g_lock);
        g_api = c.api;
        g_module = c.module;
        g_destroy = c.destroy;
        g_loaded = true;
        ReleaseSRWLockExclusive(&g_lock);
        g_temp_path = temp;
        g_loaded_stamp = read_stamp(source);
        log("logic: loaded %S (abi %u)", path.c_str(), kLogicAbiVersion);
    }

    void logic_poll_frame_boundary()
    {
        if (!config().debug_hot_reload || !g_loaded || (++g_polls % 30) != 0)
            return;
        const std::wstring source = logic_path();
        const Stamp now = read_stamp(source);
        if (!now.valid || now == g_loaded_stamp)
        {
            g_pending_stamp = Stamp{};
            return;
        }
        if (!(now == g_pending_stamp))
        {
            g_pending_stamp = now;      // changed once; a build may still be writing it
            return;
        }

        const DWORD t0 = GetTickCount();
        const std::wstring temp = temp_copy_name();
        if (!CopyFileW(source.c_str(), temp.c_str(), FALSE))
        {
            log("logic: reload failed: could not copy %S (error %lu)", source.c_str(),
                GetLastError());
            return;             // still held by the linker, perhaps: tried again next poll
        }
        g_loaded_stamp = now;   // a bad file is not retried until it changes again
        g_pending_stamp = Stamp{};
        Candidate c;
        const char* why = open_logic(temp, &c);
        if (why)
        {
            log("logic: reload failed: %s", why);
            delete_temp(temp);
            return;
        }

        const HMODULE old_module = g_module;
        const std::wstring old_temp = g_temp_path;
        AcquireSRWLockExclusive(&g_lock);
        if (g_destroy)
            g_destroy();
        g_api = c.api;
        g_module = c.module;
        g_destroy = c.destroy;
        const size_t shaders = replay_mirror(g_api);
        ReleaseSRWLockExclusive(&g_lock);
        if (old_module)
            FreeLibrary(old_module);
        delete_temp(old_temp);
        g_temp_path = temp;
        log("logic: reloaded in %lu ms (%u shaders replayed)", GetTickCount() - t0,
            (unsigned)shaders);
    }

    void logic_unload()
    {
        AcquireSRWLockExclusive(&g_lock);
        if (g_destroy)
            g_destroy();
        g_destroy = nullptr;
        g_loaded = false;
        const HMODULE m = g_module;
        g_module = nullptr;
        ReleaseSRWLockExclusive(&g_lock);
        if (m)
            FreeLibrary(m);
        delete_temp(g_temp_path);
        g_temp_path.clear();
    }

    // stereo.h, forwarded.

    void stereo_set_layout(unsigned back_width, unsigned back_height)
    {
        g_back_w = back_width;
        g_back_h = back_height;
        Shared lock;
        if (g_loaded)
            g_api.set_layout(back_width, back_height);
    }

    bool stereo_active()
    {
        Shared lock;
        return g_loaded && g_api.active();
    }

    void stereo_new_frame()
    {
        Shared lock;
        if (g_loaded)
            g_api.new_frame();
    }

    bool stereo_last_frame_had_scene()
    {
        Shared lock;
        return g_loaded ? g_api.last_frame_had_scene() : true;
    }

    void stereo_note_constants(UINT start, const float* data, UINT count)
    {
        if (data && start < 256)
            memcpy(g_vs_c[start], data,
                   (start + count > 256 ? 256 - start : count) * 4 * sizeof(float));
        Shared lock;
        if (g_loaded)
            g_api.note_constants(start, data, count);
    }

    void stereo_note_ps_constants(UINT start, const float* data, UINT count)
    {
        if (data && start < 224)
            memcpy(g_ps_c[start], data,
                   (start + count > 224 ? 224 - start : count) * 4 * sizeof(float));
        Shared lock;
        if (g_loaded)
            g_api.note_ps_constants(start, data, count);
    }

    void stereo_note_render_target(IDirect3DSurface9* rt0)
    {
        g_rt0 = rt0;
        Shared lock;
        if (g_loaded)
            g_api.note_render_target(rt0);
    }

    void stereo_note_viewport(const D3DVIEWPORT9& vp)
    {
        g_vp = vp;
        Shared lock;
        if (g_loaded)
            g_api.note_viewport(&vp);
    }

    void stereo_note_vertex_shader(IDirect3DVertexShader9* shader)
    {
        g_vs = shader;
        Shared lock;
        if (g_loaded)
            g_api.note_vertex_shader(shader);
    }

    void stereo_note_alpha_blend(bool enabled)
    {
        g_blend = enabled;
        Shared lock;
        if (g_loaded)
            g_api.note_alpha_blend(enabled);
    }

    void stereo_note_vertex_shader_created(IDirect3DVertexShader9* shader, const DWORD* bytecode,
                                           bool patched)
    {
        if (shader)
        {
            const size_t n = shader_tokens(bytecode, 16384);
            ShaderRec rec;
            if (n)
                rec.code.assign(bytecode, bytecode + n);
            rec.patched = patched;
            AcquireSRWLockExclusive(&g_mirror_lock);
            g_shaders[shader] = std::move(rec);
            ReleaseSRWLockExclusive(&g_mirror_lock);
        }
        Shared lock;
        if (g_loaded)
            g_api.note_vertex_shader_created(shader, bytecode, patched);
    }

    bool stereo_patch_vertex_shader(const DWORD* in, DWORD* out, size_t capacity, size_t* out_len)
    {
        Shared lock;
        return g_loaded && g_api.patch_vertex_shader(in, out, capacity, out_len);
    }

    bool stereo_patch_pixel_shader(const DWORD* in, DWORD* out, size_t capacity, size_t* out_len)
    {
        Shared lock;
        return g_loaded && g_api.patch_pixel_shader(in, out, capacity, out_len);
    }

    void stereo_before_single_draw(IDirect3DDevice9* device)
    {
        Shared lock;
        if (g_loaded)
            g_api.before_single_draw(device);
    }

    void stereo_note_texture(DWORD stage, IDirect3DBaseTexture9* texture)
    {
        if (stage < 16)
            g_tex[stage] = texture;
        Shared lock;
        if (g_loaded)
            g_api.note_texture(stage, texture);
    }

    bool stereo_wants_vertex_box()
    {
        Shared lock;
        return g_loaded && g_api.wants_vertex_box();
    }

    void stereo_note_vertex_box(const float* box)
    {
        Shared lock;
        if (g_loaded)
            g_api.note_vertex_box(box);
    }

    bool stereo_main_pass(int eye, Mat4* world_to_eye_view, Mat4* eye_proj,
                          D3DVIEWPORT9* eye_vp, IDirect3DSurface9** target)
    {
        Shared lock;
        return g_loaded && g_api.main_pass(eye, world_to_eye_view, eye_proj, eye_vp, target);
    }

    const char* stereo_last_draw_kind()
    {
        Shared lock;
        return g_loaded ? g_api.last_draw_kind() : "no logic";
    }

    int stereo_plan_draw()
    {
        Shared lock;
        return g_loaded ? g_api.plan_draw() : 0;
    }

    void stereo_begin_eye(IDirect3DDevice9* device, int eye)
    {
        Shared lock;
        if (g_loaded)
            g_api.begin_eye(device, eye);
    }

    void stereo_end(IDirect3DDevice9* device)
    {
        Shared lock;
        if (g_loaded)
            g_api.end(device);
    }

    // god_frame.h, forwarded (logic_host.h). No logic: no seat.

    bool logic_god_frame(const GodFrameIn& in, GodSeatState& seat, GodFrameOut& out)
    {
        Shared lock;
        if (!g_loaded)
            return false;
        g_api.god_frame(&in, &seat, &out);
        return true;
    }

    bool logic_god_seat_to_view(const float seat_pos[3], const float seat_rot[3][3], const float cam_pos[3],
                                const float cam_rot[3][3], Mat4* out)
    {
        Shared lock;
        if (!g_loaded)
            return false;
        g_api.god_seat_to_view(seat_pos, seat_rot, cam_pos, cam_rot, out);
        return true;
    }
}
