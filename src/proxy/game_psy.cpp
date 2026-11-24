#include "game_psy.h"
#include "control.h"
#include "../common/config.h"
#include "../common/log.h"
#include "../vr/vr_session.h"
#include "../vr/vr_pad.h"
#include "../vr/xr_runtime.h"
#include "../vr/vr_submit.h"
#include "overlay.h"
#include "../vr/hud.h"
#include "../vr/god_frame.h"
#include "../vr/psy_theatre.h"
#include "../vr/stereo.h"
#include "logic_host.h"
#include "hook.h"
#include <xinput.h>
#include <cmath>
#include <cwctype>
#include <cstdlib>

#include <windows.h>
#include <cstring>
#include <cstdio>
#include <share.h>
#include <string>
#include <vector>

// Everything that touches Psychonauts.exe's memory. The addresses are from the
// Steam build (Psychonauts.exe, PE TimeDateStamp 0x568C5772); every hook and
// patch checks the original bytes before it writes, so another build is left
// alone rather than corrupted.

namespace psyvr
{
    bool game_is_supported_build()
    {
        static int known = -1;
        if (known < 0)
        {
            known = 0;
            __try
            {
                const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
                known = nt->FileHeader.TimeDateStamp == 0x568C5772u ? 1 : 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        return known == 1;
    }

    namespace
    {
        void god_install();     // god mode's chase camera hook, below
        void detail_install();  // screen-size detail culling correction, below

        // [camera] first_person_stays. The chase update (FUN_004DB540) clears
        // the first-person request (camera + 0x964 bit 1) whenever anything
        // happens on the pad (its local at esp + 0xD73: the sticks, the
        // action buttons, or a D-pad change at input + 0x6C), so the look is
        // left as soon as Raz moves. The jump over that clear at 0x4DBDFB
        // (je, 74) made unconditional (jmp, EB) keeps it until R3 again (or
        // the game's own exits: cutscenes, FUN_004CD9D0). It also lets R3 in
        // at all here: input + 0x6C is the D-pad's "changed to" (-1 = no
        // change), and only the keyboard path (FUN_004055D0) puts -1 back, so
        // with a pad it stays 0 and counts as activity on every frame
        // (docs/findings.md, 2026-11-16).
        bool g_fp_patch_ok = false;     // the bytes were the expected build's
        int g_fp_patch_state = -1;      // what is written now (1 = jmp)

        void first_person_patch_check()
        {
            static const unsigned char expect[] = { 0x0F, 0xB6, 0x94, 0x24, 0x73, 0x0D, 0x00, 0x00, 0x85, 0xD2, 0x74, 0x1D };
            const unsigned char* p = reinterpret_cast<const unsigned char*>(0x4DBDF1);
            MEMORY_BASIC_INFORMATION mbi{};
            g_fp_patch_ok = VirtualQuery(p, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
                            memcmp(p, expect, sizeof(expect)) == 0;
            if (!g_fp_patch_ok)
                log("patch: first person stays skipped (code at 0x004DBDF1 is not the expected build)");
        }

        void first_person_patch_frame()
        {
            const int want = config().first_person_stays ? 1 : 0;
            if (!g_fp_patch_ok || want == g_fp_patch_state)
                return;
            unsigned char* p = reinterpret_cast<unsigned char*>(0x4DBDFB);
            DWORD old = 0;
            if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old))
                return;
            *p = want ? 0xEB : 0x74;
            VirtualProtect(p, 1, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p, 1);
            g_fp_patch_state = want;
            log("patch: first person stays %s (0x004DBDFB = %02X)", want ? "on" : "off", *p);
        }

        // Overwrites code after checking it is exactly what we expect.
        bool patch_code(uintptr_t at, const unsigned char* expect, size_t expect_len,
                        const unsigned char* write, size_t write_len, const char* what)
        {
            unsigned char* p = reinterpret_cast<unsigned char*>(at);
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
                memcmp(p, expect, expect_len) != 0)
            {
                log("patch: %s skipped (code at 0x%08X is not the expected build)", what, (unsigned)at);
                return false;
            }
            DWORD old = 0;
            if (!VirtualProtect(p, write_len, PAGE_EXECUTE_READWRITE, &old))
                return false;
            memcpy(p, write, write_len);
            VirtualProtect(p, write_len, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p, write_len);
            log("patch: %s applied at 0x%08X", what, (unsigned)at);
            return true;
        }
    }

    // The game has already read DisplaySettings.ini when Direct3DCreate9 runs:
    // FUN_0050B150 (GameApp init) reads it into the renderer (game + 0x194;
    // ScreenWidth +0x2C4, ScreenHeight +0x2C8, FullScreen byte +0x2D0), then
    // FUN_0067DC20 (renderer init) stores the renderer in 0x7914A8, calls
    // Direct3DCreate9 and only then creates the device and its targets from
    // those fields. So the size is corrected in memory here, for this launch;
    // the file write keeps the next one (and the game's own settings save,
    // FUN_00667900, which writes these fields back) in step. Written only if
    // the fields hold exactly what the file said (or the game's 800x600
    // defaults): anything else is another build.
    static bool set_renderer_size(unsigned old_w, unsigned old_h, unsigned width, unsigned height)
    {
        __try
        {
            const uintptr_t r = *reinterpret_cast<const uintptr_t*>(0x7914A8);
            if (!r)
                return false;
            unsigned* w = reinterpret_cast<unsigned*>(r + 0x2C4);
            unsigned* h = reinterpret_cast<unsigned*>(r + 0x2C8);
            if (*w != old_w || *h != old_h)
                return false;
            *w = width;
            *h = height;
            *reinterpret_cast<unsigned char*>(r + 0x2D0) = 0;     // windowed
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void game_sync_display_settings(unsigned width, unsigned height)
    {
        if (!width || !height)
            return;
        wchar_t path[MAX_PATH]{}, backup[MAX_PATH]{};
        swprintf_s(path, L"%sDisplaySettings.ini", exe_dir());
        swprintf_s(backup, L"%sDisplaySettings.ini.psyvr-backup", exe_dir());

        // Read the file (ANSI, small) into lines.
        std::string text;
        if (FILE* f = _wfsopen(path, L"rbN", _SH_DENYNO))
        {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                text.append(buf, n);
            fclose(f);
        }
        const bool existed = !text.empty();
        std::vector<std::string> lines;
        for (size_t at = 0; at <= text.size();)
        {
            size_t end = text.find('\n', at);
            if (end == std::string::npos)
                end = text.size();
            std::string line = text.substr(at, end - at);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!(end == text.size() && line.empty()))
                lines.push_back(line);
            at = end + 1;
        }
        char w[32], h[32];
        sprintf_s(w, "ScreenWidth=%u", width);
        sprintf_s(h, "ScreenHeight=%u", height);
        const char* want[3] = { w, h, "FullScreen=false" };
        const char* keys[3] = { "ScreenWidth=", "ScreenHeight=", "FullScreen=" };
        bool found[3] = {}, changed = false;
        int global_at = -1;
        unsigned old_size[2] = { 800, 600 };    // the game's defaults (FUN_005196F0)
        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (_stricmp(lines[i].c_str(), "[Global]") == 0)
                global_at = (int)i;
            for (int k = 0; k < 3; ++k)
                if (_strnicmp(lines[i].c_str(), keys[k], strlen(keys[k])) == 0)
                {
                    found[k] = true;
                    if (k < 2)
                        old_size[k] = (unsigned)strtoul(lines[i].c_str() + strlen(keys[k]), nullptr, 10);
                    if (lines[i] != want[k])
                    {
                        lines[i] = want[k];
                        changed = true;
                    }
                }
        }
        if (old_size[0] != width || old_size[1] != height)
        {
            if (set_renderer_size(old_size[0], old_size[1], width, height))
                log("display: the game read %ux%u from DisplaySettings.ini; its renderer set to %ux%u before the device",
                    old_size[0], old_size[1], width, height);
            else
                log("display: the game's renderer size not set (not %ux%u at 0x7914A8 + 0x2C4): %ux%u from the next launch",
                    old_size[0], old_size[1], width, height);
        }
        if (global_at < 0)
        {
            lines.insert(lines.begin(), "[Global]");
            global_at = 0;
            changed = true;
        }
        for (int k = 2; k >= 0; --k)
            if (!found[k])
            {
                lines.insert(lines.begin() + global_at + 1, want[k]);
                changed = true;
            }
        if (!changed)
        {
            log("display: DisplaySettings.ini already %ux%u windowed", width, height);
            return;
        }
        if (existed && GetFileAttributesW(backup) == INVALID_FILE_ATTRIBUTES)
            CopyFileW(path, backup, TRUE);
        if (!existed)
        {
            // uninstall.bat removes a DisplaySettings.ini only with this mark
            // (the mod made it); one it neither made nor backed up is the player's.
            wchar_t mark[MAX_PATH]{};
            swprintf_s(mark, L"%sDisplaySettings.ini.psyvr-created", exe_dir());
            if (FILE* m = _wfsopen(mark, L"wbN", _SH_DENYWR))
                fclose(m);
        }
        if (FILE* f = _wfsopen(path, L"wbN", _SH_DENYWR))
        {
            for (const std::string& line : lines)
            {
                fputs(line.c_str(), f);
                fputs("\r\n", f);
            }
            fclose(f);
            log("display: DisplaySettings.ini set to %ux%u windowed (the game sizes its render targets from it)",
                width, height);
        }
        else
            log("display: could not write DisplaySettings.ini");
    }

