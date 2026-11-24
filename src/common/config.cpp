#include "config.h"
#include "default_ini.h"
#include "log.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace psyvr
{
    namespace
    {
        Config g_config;
        unsigned g_added = 0;
        wchar_t g_path[MAX_PATH] = L"";
        bool g_created_default = false;

        // Warnings about values that were out of range. The log is not open
        // yet when the file is first read (dllmain opens it afterwards), so
        // they are held and written by config_flush_warnings().
        char g_warnings[24][200];
        unsigned g_warning_count = 0;
        bool g_log_ready = false;

        void warn(const char* fmt, ...)
        {
            char line[200];
            va_list args;
            va_start(args, fmt);
            vsnprintf(line, sizeof(line), fmt, args);
            va_end(args);
            if (g_log_ready)
            {
                log("config: %s", line);
            }
            else if (g_warning_count < 24)
            {
                strcpy_s(g_warnings[g_warning_count], line);
                ++g_warning_count;
            }
        }

        // Keeps a numeric setting inside its sensible range, saying so.
        void limit_int(const char* name, int& v, int lo, int hi)
        {
            const int was = v;
            if (v < lo) v = lo;
            if (v > hi) v = hi;
            if (v != was)
                warn("%s = %d is outside %d..%d; using %d", name, was, lo, hi, v);
        }

        void limit_float(const char* name, float& v, float lo, float hi, float fallback)
        {
            const float was = v;
            if (!(v >= lo && v <= hi))      // also catches NaN
            {
                v = (v != v) ? fallback : (v < lo ? lo : hi);
                warn("%s = %g is outside %g..%g; using %g", name, (double)was, (double)lo,
                     (double)hi, (double)v);
            }
        }

        bool has_key(const wchar_t* section, const wchar_t* key)
        {
            wchar_t probe[8]{};
            // A sentinel default no real value would use.
            GetPrivateProfileStringW(section, key, L"\x01", probe, 8, g_path);
            return probe[0] != L'\x01';
        }

        void ensure(const wchar_t* section, const wchar_t* key, const wchar_t* value)
        {
            if (has_key(section, key))
                return;
            WritePrivateProfileStringW(section, key, value, g_path);
            ++g_added;
        }

        void read_string(const wchar_t* section, const wchar_t* key,
                         wchar_t* out, DWORD size)
        {
            wchar_t def[MAX_PATH]{};
            wcscpy_s(def, out);
            ensure(section, key, def);
            GetPrivateProfileStringW(section, key, def, out, size, g_path);
        }

        int read_int(const wchar_t* section, const wchar_t* key, int def)
        {
            wchar_t text[32]{};
            swprintf_s(text, L"%d", def);
            ensure(section, key, text);
            return (int)GetPrivateProfileIntW(section, key, def, g_path);
        }

        float read_float(const wchar_t* section, const wchar_t* key, float def)
        {
            wchar_t text[64]{};
            swprintf_s(text, L"%g", def);
            ensure(section, key, text);
            wchar_t out[64]{};
            GetPrivateProfileStringW(section, key, text, out, 64, g_path);
            wchar_t* end = nullptr;
            const float v = wcstof(out, &end);
            return end != out ? v : def;
        }

        // "F8", "R", "7" -> a virtual-key code; "none" or unknown -> 0.
        int read_key(const wchar_t* section, const wchar_t* key, int def_vk, const wchar_t* def_name)
        {
            wchar_t text[16]{};
            wcscpy_s(text, def_name);
            read_string(section, key, text, 16);
            if (!text[0] || _wcsicmp(text, L"none") == 0)
                return 0;
            if ((text[0] == L'F' || text[0] == L'f') && text[1] >= L'1' && text[1] <= L'9')
            {
                const int n = _wtoi(text + 1);
                if (n >= 1 && n <= 24)
                    return 0x6F + n;                        // VK_F1 = 0x70
            }
            if (text[1] == 0)
            {
                const wchar_t ch = (wchar_t)towupper(text[0]);
                if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9'))
                    return (int)ch;
            }
            log("config: %S = %S is not a key (F1..F24, A..Z, 0..9, none); using %S", key, text, def_name);
            return def_vk;
        }

        struct SourceName { const wchar_t* name; ControlSource src; };
        const SourceName kSources[] = {
            { L"none", SrcNone },
            { L"right_a", SrcRightPrimary },   { L"right_b", SrcRightSecondary },
            { L"left_x", SrcLeftPrimary },     { L"left_y", SrcLeftSecondary },
            { L"left_menu", SrcLeftMenu },     { L"right_menu", SrcRightMenu },
            { L"left_stick", SrcLeftStick },   { L"right_stick", SrcRightStick },
            { L"left_grip", SrcLeftGrip },     { L"right_grip", SrcRightGrip },
            { L"left_trigger", SrcLeftTrigger }, { L"right_trigger", SrcRightTrigger },
        };

        ControlSource read_source(const wchar_t* key, ControlSource def)
        {
            const wchar_t* def_name = L"none";
            for (const auto& s : kSources)
                if (s.src == def)
                    def_name = s.name;
            wchar_t text[32]{};
            wcscpy_s(text, def_name);
            read_string(L"controls", key, text, 32);
            for (const auto& s : kSources)
                if (_wcsicmp(s.name, text) == 0)
                    return s.src;
            log("config: [controls] %S = %S is not a control; using %S", key, text, def_name);
            return def;
        }


        bool read_bool(const wchar_t* section, const wchar_t* key, bool def)
        {
            return read_int(section, key, def ? 1 : 0) != 0;
        }

        // A setting with named values ("off", "auto", "on"): the name, or its
        // index as a number. Unknown -> warn and keep the default.
        int read_choice(const wchar_t* section, const wchar_t* key,
                        const wchar_t* const* names, int count, int def)
        {
            wchar_t text[32]{};
            wcscpy_s(text, def >= 0 && def < count ? names[def] : L"");
            read_string(section, key, text, 32);
            for (int i = 0; i < count; ++i)
                if (_wcsicmp(text, names[i]) == 0)
                    return i;
            wchar_t* end = nullptr;
            const long n = wcstol(text, &end, 10);
            if (end != text && *end == 0 && n >= 0 && n < count)
                return (int)n;
            log("config: [%S] %S = %S is not one of the choices; using %S", section, key, text,
                names[def]);
            return def;
        }

        const wchar_t* const kHorizonLockNames[] = { L"off", L"auto", L"on" };

    }

    const Config& config()
    {
        return g_config;
    }

    unsigned config_defaults_added()
    {
        return g_added;
    }

    namespace
    {
    void read_all(Config& c)
    {
        read_string(L"proxy", L"backend", c.backend, MAX_PATH);
        c.logging = read_bool(L"proxy", L"logging", c.logging);

        c.force_windowed = read_bool(L"window", L"force_windowed", c.force_windowed);
        c.window_width = read_int(L"window", L"width", c.window_width);
        c.window_height = read_int(L"window", L"height", c.window_height);
        c.window_borderless = read_bool(L"window", L"borderless", c.window_borderless);
        c.pause_when_unfocused = read_bool(L"window", L"pause_when_unfocused", c.pause_when_unfocused);
        c.release_cursor_unfocused =
            read_bool(L"window", L"release_cursor_unfocused", c.release_cursor_unfocused);

        c.msaa = read_int(L"graphics", L"msaa", c.msaa);
        c.max_frame_rate = read_int(L"graphics", L"max_frame_rate", c.max_frame_rate);
        c.fast_fades = read_bool(L"graphics", L"fast_fades", c.fast_fades);
        c.texture_pack_compat = read_bool(L"graphics", L"texture_pack_compat", c.texture_pack_compat);
        c.vsync = read_int(L"graphics", L"vsync", c.vsync);
        c.video_memory_mb = read_int(L"graphics", L"video_memory_mb", c.video_memory_mb);

        c.vr_enabled = read_bool(L"vr", L"enabled", c.vr_enabled);
        read_string(L"vr", L"xr_runtime_json", c.xr_runtime_json, MAX_PATH);
        c.resolution_scale = read_float(L"vr", L"resolution_scale", c.resolution_scale);
        c.world_scale = read_float(L"vr", L"world_scale", c.world_scale);
        c.cull_fov_degrees = read_float(L"vr", L"cull_fov_degrees", c.cull_fov_degrees);
        c.detail_culling = read_bool(L"vr", L"detail_culling", c.detail_culling);
        c.loading_screens = read_int(L"vr", L"loading_screens", c.loading_screens);
        limit_int("[vr] loading_screens", c.loading_screens, 0, 2);
        c.horizon_lock = read_choice(L"vr", L"horizon_lock", kHorizonLockNames, 3,
                                     Config::HorizonAuto);
        read_string(L"vr", L"horizon_lock_off_levels", c.horizon_lock_off_levels,
                    _countof(c.horizon_lock_off_levels));
        // [theatre] mode. Before it, enabled = 1 was the theatre all the time
        // (now 2); enabled = 0 predates the cutscene theatre, so it keeps the
        // default. The key is written with the result.
        if (!has_key(L"theatre", L"mode") && has_key(L"theatre", L"enabled") &&
            read_bool(L"theatre", L"enabled", false))
            c.theatre_mode = 2;
        c.theatre_mode = read_int(L"theatre", L"mode", c.theatre_mode);
        c.theatre_style = read_int(L"theatre", L"style", c.theatre_style);
        c.theatre_depth = read_float(L"theatre", L"depth", c.theatre_depth);
        c.theatre_distance = read_float(L"theatre", L"distance", c.theatre_distance);
        c.theatre_width_degrees = read_float(L"theatre", L"width_degrees", c.theatre_width_degrees);
        c.theatre_aspect = read_float(L"theatre", L"aspect", c.theatre_aspect);
        c.recenter_vk = read_key(L"vr", L"recenter_key", c.recenter_vk, L"F8");
        c.chase_radius_scale = read_float(L"camera", L"chase_radius_scale", c.chase_radius_scale);
        c.camera_shake = read_bool(L"camera", L"camera_shake", c.camera_shake);
        c.first_person_stays = read_bool(L"camera", L"first_person_stays", c.first_person_stays);
        c.zoom_input = read_bool(L"camera", L"zoom_input", c.zoom_input);
        c.start_menu_zoom = read_float(L"camera", L"start_menu_zoom", c.start_menu_zoom);
        c.start_menu_near_limit = read_float(L"camera", L"start_menu_near_limit", c.start_menu_near_limit);
        c.god_mode = read_bool(L"god", L"enabled", c.god_mode);
        c.god_raz_height_cm = read_float(L"god", L"raz_height_cm", c.god_raz_height_cm);
        c.god_distance = read_float(L"god", L"distance", c.god_distance);
        c.god_height = read_float(L"god", L"height", c.god_height);
        c.god_leash = read_float(L"god", L"leash", c.god_leash);
        c.god_leash_height = read_float(L"god", L"leash_height", c.god_leash_height);
        c.god_turn_speed = read_float(L"god", L"turn_speed", c.god_turn_speed);
        c.god_steer_camera = read_bool(L"god", L"steer_camera", c.god_steer_camera);
        c.god_aim_with_head = read_bool(L"god", L"aim_with_head", c.god_aim_with_head);
        c.hud_enabled = read_bool(L"hud", L"enabled", c.hud_enabled);
        c.hud_distance = read_float(L"hud", L"distance", c.hud_distance);
        c.hud_width_degrees = read_float(L"hud", L"width_degrees", c.hud_width_degrees);
        c.hud_menu_distance = read_float(L"hud", L"menu_distance", c.hud_menu_distance);
        c.hud_menu_width_degrees = read_float(L"hud", L"menu_width_degrees", c.hud_menu_width_degrees);
        c.hud_menu_aspect = read_float(L"hud", L"menu_aspect", c.hud_menu_aspect);
        c.hud_aspect = read_float(L"hud", L"aspect", c.hud_aspect);
        c.hud_world_locked = read_bool(L"hud", L"world_locked", c.hud_world_locked);
        c.hud_body_locked = read_bool(L"hud", L"body_locked", c.hud_body_locked);
        c.hud_lock_reticle = read_bool(L"hud", L"lock_reticle", c.hud_lock_reticle);
        c.hud_lock_reticle_height = read_float(L"hud", L"lock_reticle_height", c.hud_lock_reticle_height);
        c.hud_lock_reticle_scale = read_float(L"hud", L"lock_reticle_scale", c.hud_lock_reticle_scale);
        c.hud_screen_fades = read_int(L"hud", L"screen_fades", c.hud_screen_fades);
        c.hud_screen_flashes = read_int(L"hud", L"screen_flashes", c.hud_screen_flashes);
        c.overlay_vr_distance = read_float(L"overlay", L"vr_distance", c.overlay_vr_distance);
        c.overlay_vr_width_degrees = read_float(L"overlay", L"vr_width_degrees", c.overlay_vr_width_degrees);
        c.overlay_vr_yaw_degrees = read_float(L"overlay", L"vr_yaw_degrees", c.overlay_vr_yaw_degrees);
        c.overlay_vr_pitch_degrees = read_float(L"overlay", L"vr_pitch_degrees", c.overlay_vr_pitch_degrees);
        c.overlay_vr_open_hold = read_float(L"overlay", L"vr_open_hold", c.overlay_vr_open_hold);
        ControlMap& m = c.controls;
        m.enabled = read_bool(L"controls", L"enabled", m.enabled);
        m.a = read_source(L"a", m.a);
        m.b = read_source(L"b", m.b);
        m.x = read_source(L"x", m.x);
        m.y = read_source(L"y", m.y);
        m.start = read_source(L"start", m.start);
        m.back = read_source(L"back", m.back);
        m.lb = read_source(L"lb", m.lb);
        m.rb = read_source(L"rb", m.rb);
        m.l3 = read_source(L"l3", m.l3);
        m.r3 = read_source(L"r3", m.r3);
        m.dpad_modifier = read_source(L"dpad_modifier", m.dpad_modifier);
        m.rumble = read_float(L"controls", L"rumble", m.rumble);
        m.god_toggle_hold = read_float(L"controls", L"god_toggle_hold", m.god_toggle_hold);
        m.first_person_in_cycle = read_bool(L"controls", L"first_person_in_cycle", m.first_person_in_cycle);
        m.view_transition_seconds = read_float(L"controls", L"view_transition_seconds", m.view_transition_seconds);

        c.debug_head_yaw = read_float(L"debug", L"head_yaw_test", c.debug_head_yaw);
        c.debug_head_wobble = read_float(L"debug", L"head_wobble", c.debug_head_wobble);
        c.debug_head_sway = read_float(L"debug", L"head_sway_test", c.debug_head_sway);
        c.debug_hang_exit = read_float(L"debug", L"hang_exit_seconds", c.debug_hang_exit);
        c.debug_skip_fullscreen = read_bool(L"debug", L"skip_fullscreen_passes", c.debug_skip_fullscreen);
        c.debug_hot_reload = read_bool(L"debug", L"hot_reload", c.debug_hot_reload);
        c.debug_control_pipe = read_bool(L"debug", L"control_pipe", c.debug_control_pipe);

        // Sensible ranges: a typo should not become a 100000 m HUD or a
        // negative window.
        limit_int("[window] width", c.window_width, 0, 16384);
        limit_int("[window] height", c.window_height, 0, 16384);
        limit_int("[graphics] msaa", c.msaa, 0, 16);
        limit_int("[graphics] max_frame_rate", c.max_frame_rate, -1000, 1000);
        limit_int("[graphics] vsync", c.vsync, -1, 1);
        if (c.video_memory_mb != 0)
            limit_int("[graphics] video_memory_mb", c.video_memory_mb, 256, 4096);
        limit_float("[vr] resolution_scale", c.resolution_scale, 0.1f, 3.0f, 1.0f);
        limit_float("[vr] world_scale", c.world_scale, 1.0f, 100000.0f, 100.0f);
        limit_int("[theatre] mode", c.theatre_mode, 0, 2);
        limit_int("[theatre] style", c.theatre_style, 0, 1);
        limit_float("[theatre] depth", c.theatre_depth, 0.0f, 3.0f, 1.0f);
        limit_float("[theatre] distance", c.theatre_distance, 0.3f, 50.0f, 2.5f);
        limit_float("[theatre] width_degrees", c.theatre_width_degrees, 10.0f, 150.0f, 70.0f);
        limit_float("[theatre] aspect", c.theatre_aspect, 0.5f, 4.0f, 1.78f);
        limit_float("[camera] chase_radius_scale", c.chase_radius_scale, 0.1f, 3.0f, 1.0f);
        limit_float("[camera] start_menu_zoom", c.start_menu_zoom, 0.0f, 5.0f, 1.5f);
        limit_float("[camera] start_menu_near_limit", c.start_menu_near_limit, 0.0f, 10.0f, 2.0f);
        limit_float("[god] raz_height_cm", c.god_raz_height_cm, 1.0f, 125.0f, 15.0f);
        limit_float("[god] distance", c.god_distance, 0.05f, 5.0f, 0.6f);
        limit_float("[god] height", c.god_height, -1.0f, 5.0f, 0.35f);
        limit_float("[god] leash", c.god_leash, 0.0f, 3.0f, 0.35f);
        limit_float("[god] leash_height", c.god_leash_height, 0.0f, 2.0f, 0.1f);
        limit_float("[god] turn_speed", c.god_turn_speed, 0.0f, 360.0f, 90.0f);
        // 0 means "leave the game's own"; otherwise a real field of view.
        if (c.cull_fov_degrees != 0.0f)
            limit_float("[vr] cull_fov_degrees", c.cull_fov_degrees, 10.0f, 179.0f, 110.0f);
        limit_float("[hud] distance", c.hud_distance, 0.1f, 100.0f, 2.0f);
        limit_float("[hud] width_degrees", c.hud_width_degrees, 2.0f, 179.0f, 48.0f);
        limit_float("[hud] lock_reticle_height", c.hud_lock_reticle_height, -2.0f, 5.0f, 0.6f);
        limit_float("[hud] lock_reticle_scale", c.hud_lock_reticle_scale, 0.1f, 10.0f, 2.0f);
        limit_int("[hud] screen_fades", c.hud_screen_fades, 0, 2);
        limit_int("[hud] screen_flashes", c.hud_screen_flashes, 0, 2);
        limit_float("[hud] menu_distance", c.hud_menu_distance, 0.1f, 100.0f, 1.4f);
        limit_float("[hud] menu_width_degrees", c.hud_menu_width_degrees, 2.0f, 179.0f, 50.0f);
        // 0 means "the game's whole screen"; otherwise a real shape.
        if (c.hud_menu_aspect != 0.0f)
            limit_float("[hud] menu_aspect", c.hud_menu_aspect, 1.0f, 4.0f, 1.333f);
        if (c.hud_aspect != 0.0f)
            limit_float("[hud] aspect", c.hud_aspect, 1.0f, 4.0f, 1.333f);
        limit_float("[overlay] vr_distance", c.overlay_vr_distance, 0.3f, 5.0f, 1.0f);
        limit_float("[overlay] vr_width_degrees", c.overlay_vr_width_degrees, 10.0f, 120.0f, 38.0f);
        limit_float("[overlay] vr_yaw_degrees", c.overlay_vr_yaw_degrees, -90.0f, 90.0f, 15.0f);
        limit_float("[overlay] vr_pitch_degrees", c.overlay_vr_pitch_degrees, -60.0f, 60.0f, -10.0f);
        limit_float("[overlay] vr_open_hold", c.overlay_vr_open_hold, 0.0f, 3.0f, 0.6f);
        limit_float("[controls] rumble", m.rumble, 0.0f, 10.0f, 1.0f);
        limit_float("[controls] god_toggle_hold", m.god_toggle_hold, 0.0f, 3.0f, 0.4f);
        limit_float("[controls] view_transition_seconds", m.view_transition_seconds, 0.0f, 10.0f, 0.3f);
        limit_float("[debug] head_yaw_test", c.debug_head_yaw, -360.0f, 360.0f, 0.0f);
        limit_float("[debug] head_wobble", c.debug_head_wobble, 0.0f, 90.0f, 0.0f);
        limit_float("[debug] head_sway_test", c.debug_head_sway, 0.0f, 50.0f, 0.0f);
        limit_float("[debug] hang_exit_seconds", c.debug_hang_exit, 0.0f, 600.0f, 0.0f);
    }
    }   // namespace

    void load_config()
    {
        // PSYVR_INI: another settings file for this process only. The test
        // harness (tools\drive.ps1) starts the game with its own ini this
        // way, so the player's psyvr.ini (and their headset runtime) is never
        // replaced and nothing has to be put back if a test is not cleaned up.
        wchar_t from_env[MAX_PATH] = {};
        const DWORD env_len = GetEnvironmentVariableW(L"PSYVR_INI", from_env, MAX_PATH);
        if (env_len > 0 && env_len < MAX_PATH)
            wcscpy_s(g_path, from_env);
        else
            swprintf_s(g_path, L"%spsyvr.ini", exe_dir());

        // First run: write the whole commented file rather than letting the
        // missing keys be added one by one without their explanations.
        if (GetFileAttributesW(g_path) == INVALID_FILE_ATTRIBUTES)
        {
            HANDLE f = CreateFileW(g_path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE)
            {
                // Notepad-friendly line endings.
                char buffer[sizeof(kDefaultIni) * 2];
                size_t n = 0;
                for (size_t i = 0; kDefaultIni[i]; ++i)
                {
                    if (kDefaultIni[i] == 0x0A)
                        buffer[n++] = 0x0D;
                    buffer[n++] = kDefaultIni[i];
                }
                DWORD written = 0;
                WriteFile(f, buffer, (DWORD)n, &written, nullptr);
                CloseHandle(f);
                g_created_default = written == n;
            }
        }
        read_all(g_config);
    }

    namespace
    {
        bool g_reload_requested = false;
    }

    void config_save_setting(const wchar_t* section, const wchar_t* key,
                             const wchar_t* value)
    {
        if (!g_path[0])
            return;
        WritePrivateProfileStringW(section, key, value, g_path);
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, g_path);  // flush
        g_reload_requested = true;
    }

    void config_request_reload()
    {
        g_reload_requested = true;
    }


    bool config_take_reload_request()
    {
        const bool r = g_reload_requested;
        g_reload_requested = false;
        return r;
    }

    void config_flush_warnings()
    {
        g_log_ready = true;
        if (g_created_default)
            log("config: psyvr.ini did not exist; wrote the commented defaults");
        for (unsigned i = 0; i < g_warning_count; ++i)
            log("config: %s", g_warnings[i]);
        g_warning_count = 0;
    }

    bool config_reload_live()
    {
        // Re-read everything into a copy, then take only what is safe to
        // change while the game runs. Device-level settings (stereo layout,
        // MSAA, window, backend, frame cap) stay as they were at start-up.
        Config n = g_config;
        read_all(n);
        Config& c = g_config;
        unsigned changed = 0;

#define LIVE_INT(field)         if (c.field != n.field) { log("config: reload %s %d -> %d", #field, (int)c.field, (int)n.field); c.field = n.field; ++changed; }
#define LIVE_FLOAT(field)         if (c.field != n.field) { log("config: reload %s %g -> %g", #field, (double)c.field, (double)n.field); c.field = n.field; ++changed; }

        LIVE_INT(release_cursor_unfocused)
        LIVE_INT(recenter_vk)
        LIVE_INT(horizon_lock)
        LIVE_INT(theatre_mode)
        LIVE_INT(theatre_style)
        LIVE_FLOAT(theatre_depth)
        LIVE_FLOAT(chase_radius_scale)
        LIVE_INT(zoom_input)
        LIVE_FLOAT(start_menu_zoom)
        LIVE_FLOAT(start_menu_near_limit)
        LIVE_INT(god_mode)
        LIVE_FLOAT(god_raz_height_cm)
        LIVE_FLOAT(god_distance)
        LIVE_FLOAT(god_height)
        LIVE_FLOAT(god_leash)
        LIVE_FLOAT(god_leash_height)
        LIVE_FLOAT(god_turn_speed)
        LIVE_INT(god_steer_camera)
        LIVE_INT(god_aim_with_head)
        if (wcscmp(c.horizon_lock_off_levels, n.horizon_lock_off_levels) != 0)
        {
            log("config: reload horizon_lock_off_levels %ls -> %ls", c.horizon_lock_off_levels,
                n.horizon_lock_off_levels);
            wcscpy_s(c.horizon_lock_off_levels, n.horizon_lock_off_levels);
            ++changed;
        }
        LIVE_FLOAT(theatre_distance)
        LIVE_FLOAT(theatre_width_degrees)
        LIVE_FLOAT(theatre_aspect)
        LIVE_INT(debug_skip_fullscreen)
        LIVE_INT(fast_fades)
        LIVE_INT(texture_pack_compat)
        LIVE_INT(loading_screens)
        LIVE_FLOAT(world_scale)
        LIVE_FLOAT(cull_fov_degrees)
        LIVE_INT(detail_culling)
        LIVE_INT(hud_enabled)
        LIVE_FLOAT(hud_distance)
        LIVE_FLOAT(hud_width_degrees)
        LIVE_FLOAT(hud_menu_distance)
        LIVE_FLOAT(hud_menu_width_degrees)
        LIVE_FLOAT(hud_menu_aspect)
        LIVE_FLOAT(hud_aspect)
        LIVE_INT(hud_world_locked)
        LIVE_INT(hud_body_locked)
        LIVE_INT(hud_lock_reticle)
        LIVE_FLOAT(hud_lock_reticle_height)
        LIVE_FLOAT(hud_lock_reticle_scale)
        LIVE_INT(hud_screen_fades)
        LIVE_INT(hud_screen_flashes)
        LIVE_FLOAT(overlay_vr_distance)
        LIVE_FLOAT(overlay_vr_width_degrees)
        LIVE_FLOAT(overlay_vr_yaw_degrees)
        LIVE_FLOAT(overlay_vr_pitch_degrees)
        LIVE_FLOAT(overlay_vr_open_hold)
        LIVE_FLOAT(debug_head_yaw)
        LIVE_FLOAT(debug_head_wobble)
        LIVE_FLOAT(debug_head_sway)
        LIVE_INT(debug_hot_reload)
        LIVE_INT(controls.enabled)
        LIVE_INT(controls.a)
        LIVE_INT(controls.b)
        LIVE_INT(controls.x)
        LIVE_INT(controls.y)
        LIVE_INT(controls.start)
        LIVE_INT(controls.back)
        LIVE_INT(controls.lb)
        LIVE_INT(controls.rb)
        LIVE_INT(controls.l3)
        LIVE_INT(controls.r3)
        LIVE_INT(controls.dpad_modifier)
        LIVE_FLOAT(controls.rumble)
        LIVE_FLOAT(controls.god_toggle_hold)
        LIVE_INT(controls.first_person_in_cycle)
        LIVE_FLOAT(controls.view_transition_seconds)
        LIVE_INT(first_person_stays)
#undef LIVE_INT
#undef LIVE_FLOAT

        log("config: reloaded psyvr.ini, %u live setting%s changed", changed, changed == 1 ? "" : "s");
        return changed != 0;
    }
}