    void game_write_lua_mod(bool vr)
    {
        wchar_t dir[MAX_PATH]{}, path[MAX_PATH]{};
        swprintf_s(dir, L"%sModResource\\PsyVR", exe_dir());
        if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES)
        {
            log("camera: no ModResource\\PsyVR (Astralathe mod) -- [camera] settings not applied");
            return;
        }
        swprintf_s(path, L"%s\\Scripts", dir);
        CreateDirectoryW(path, nullptr);
        swprintf_s(path, L"%s\\Scripts\\Dart_hooks.lua", dir);
        const Config& c = config();
        char text[2048];
        sprintf_s(text,
            "-- Written by the Psychonauts VR mod (d3d9.dll) at startup from psyvr.ini\r\n"
            "-- [camera]; edits here are overwritten.\r\n"
            "PSYVR_ENABLED = %d\r\n"

            "PSYVR_CAMERA_SHAKE = %d\r\n"
            "PSYVR_CULL_FOV = %d\r\n"
            "\r\n"
            "function Dart_hooks(Ob)\r\n"
            "\tlocal onPostBeginLevel_original = Ob.onPostBeginLevel\r\n"
            "\tfunction Ob:onPostBeginLevel()\r\n"
            "\t\t%%onPostBeginLevel_original(self)\r\n"
            "\t\tif PSYVR_ENABLED == 1 then\r\n"
            "\t\t\tif PSYVR_CAMERA_SHAKE == 0 then\r\n"
            "\t\t\t\tSetCameraShakeEnable(0)\r\n"
            "\t\t\tend\r\n"
            // The chase distance is scaled natively and live (game_zoom_frame).
            "\t\t\tif PSYVR_CULL_FOV > 0 then\r\n"
            "\t\t\t\tSetCameraFieldOfView(PSYVR_CULL_FOV)\r\n"
            "\t\t\tend\r\n"
            "\t\t\tGamePrint(\"psyvr: camera radius \" .. tostring(GetChaseCameraRadius()) .. \", shake \" .. PSYVR_CAMERA_SHAKE)\r\n"
            "\t\tend\r\n"
            "\tend\r\n"
            "end\r\n",
            vr ? 1 : 0, c.camera_shake ? 1 : 0,
            // The Lua field of view is twice the vertical angle in degrees
            // (104 = the 52 deg measured from the projection).
            c.cull_fov_degrees > 0.0f ? (int)(c.cull_fov_degrees * 2.0f + 0.5f) : 0);
        if (FILE* f = _wfsopen(path, L"wbN", _SH_DENYWR))
        {
            fputs(text, f);
            fclose(f);
            log("camera: PsyVR mod hook written (shake %s, cull fov %.0f deg%s)",
                c.camera_shake ? "on" : "off", c.cull_fov_degrees, vr ? "" : "; VR off, so not applied");
        }
    }

    void game_apply_patches()
    {
        // 0x465440 "is the game window in front": GetForegroundWindow() ==
        // the game window. The pause-on-focus-loss and input checks call it;
        // with the VR runtime's window (or the headset) in front the game
        // paused itself. Made to answer "yes" (ThirteenAG's widescreen fix
        // finds it by pattern 55 8B EC FF 15 ? ? ? ? 33 C9; the Steam build has
        // no ASLR, so the address is fixed and the bytes are checked).
        if (!config().pause_when_unfocused)
        {
            static const unsigned char expect[] = { 0x55, 0x8B, 0xEC, 0xFF, 0x15, 0x04, 0x04, 0x70, 0x00, 0x33, 0xC9 };
            static const unsigned char write[] = { 0xB0, 0x01, 0xC3 };     // mov al, 1; ret
            patch_code(0x465440, expect, sizeof(expect), write, sizeof(write), "never pause when unfocused");
        }
        god_install();
        detail_install();
        first_person_patch_check();
    }

    bool game_camera_read(float pos[3], float quat[4])
    {
        __try
        {
            const uintptr_t game = *reinterpret_cast<const uintptr_t*>(0x78BC20);
            if (!game)
                return false;
            const uintptr_t cam = *reinterpret_cast<const uintptr_t*>(game + 0x8C98);
            if (!cam)
                return false;
            memcpy(pos, reinterpret_cast<const void*>(cam + 0x08), 12);
            memcpy(quat, reinterpret_cast<const void*>(cam + 0x20), 16);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    const char* game_level_name()
    {
        static char name[32];
        name[0] = 0;
        __try
        {
            const uintptr_t game = *reinterpret_cast<const uintptr_t*>(0x78BC20);
            if (!game)
                return name;
            const char* s = reinterpret_cast<const char*>(game + 0x989A);
            size_t i = 0;
            for (; i < sizeof(name) - 1 && s[i] >= 0x20 && s[i] < 0x7F; ++i)
                name[i] = s[i];
            name[s[i] == 0 ? i : 0] = 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            name[0] = 0;
        }
        return name;
    }

    bool game_controls_paused()
    {
        __try
        {
            return *reinterpret_cast<const volatile int*>(0x78BB14) > 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    namespace
    {
        // The level now loaded is named in `list` (config: names or the start
        // of names, separated by spaces or commas; any case).
        bool level_in_list(const wchar_t* list, const char* level)
        {
            if (!level[0])
                return false;
            for (const wchar_t* p = list; *p;)
            {
                while (*p == L' ' || *p == L',' || *p == L'	')
                    ++p;
                size_t n = 0;
                while (p[n] && p[n] != L' ' && p[n] != L',' && p[n] != L'	')
                    ++n;
                if (n)
                {
                    size_t i = 0;
                    while (i < n && level[i] && towupper(p[i]) == towupper((wchar_t)(unsigned char)level[i]))
                        ++i;
                    if (i == n)
                        return true;
                }
                p += n;
            }
            return false;
        }

        bool in_start_menu()
        {
            return _stricmp(game_level_name(), "STMU") == 0;
        }

        // Theatre (psy_theatre.h). Set at Present for the next frame, read
        // by the chase update and the draws.
        volatile LONG g_theatre_showing = 0;
        volatile LONG g_theatre_opens = 0;
        // The game's own field of view (Lua units: 2 x the vertical angle),
        // as hold_settings_frame last saw it.
        float g_theatre_game_fov_lua = 104.0f;
        // The window style culls this much wider (in tan) than the game's
        // shot: leaning and turning look past its edges.
        const float kTheatreWindowCull = 1.6f;

        // The type of the camera the game draws with, *(int*)camera (2 =
        // chase), or -1 when there is none (no game object, loading).
        int active_camera_type()
        {
            __try
            {
                const uintptr_t game = *reinterpret_cast<const uintptr_t*>(0x78BC20);
                const uintptr_t cam = game ? *reinterpret_cast<const uintptr_t*>(game + 0x8C98) : 0;
                return cam ? *reinterpret_cast<const int*>(cam) : -1;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return -1;
            }
        }

        // A change has to last this many frames, so a camera that is active
        // for a frame or two (a level start, a snap) does not flash the
        // window open and shut.
        const unsigned kTheatreSettleFrames = 3;
        // A loading screen (docs/findings.md 2026-11-24): the game's loader
        // thread presents it, not the main thread. The theatre opens on its
        // first frame (no settling: the loading screen otherwise filled the
        // view for the few frames before the script camera opened it) and
        // stays open while it lasts, whatever the camera (the chase camera
        // came back up to 0.6 s before the loader thread stopped), and this
        // many of the main thread's frames after.
        const unsigned kTheatreAfterLoadingFrames = 5;

        void theatre_frame()
        {
            const Config& c = config();
            const int type = active_camera_type();
            {
                static int last = -2;
                static unsigned told = 0;
                if (type != last && told < 200)
                {
                    ++told;
                    log("theatre: the game's camera is now type %d%s (controls %s, level %s)", type,
                        type == 0 ? " (fly)" : type == 1 ? " (script)" : type == 2 ? " (chase)" : type == 3 ? " (rail)"
                        : type == 5 ? " (sphere)" : "", game_controls_paused() ? "paused" : "free",
                        game_level_name()[0] ? game_level_name() : "(none)");
                }
                last = type;
            }
            // The game's camera types (Lua constants, registered at 0x6FE9xx):
            // kCAMERA_FLY 0, SCRIPT 1, CHASE 2, RAIL 3, SPHERE 5. Rail and
            // sphere cameras play in gameplay areas; a cutscene is SCRIPT.
            const bool cutscene = type == 1;
            const bool loader = vr_submit_ready() && GetCurrentThreadId() != vr_submit_thread();
            static unsigned loading_hold = 0;
            if (loader)
                loading_hold = kTheatreAfterLoadingFrames;
            else if (loading_hold)
                --loading_hold;
            const bool loading = loading_hold != 0 && c.theatre_mode != 0;
            const bool want = vr_ready() && stereo_active() &&
                              (loading || theatre_wanted(c.theatre_mode, cutscene, in_start_menu()));
            // Turning it on or off in the settings is immediate.
            static int last_mode = -1;
            const bool mode_changed = last_mode != c.theatre_mode;
            last_mode = c.theatre_mode;
            static unsigned pending = 0;
            const bool now = g_theatre_showing != 0;
            if (want == now)
            {
                pending = 0;
                return;
            }
            if (!mode_changed && !(want && loader) && ++pending < kTheatreSettleFrames)
                return;
            pending = 0;
            if (want)
                InterlockedIncrement(&g_theatre_opens);
            InterlockedExchange(&g_theatre_showing, want ? 1 : 0);
            log("theatre: %s (mode %d, camera type %d, level %s%s)", want ? "open" : "closed", c.theatre_mode, type,
                game_level_name()[0] ? game_level_name() : "(none)", loading ? ", loading screen" : "");
        }
    }

    bool game_theatre_showing()
    {
        return g_theatre_showing != 0;
    }

    unsigned game_theatre_open_count()
    {
        return (unsigned)g_theatre_opens;
    }

    bool game_theatre_eyes(TheatreEyes* out)
    {
        if (!out || !g_theatre_showing)
            return false;
        float l[3], r[3];
        if (!vr_eye_offset_metres(0, l) || !vr_eye_offset_metres(1, r))
            return false;
        const Config& c = config();
        const TheatreWindowSize w = theatre_window_size(c.theatre_distance, c.theatre_width_degrees, c.theatre_aspect);
        if (c.theatre_style == kTheatreWindow)
        {
            // Each eye in the anchor's frame (the window moved `distance`
            // back along its normal), translation in game units.
            Mat4 window, head, room_to_anchor;
            if (!hud_theatre_window(&window) || !vr_head_in_room_metres(&head))
                return false;
            Mat4 anchor = window;
            for (int k = 0; k < 3; ++k)
                anchor.m[3][k] -= w.distance * window.m[2][k];
            if (!invert(anchor, &room_to_anchor))
                return false;
            TheatreEyes t{};
            t.style = kTheatreWindow;
            t.aspect = c.theatre_aspect;
            const float* off[2] = { l, r };
            for (int e = 0; e < 2; ++e)
            {
                Mat4 m = translation(off[e][0], off[e][1], off[e][2]) * head * room_to_anchor;
                for (int k = 0; k < 3; ++k)
                    m.m[3][k] *= c.world_scale;
                t.eye_to_view[e] = m;
            }
            // The game's own vertical view (Lua units: 2 x the angle).
            const float game_sy = 1.0f / tanf(g_theatre_game_fov_lua * 3.14159265f / 720.0f);
            t.fit_s = theatre_fit_scale(w.half_h, w.distance, game_sy);
            *out = t;
            return true;
        }
        // Only the eyes' separation: half of it each side of the camera.
        const float half = 0.5f * (r[0] - l[0]);
        const float x[2] = { -half, half };
        *out = theatre_eyes(x, c.world_scale, c.theatre_depth, w.half_w, c.theatre_aspect);
        return true;
    }

    namespace
    {
        // Rotation matrix (column-vector convention) of a unit quaternion x, y, z, w.
        void quat_to_matrix(const float q[4], float m[3][3])
        {
            const float x = q[0], y = q[1], z = q[2], w = q[3];
            m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y - z * w);     m[0][2] = 2 * (x * z + y * w);
            m[1][0] = 2 * (x * y + z * w);     m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z - x * w);
            m[2][0] = 2 * (x * z - y * w);     m[2][1] = 2 * (y * z + x * w);     m[2][2] = 1 - 2 * (x * x + y * y);
        }

        // Horizon lock (docs/findings.md): the camera quaternion is camera ->
        // world, right-handed, looking down -Z, +Y up (checked against Lua's
        // GetCameraOrientation: yaw about Y times pitch about X). The leveled
        // camera keeps only its yaw. In the D3D view space the stereo works in
        // (z forward: S = diag(1, 1, -1) from camera space) the correction is
        // S * R_yaw^T * R * S, applied to view-space row vectors as its transpose.
        // Horizon lock applies now. Not while the theatre shows: it shows
        // the game's own framing. [vr] horizon_lock_off_levels: levels that
        // play better with the game's own camera (the start menu, the Milkman
        // Conspiracy).
        bool horizon_lock_applies()
        {
            const int mode = config().horizon_lock;
            return !(mode == Config::HorizonOff || game_theatre_showing() ||
                     (mode == Config::HorizonAuto &&
                      level_in_list(config().horizon_lock_off_levels, game_level_name())));
        }

        void update_horizon_lock()
        {
            float pos[3], q[4];
            if (!horizon_lock_applies() || !game_camera_read(pos, q))
            {
                vr_set_view_correction(Mat4::identity());
                return;
            }
            const float len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (!(len > 0.5f && len < 1.5f))
            {
                vr_set_view_correction(Mat4::identity());
                return;
            }
            for (float& c : q)
                c /= len;
            float r[3][3];
            quat_to_matrix(q, r);
            // Forward = R * (0, 0, -1); yaw so that R_yaw * (0, 0, -1) = (-sin, 0, -cos).
            const float fx = -r[0][2], fz = -r[2][2];
            if (fx * fx + fz * fz < 1e-6f)
                return;     // looking straight up or down: keep last frame's
            const float yaw = atan2f(-fx, -fz);
            const float cy = cosf(yaw), sy = sinf(yaw);
            const float ry[3][3] = { { cy, 0, sy }, { 0, 1, 0 }, { -sy, 0, cy } };
            // rel = R_yaw^T * R (column convention): the pitch/roll left over.
            float rel[3][3];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    rel[i][j] = ry[0][i] * r[0][j] + ry[1][i] * r[1][j] + ry[2][i] * r[2][j];
            // S rel S, then transposed for row vectors.
            Mat4 corr = Mat4::identity();
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                {
                    const float sign = ((i == 2) != (j == 2)) ? -1.0f : 1.0f;
                    corr.m[j][i] = sign * rel[i][j];
                }
            vr_set_view_correction(corr);
        }
    }

    namespace
    {
        // Live camera zoom. The chase camera's distance (Lua Get/SetChaseCameraRadius,
        // 0x56D560 / 0x56D480 -> 0x4E3940) is the float at camera + 0x968; the setter
        // also keeps an easing offset at + 0x974. Each frame the field is set to the
        // game's own value x the zoom; when the game sets a new value itself (camera
        // volumes, scripts) that becomes the base, so the two never fight.
        // Input: hold D-pad up / down on the Xbox pad (the game does not use them),
        // or the VR D-pad ([controls] dpad_modifier) when the controllers are
        // the game's pad, else the VR right stick up / down; overlay closed.
        // Also, outside god mode, the right stick clicked in and pushed right
        // (closer) / left (farther), as the Underworld mod's classic view
        // (vr_pad.h VrRightStick; its y is the height, god_frame.cpp).
        typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD, XINPUT_STATE*);

        XINPUT_GAMEPAD xinput_pad()
        {
            static PFN_XInputGetState get = nullptr;
            static bool tried = false;
            if (!tried)
            {
                tried = true;
                const wchar_t* names[] = { L"xinput1_3.dll", L"xinput1_4.dll", L"xinput9_1_0.dll" };
                for (const wchar_t* n : names)
                    if (HMODULE m = GetModuleHandleW(n))
                        if ((get = (PFN_XInputGetState)GetProcAddress(m, "XInputGetState")) != nullptr)
                            break;
                if (!get)
                    if (HMODULE m = LoadLibraryW(L"xinput1_3.dll"))
                        get = (PFN_XInputGetState)GetProcAddress(m, "XInputGetState");
            }
            // One pad a call, the connected one found by a slow scan every 2 s:
            // XInputGetState on an empty slot is slow.
            static DWORD pad = 0;
            static ULONGLONG next_scan = 0;
            XINPUT_GAMEPAD b{};
            if (!get)
                return b;
            XINPUT_STATE s{};
            if (get(pad, &s) == ERROR_SUCCESS)
                return s.Gamepad;
            const ULONGLONG now = GetTickCount64();
            if (now >= next_scan)
            {
                next_scan = now + 2000;
                for (DWORD i = 0; i < 4; ++i)
                    if (i != pad && get(i, &s) == ERROR_SUCCESS)
                    {
                        pad = i;
                        b = s.Gamepad;
                        break;
                    }
            }
            return b;
        }

        void zoom_frame()
        {
            static LARGE_INTEGER freq{}, last{};
            if (!freq.QuadPart)
                QueryPerformanceFrequency(&freq);
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            float dt = last.QuadPart ? float(double(now.QuadPart - last.QuadPart) / double(freq.QuadPart)) : 0.0f;
            last = now;
            if (dt > 0.1f)
                dt = 0.1f;

            static float zoom = -1.0f, config_seen = -1.0f;
            const float configured = config().chase_radius_scale;
            if (configured != config_seen)      // first frame, or changed in the overlay / ini
            {
                config_seen = configured;
                zoom = configured;
            }

            float dir = 0.0f;   // +1 = farther
            // Only in play: the game's menus (the pause notebook, the start
            // menu) are driven with the same D-pad, and zoomed the camera.
            if (config().zoom_input && !hud_showing_menu() && !in_start_menu())
            {
                WORD b = xinput_pad().wButtons;
                // The VR controllers as the game's pad: their D-pad
                // (dpad_modifier + a left stick slide), and the right stick
                // is the game's camera, not the zoom.
                const bool vr_pad = vr_pad_drives_game();
                XINPUT_GAMEPAD vp{};
                if (vr_pad && !overlay_open() && vr_pad_menu_state(&vp))
                    b |= vp.wButtons;
                if (b & XINPUT_GAMEPAD_DPAD_UP)
                    dir -= 1.0f;
                if (b & XINPUT_GAMEPAD_DPAD_DOWN)
                    dir += 1.0f;
                VrRightStick rs;
                if (vr_pad && !config().god_mode && !overlay_open() && vr_pad_right_stick(&rs) && rs.clicked)
                {
                    const float x = fabsf(rs.x) < 0.2f ? 0.0f : (rs.x - (rs.x > 0.0f ? 0.2f : -0.2f)) / 0.8f;
                    dir -= fminf(fmaxf(x, -1.0f), 1.0f);
                }
                float rx = 0.0f, ry = 0.0f;
                if (!vr_pad && vr_ready() && !overlay_open() && xr_input_active())
                {
                    xr_input_stick(1, &rx, &ry);
                    if (fabsf(ry) > 0.5f && fabsf(ry) > fabsf(rx))
                        dir -= ry > 0.0f ? 1.0f : -1.0f;
                }
            }
            static bool zooming = false;
            if (dir != 0.0f)
            {
                zoom *= expf(dir * 0.9f * dt);      // ~2.5x a second
                zoom = fminf(fmaxf(zoom, 0.15f), 3.0f);
                zooming = true;
            }
            else if (zooming)
            {
                zooming = false;
                wchar_t text[32];
                swprintf_s(text, L"%.3f", zoom);
                config_save_setting(L"camera", L"chase_radius_scale", text);
                config_seen = zoom;     // the reload brings back what we already have
                log("camera: zoom x%.2f saved", zoom);
            }

            __try
            {
                const uintptr_t game = *reinterpret_cast<const uintptr_t*>(0x78BC20);
                if (!game)
                    return;
                const uintptr_t cam = *reinterpret_cast<const uintptr_t*>(game + 0x8C98);
                if (!cam)
                    return;
                float* radius = reinterpret_cast<float*>(cam + 0x968);
                static uintptr_t last_cam = 0;
                static float base = 0.0f, written = -1.0f;
                const float r = *radius;
                if (cam != last_cam || fabsf(r - written) > 0.01f)
                {
                    // A new camera, or the game set its own distance.
                    last_cam = cam;
                    base = r;
                }
                if (!(base > 1.0f && base < 100000.0f))
                    return;
                // Not in the start menu: once Raz walks on the brain its camera
                // is a chase camera too, and the play zoom pushed it far out
                // (user 2026-11-16); the start menu has its own zoom.
                const float want = in_start_menu() ? base : base * zoom;
                if (fabsf(want - r) > 0.01f)
                    *radius = want;
                written = want;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
    }

    namespace
    {
        // [camera] start_menu_zoom, live in the start menu: the right stick
        // (the pad's, or the VR right stick with the overlay closed) up =
        // nearer, down = farther; saved when let go. The D-pad stays the
        // menu's.
        void start_menu_zoom_frame()
        {
            static LARGE_INTEGER freq{}, last{};
            if (!freq.QuadPart)
                QueryPerformanceFrequency(&freq);
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            float dt = last.QuadPart ? float(double(now.QuadPart - last.QuadPart) / double(freq.QuadPart)) : 0.0f;
            last = now;
            if (dt > 0.1f)
                dt = 0.1f;

            static float zoom = 0.0f;
            static bool zooming = false;
            float dir = 0.0f;   // +1 = nearer
            if (config().zoom_input && in_start_menu())
            {
                const XINPUT_GAMEPAD p = xinput_pad();
                if (abs(p.sThumbRY) > 16000 && abs(p.sThumbRY) > abs(p.sThumbRX))
                    dir += p.sThumbRY > 0 ? 1.0f : -1.0f;
                float rx = 0.0f, ry = 0.0f;
                if (dir == 0.0f && vr_ready() && !overlay_open() && xr_input_active())
                {
                    xr_input_stick(1, &rx, &ry);
                    if (fabsf(ry) > 0.5f && fabsf(ry) > fabsf(rx))
                        dir += ry > 0.0f ? 1.0f : -1.0f;
                }
            }
            if (dir != 0.0f)
            {
                if (!zooming)
                    zoom = config().start_menu_zoom;
                zooming = true;
                zoom = fminf(fmaxf(zoom + dir * 1.0f * dt, 0.0f), 5.0f);     // 1 m a second
                // Seen at once: the view follows the stick, the file the release.
                vr_set_view_forward(zoom * config().world_scale);
            }
            else if (zooming)
            {
                zooming = false;
                wchar_t text[32];
                swprintf_s(text, L"%.2f", zoom);
                config_save_setting(L"camera", L"start_menu_zoom", text);
                log("camera: start menu zoom %.2f m saved", zoom);
            }
            const bool menu_scene = in_start_menu() && !game_theatre_showing();
            if (!zooming)
                vr_set_view_forward(menu_scene ? config().start_menu_zoom * config().world_scale : 0.0f);
            // Not with the god seat: its eyes are placed without the forward move.
            vr_set_near_limit(menu_scene && !config().god_mode
                                  ? config().start_menu_near_limit * config().world_scale : 0.0f);
        }
    }

    namespace
    {
        // Settings the game itself changes as you go (zones, cutscenes, camera
        // scripts), held every frame rather than only at level start:
        //  * camera shake: the byte SetCameraShakeEnable (0x56A7C0) writes,
        //    0x788EA0 (1 = on);
        //  * the culling field of view: camera + 0x30 (GetCameraFieldOfView ->
        //    0x4CDF20), in Lua units (2 x the vertical angle); raised to
        //    [vr] cull_fov_degrees when the game's own is narrower. The stereo
        //    draws with the headset's view either way, so this only changes
        //    what the game draws. Not while the theatre shows: it draws with
        //    the game's own projection, so the game's own value goes back;
        //  * detail culling: game + 0xA7 = [vr] detail_culling.
        void hold_settings_frame()
        {
            const Config& c = config();
            if (!vr_ready())
                return;
            __try
            {
                if (!c.camera_shake)
                    *reinterpret_cast<volatile unsigned char*>(0x788EA0) = 0;
                const uintptr_t game = *reinterpret_cast<const uintptr_t*>(0x78BC20);
                if (!game)
                    return;
                // DetailCulling (game + 0xA7; the debug flag table at 0x5728xx,
                // flag id 0x7B + 0x2C): 1 fades out small far things by their
                // size on screen (0x43EA73). Held, since the game sets it.
                *reinterpret_cast<volatile unsigned char*>(game + 0xA7) = c.detail_culling ? 1 : 0;
                const uintptr_t cam = *reinterpret_cast<const uintptr_t*>(game + 0x8C98);
                if (!cam)
                    return;
                float* fov = reinterpret_cast<float*>(cam + 0x30);
                const float want = c.cull_fov_degrees * 2.0f;
                // The game's own value: the last one narrower than ours. Ours
                // (also written by the Lua hook at level start) and god
                // mode's (+0x30, god_frame.cpp) are never narrower.
                // The theatre window's (wider than the game's own) is ours too.
                static uintptr_t own_cam = 0;
                static float own = 0.0f, theatre_written = 0.0f;
                if (cam != own_cam)
                {
                    own_cam = cam;
                    own = theatre_written = 0.0f;
                }
                if (c.cull_fov_degrees <= 0.0f)
                    return;
                if (*fov > 1.0f && *fov < want - 0.5f && fabsf(*fov - theatre_written) > 0.01f)
                    own = *fov;
                // Unknown: the chase camera's usual 52 degrees, Lua 104.
                g_theatre_game_fov_lua = own > 1.0f ? own : 104.0f;
                if (game_theatre_showing())
                {
                    // The screen: the game's own. The window: wider by
                    // kTheatreWindowCull (in tan), since leaning and turning
                    // look past the game's shot; never past ours.
                    float target = g_theatre_game_fov_lua;
                    if (c.theatre_style == kTheatreWindow)
                    {
                        const float k = 3.14159265f / 720.0f;   // Lua units -> half angle, radians
                        target = fminf(atanf(tanf(target * k) * kTheatreWindowCull) / k, want);
                    }
                    *fov = target;
                    theatre_written = target;
                    return;
                }
                theatre_written = 0.0f;
                if (*fov > 1.0f && *fov < want)
                    *fov = want;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
    }

    namespace
    {
        // God mode ([god], src/vr/god_seat.h, docs/findings.md). The chase
        // camera's update FUN_004DB540 (thiscall: camera, input, dt) is
        // called by the camera update FUN_004CAEF0 when the camera's type
        // (*(int*)camera) is 2, chase. After it, 4CAEF0 turns the camera to
        // look from its position (+0x08) at its target (FUN_004D1800) and
        // builds its matrices from +0x08 / +0x20, which the game culls and
        // draws with. So the seat goes into +0x08 right after the chase
        // update: the game's own view, culling included, is then the seat's,
        // looking at Raz. The game's own position is put back before its
        // next chase update, so its spring, wall collision and orbit never
        // see the seat. Script cameras (cutscenes) and the other types never
        // run the chase update: those frames are the game's own, life size.
        typedef void(__fastcall* PFN_ChaseUpdate)(char* camera, void* edx, void* input, float dt);
        PFN_ChaseUpdate g_chase_update = nullptr;

        // The seat's running state: here, so it outlives a reload of the
        // logic that updates it (god_frame.h).
        GodSeatState g_seat;

        // The logic's writes into the camera (god_frame.h: the seat at +0x08,
        // with [god] aim_with_head the gaze at +0x20 and the cull field of
        // view at +0x30), applied right after the chase update. The bytes
        // they replaced go back, last first, before the camera's next chase
        // update, so its spring, wall collision and orbit never see them.
        char* g_written_camera = nullptr;
        GodFrameOut g_out;
        unsigned char g_saved[kGodMaxWrites][sizeof(GodWrite::bytes)];
        unsigned g_saved_count = 0;

        // This frame's seat for the draws (god_seat_source); `ran` is set by
        // a chase update that placed it and cleared at Present.
        SRWLOCK g_god_lock = SRWLOCK_INIT;
        struct GodShared
        {
            bool ran = false;
            char* camera = nullptr;
            float pos[3] = {};
            float rot[3][3] = {};
            float scale = 1.0f;
        };
        GodShared g_god;

        bool read_vec3(uintptr_t at, float out[3])
        {
            __try
            {
                memcpy(out, reinterpret_cast<const void*>(at), 12);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
        }

        // Raz's position: the player entity *(game + 0x818C), its body at
        // entity + 0x10, the position at body + 0x40 (what Lua GetPosition,
        // 0x5C1A40 -> 0x5C0C70, reads).
        bool read_raz(float out[3])
        {
            __try
            {
                const uintptr_t game = *reinterpret_cast<const uintptr_t*>(0x78BC20);
                const uintptr_t raz = game ? *reinterpret_cast<const uintptr_t*>(game + 0x818C) : 0;
                const uintptr_t body = raz ? *reinterpret_cast<const uintptr_t*>(raz + 0x10) : 0;
                return body && read_vec3(body + 0x40, out);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // Raz's forward: the third row of his body matrix (body + 0x10, rows
        // of four floats: right, up, forward), as Psy-Where reads it.
        bool read_raz_forward(float out[3])
        {
            __try
            {
                const uintptr_t game = *reinterpret_cast<const uintptr_t*>(0x78BC20);
                const uintptr_t raz = game ? *reinterpret_cast<const uintptr_t*>(game + 0x818C) : 0;
                const uintptr_t body = raz ? *reinterpret_cast<const uintptr_t*>(raz + 0x10) : 0;
                return body && read_vec3(body + 0x30, out);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // View reset (a tap of the right stick click, vr_pad.h): pending
        // until this time, taken by the next chase update in the normal
        // chase (not the first-person look, which the tap leaves first).
        ULONGLONG g_view_reset_until = 0;

        // The chase camera's orbit (docs/findings.md 2026-11-16, "View
        // reset"): Lua SetChaseCameraAzimuth -> FUN_004E3990 (thiscall:
        // camera, azimuth in degrees) turns it, moving the camera at once;
        // the azimuth is the heading from the camera to its target,
        // atan2(x, z) of the look. The right stick's orbit up / down is the
        // offset +0x9F0 (degrees) on the altitude +0x96C (the game's 14).
        typedef void(__fastcall* PFN_SetAzimuth)(char* camera, void* edx, float degrees);

        // The chase camera behind Raz, at its own height. False if Raz's
        // facing could not be read.
        bool chase_reset(char* camera, float raz_fwd[3])
        {
            if (!read_raz_forward(raz_fwd))
                return false;
            const float h = sqrtf(raz_fwd[0] * raz_fwd[0] + raz_fwd[2] * raz_fwd[2]);
            if (!(h > 1e-3f))
                return false;
            const float az = atan2f(raz_fwd[0], raz_fwd[2]) * 57.29578f;
            __try
            {
                reinterpret_cast<PFN_SetAzimuth>(0x4E3990)(camera, nullptr, az);
                *reinterpret_cast<float*>(camera + 0x9F0) = 0.0f;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            log("view reset: chase camera behind Raz (azimuth %.1f deg), orbit height back to the game's", az);
            return true;
        }

        // game_psy.h: game_lock_target. Addresses: docs/findings.md
        // (2026-11-15, "The lock target, found"; FUN_0053B200).
        bool read_lock_target(float pos[3], float view[4][4])
        {
            __try
            {
                const uintptr_t game = *reinterpret_cast<const uintptr_t*>(0x78BC20);
                const uintptr_t raz = game ? *reinterpret_cast<const uintptr_t*>(game + 0x818C) : 0;
                if (!raz || *reinterpret_cast<const unsigned char*>(raz + 0x184) == 0)
                    return false;
                const uintptr_t target = *reinterpret_cast<const uintptr_t*>(raz + 0x80);
                if (!target || target == raz)
                    return false;
                const uintptr_t body = *reinterpret_cast<const uintptr_t*>(target + 0x10);
                if (!body || !read_vec3(body + 0x40, pos))
                    return false;
                const uintptr_t camera = *reinterpret_cast<const uintptr_t*>(game + 0x8C98);
                if (!camera)
                    return false;
                memcpy(view, reinterpret_cast<const void*>(camera + 0x50), 64);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    if (!std::isfinite(view[r][c]))
                        return false;
            return fabsf(view[3][3] - 1.0f) < 1e-3f;
        }

        bool read_camera_rot(const char* camera, float rot[3][3])
        {
            float q[4];
            __try
            {
                memcpy(q, camera + 0x20, 16);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            const float len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (!(len > 0.5f && len < 1.5f))
                return false;
            for (float& c : q)
                c /= len;
            god_quat_to_rot(q, rot);
            return true;
        }

        // The view this chase camera is shown with now (god_frame.h GodView),
        // or -1 = none of ours (not in VR, the theatre showing (the game's
        // own camera, as classic), the start menu, or not
        // the camera the game draws with). God mode and the level seat
        // (classic view under horizon lock) only in the normal chase
        // (sub-mode +0x908: 0); the first-person look (2) and the others are
        // plain. *first_person: sub-mode 2.
        int chase_view(const char* camera, bool* first_person)
        {
            *first_person = false;
            const Config& c = config();
            if (!vr_ready() || game_theatre_showing() || in_start_menu())
                return -1;
            int sub = -1;
            __try
            {
                const uintptr_t game = *reinterpret_cast<const uintptr_t*>(0x78BC20);
                if (!game || *reinterpret_cast<char* const*>(game + 0x8C98) != camera)
                    return -1;
                sub = *reinterpret_cast<const int*>(camera + 0x908);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return -1;
            }
            *first_person = sub == 2;
            if (sub != 0)
                return kGodViewPlain;
            return c.god_mode ? kGodViewGod : horizon_lock_applies() ? kGodViewLevel : kGodViewPlain;
        }

        // The active camera is the chase camera in its first-person look.
        bool in_first_person()
        {
            __try
            {
                const uintptr_t game = *reinterpret_cast<const uintptr_t*>(0x78BC20);
                const char* cam = game ? *reinterpret_cast<char* const*>(game + 0x8C98) : nullptr;
                return cam && *reinterpret_cast<const int*>(cam) == 2 && *reinterpret_cast<const int*>(cam + 0x908) == 2;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // Puts back what the logic's writes replaced; true if the position
        // (+0x08) was among them.
        bool undo_writes(char* camera)
        {
            bool moved = false;
            for (unsigned i = g_saved_count; i-- > 0;)
            {
                const GodWrite& w = g_out.writes[i];
                __try
                {
                    memcpy(camera + w.offset, g_saved[i], w.size);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
                moved = moved || (w.offset < 0x14 && w.offset + w.size > 0x08);
            }
            return moved;
        }

        // Applies the logic's writes, keeping what they replace; stops at the
        // first that fails (nothing after it is undone either).
        void apply_writes(char* camera)
        {
            g_saved_count = 0;
            for (unsigned i = 0; i < g_out.write_count && i < kGodMaxWrites; ++i)
            {
                const GodWrite& w = g_out.writes[i];
                if (w.size == 0 || w.size > sizeof(w.bytes) || w.offset + w.size > kGodWriteSpan)
                    break;
                __try
                {
                    memcpy(g_saved[i], camera + w.offset, w.size);
                    memcpy(camera + w.offset, w.bytes, w.size);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    break;
                }
                g_saved_count = i + 1;
            }
            g_written_camera = g_saved_count ? camera : nullptr;
        }

        void __fastcall chase_update_detour(char* camera, void* edx, void* input, float dt)
        {
            if (g_written_camera == camera && undo_writes(camera))
            {
                // The game's own position is also the "previous position"
                // 4CAEF0 copied to +0x14 before calling us.
                __try
                {
                    memcpy(camera + 0x14, camera + 0x08, 12);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }
            g_written_camera = nullptr;
            g_saved_count = 0;
            g_chase_update(camera, edx, input, dt);

            // Wall-clock seconds between calls, for the view glide.
            static LARGE_INTEGER freq{}, last{};
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            if (!freq.QuadPart)
                QueryPerformanceFrequency(&freq);
            const float real_dt = last.QuadPart ? (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart : 0.0f;
            last = now;

            GodFrameIn in{};
            bool first_person = false;
            const int view = chase_view(camera, &first_person);
            // The theatre shows the game's own camera (chase_view -1): turned
            // there too, without a glide.
            bool normal_chase = false;
            if (view < 0 && game_theatre_showing())
            {
                __try
                {
                    const uintptr_t game = *reinterpret_cast<const uintptr_t*>(0x78BC20);
                    normal_chase = game && *reinterpret_cast<char* const*>(game + 0x8C98) == camera &&
                                   *reinterpret_cast<const int*>(camera + 0x908) == 0;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }
            if (((view >= 0 && !first_person) || normal_chase) && g_view_reset_until &&
                GetTickCount64() < g_view_reset_until)
            {
                g_view_reset_until = 0;
                in.reset = true;
                // God mode's seat turns in the logic; the game's own camera
                // too, so the classic view is behind Raz when it comes back.
                in.raz_fwd_ok = chase_reset(camera, in.raz_fwd);
            }
            if (view < 0 || (view == kGodViewGod && !read_raz(in.raz)) || !read_camera_rot(camera, in.cam_rot) ||
                !read_vec3((uintptr_t)camera + 0x08, in.cam_pos))
            {
                // Not ours (a cutscene, a scripted camera): the next view
                // starts with a cut, as the game's own does.
                g_seat.have = false;
                g_seat.view_kind = -1;
                g_seat.glide = 1.0f;
                return;
            }
            in.camera = camera;
            in.view = view;
            in.first_person = first_person;
            in.lock = horizon_lock_applies();
            in.real_dt = real_dt;
            in.dt_ms = dt;
            VrRightStick rs;
            in.pad_ok = !overlay_open() && vr_pad_right_stick(&rs);
            in.stick_x = rs.x;
            in.stick_y = rs.y;
            in.stick_clicked = rs.clicked;
            in.stick_settling = rs.settling;
            static unsigned recentres = 0;
            in.relatch = vr_recenter_count() != recentres;
            recentres = vr_recenter_count();
            in.head_ok = vr_head_in_room_metres(&in.head);
            in.fov_ok = vr_eye_fov_extent(&in.fov_up, &in.fov_down, &in.fov_side);
            if (!logic_god_frame(in, g_seat, g_out))
            {
                g_seat.have = false;
                g_seat.view_kind = -1;
                g_seat.glide = 1.0f;
                return;
            }
            apply_writes(camera);
            // A plain view on its own leaves the draws to the game camera.
            if (!g_out.seat)
                return;
            GodShared s;
            s.ran = true;
            s.camera = camera;
            memcpy(s.pos, g_out.pos, sizeof(s.pos));
            memcpy(s.rot, g_out.rot, sizeof(s.rot));
            s.scale = g_out.scale;
            AcquireSRWLockExclusive(&g_god_lock);
            g_god = s;
            ReleaseSRWLockExclusive(&g_god_lock);
        }

        // vr_set_view_seat's source, at draw time: the seat against the camera
        // as the game draws with it now.
        bool god_seat_source(Mat4* seat_to_view, float* scale)
        {
            AcquireSRWLockShared(&g_god_lock);
            const GodShared s = g_god;
            ReleaseSRWLockShared(&g_god_lock);
            float cam_pos[3], cam_rot[3][3];
            if (!s.ran || !read_vec3((uintptr_t)s.camera + 0x08, cam_pos) || !read_camera_rot(s.camera, cam_rot))
                return false;
            if (!logic_god_seat_to_view(s.pos, s.rot, cam_pos, cam_rot, seat_to_view))
                return false;
            *scale = s.scale;
            return true;
        }

        void god_install()
        {
            // push ebp; mov ebp, esp; and esp, -16
            static const unsigned char kChaseUpdateBytes[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
            if (!g_chase_update &&
                hook_install(reinterpret_cast<void*>(0x4DB540), reinterpret_cast<void*>(&chase_update_detour),
                             reinterpret_cast<void**>(&g_chase_update), kChaseUpdateBytes,
                             sizeof(kChaseUpdateBytes), "chase camera update (god mode)"))
                vr_set_view_seat(&god_seat_source);
        }

        // At Present: the next frame's draws use the seat only if its chase
        // update places it again.
        void god_present()
        {
            AcquireSRWLockExclusive(&g_god_lock);
            const bool was = g_god.ran;
            g_god.ran = false;
            ReleaseSRWLockExclusive(&g_god_lock);
            static bool last = false;
            if (last && !was && !g_seat.have)
                log("god: seat off (not the gameplay chase camera, or god mode / horizon lock off)");
            last = was;
        }

        // Detail culling (docs/findings.md, 2026-11-16). FUN_004D03B0 (thiscall:
        // camera, box) is an object's size on screen in pixels: its box through
        // the camera's view-projection (+0x150) times half the width / height
        // (+0x40 / +0x44). Objects past 8000 units fade out between 400 and 10
        // pixels of it (entities at 0x43E8EC; render items at 0x448F8B with
        // their own range; both via FUN_0050E470, the largest size over the
        // cameras), so the wide cull field of view we give the game in VR
        // (+0x30) also shrank them below that: distant tree tops vanished in
        // the middle of the view and came back towards its edges, where the
        // depth is smaller. The size is scaled back to what the chase camera's
        // own field of view gives (52 deg vertical, Lua 104).
        typedef float(__fastcall* PFN_ScreenSize)(char* camera, void* edx, const void* box);
        PFN_ScreenSize g_screen_size = nullptr;
        constexpr float kDetailRefFovLua = 104.0f;

        float __fastcall screen_size_detour(char* camera, void* edx, const void* box)
        {
            const float size = g_screen_size(camera, edx, box);
            if (!vr_ready())
                return size;
            float fov = 0.0f;
            __try
            {
                fov = *reinterpret_cast<const float*>(camera + 0x30);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return size;
            }
            // Lua units: 2 x the vertical angle in degrees, so half of it in
            // radians is fov * pi / 720.
            if (!(fov > kDetailRefFovLua && fov < 350.0f))
                return size;
            const float k = 3.14159265f / 720.0f;
            return size * tanf(fov * k) / tanf(kDetailRefFovLua * k);
        }

        void detail_install()
        {
            // push ebp; mov ebp, esp; sub esp, 0x44
            static const unsigned char kScreenSizeBytes[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x44 };
            if (!g_screen_size)
                hook_install(reinterpret_cast<void*>(0x4D03B0), reinterpret_cast<void*>(&screen_size_detour),
                             reinterpret_cast<void**>(&g_screen_size), kScreenSizeBytes, sizeof(kScreenSizeBytes),
                             "screen size for detail culling");
        }
    }

    void game_frame()
    {
        god_present();
        // [camera] start_menu_zoom: nearer the start menu's 3D scene.
        start_menu_zoom_frame();
        {
            static char last[32] = "";
            const char* level = game_level_name();
            if (strcmp(level, last) != 0)
            {
                strcpy_s(last, level);
                log("game: level %s", level[0] ? level : "(none)");
            }
        }
        theatre_frame();
        update_horizon_lock();
        // A hold of the right stick click switches the view (vr_pad.h):
        // classic -> god mode -> first person ([controls]
        // first_person_in_cycle) -> classic. The first-person look is the
        // game's own, entered and left with a short R3 as a tap does; the
        // eyes glide between them (god_frame.cpp).
        first_person_patch_frame();
        if (vr_pad_take_god_toggle())
        {
            const Config& c = config();
            if (in_first_person())
            {
                vr_pad_press_r3();
                if (c.god_mode)
                    config_save_setting(L"god", L"enabled", L"0");
                log("view: first person -> classic (right stick held)");
            }
            else if (c.god_mode)
            {
                config_save_setting(L"god", L"enabled", L"0");
                if (c.controls.first_person_in_cycle)
                {
                    vr_pad_press_r3();
                    log("view: god mode -> first person (right stick held)");
                }
                else
                    log("view: god mode -> classic (right stick held)");
            }
            else
            {
                config_save_setting(L"god", L"enabled", L"1");
                log("view: classic -> god mode (right stick held)");
            }
        }
        // A tap of the right stick click: the view reset (chase_reset, the
        // next chase update takes it). From the first-person look, leave it
        // first (the game's R3).
        if (vr_pad_take_view_reset())
        {
            if (in_first_person())
                vr_pad_press_r3();
            g_view_reset_until = GetTickCount64() + 1500;
            log("view: reset asked for (right stick tapped%s)", in_first_person() ? ", leaving first person" : "");
        }
        zoom_frame();
        hold_settings_frame();
        // The VR controllers as the game's pad 0 (vr_pad.h).
        vr_pad_frame();
        vr_keyboard_frame();
        // [camera] changed in the overlay: rewrite the Lua hook (it is read again
        // at every level load).
        {
            static float cull = -1.0f;
            static int shake = -1;
            const Config& c = config();
            if (cull >= 0.0f && (cull != c.cull_fov_degrees || shake != (int)c.camera_shake))
                game_write_lua_mod(vr_ready());
            cull = c.cull_fov_degrees;
            shake = (int)c.camera_shake;
        }
        // Runs queued game-thread control commands (`status`).
        control_game_update();
    }

    namespace
    {
        // The game imports XInputGetState / XInputSetState from xinput1_3 by
        // ordinal (2, 3) and calls them only through two thunks,
        // jmp [0x700414] at 0x6ECD8A and jmp [0x700418] at 0x6ECD90. The
        // import slots are in .rdata, so they are made writable once.
        void* xinput_slot(uintptr_t slot)
        {
            static int ok = -1;
            if (ok < 0)
            {
                __try
                {
                    const unsigned char get[6] = { 0xFF, 0x25, 0x14, 0x04, 0x70, 0x00 };
                    const unsigned char set[6] = { 0xFF, 0x25, 0x18, 0x04, 0x70, 0x00 };
                    ok = memcmp(reinterpret_cast<const void*>(0x6ECD8A), get, 6) == 0 &&
                         memcmp(reinterpret_cast<const void*>(0x6ECD90), set, 6) == 0;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    ok = 0;
                }
                DWORD old = 0;
                if (ok && !VirtualProtect(reinterpret_cast<void*>(0x700414), 8, PAGE_READWRITE, &old))
                    ok = 0;
                if (!ok)
                    log("pad: the game's XInput thunks are not where expected; the VR controllers drive only the overlay");
            }
            return ok ? reinterpret_cast<void*>(slot) : nullptr;
        }
    }

    void* game_xinput_get_state_slot()
    {
        return xinput_slot(0x700414);
    }

    void* game_xinput_set_state_slot()
    {
        return xinput_slot(0x700418);
    }

    bool game_lock_target(float pos[3], float view[4][4])
    {
        return read_lock_target(pos, view);
    }
}
