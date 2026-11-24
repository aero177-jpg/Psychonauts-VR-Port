// Offline checks of the settings loader: the commented file written on first
// run, the defaults it reads back, clamping of bad values, and the live
// reload keeping start-up-only settings. Works in the folder of the test
// executable (psyvr.ini there), never in the game's.

#include "../common/config.h"
#include "../common/log.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

using namespace psyvr;

namespace
{
    int g_checks = 0, g_failed = 0;

    void check(bool ok, const char* what, int line)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failed;
            std::printf("FAIL  %s  (configtest.cpp:%d)\n", what, line);
        }
    }
#define CHECK(c) check((c), #c, __LINE__)

    std::wstring ini_path()
    {
        return std::wstring(exe_dir()) + L"psyvr.ini";
    }

    void write_ini(const char* text)
    {
        FILE* f = nullptr;
        _wfopen_s(&f, ini_path().c_str(), L"wb");
        if (f)
        {
            std::fwrite(text, 1, std::strlen(text), f);
            std::fclose(f);
        }
    }

    std::string read_ini()
    {
        std::string out;
        FILE* f = nullptr;
        _wfopen_s(&f, ini_path().c_str(), L"rb");
        if (!f)
            return out;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            out.append(buf, n);
        std::fclose(f);
        return out;
    }
}

int main()
{
    DeleteFileW(ini_path().c_str());

    // --- first run: a commented file, and the defaults -------------------
    load_config();
    const std::string created = read_ini();
    CHECK(!created.empty());
    CHECK(created.rfind("; psyvr.ini", 0) == 0);                 // starts with the header
    CHECK(created.find("\r\n") != std::string::npos);            // Notepad line endings
    CHECK(created.find("cull_fov_degrees = 99") != std::string::npos);
    CHECK(created.find("; live: game units per metre") != std::string::npos);


    // --- bad values are clamped, not trusted -----------------------------
    write_ini(
        "[window]\r\nwidth=-5\r\nheight=999999\r\n"
        "[graphics]\r\nmsaa=64\r\nmax_frame_rate=100000\r\nvsync=7\r\n"
        "[vr]\r\nworld_scale=0\r\ncull_fov_degrees=3\r\nrecenter_key=banana\r\n"
        "[hud]\r\ndistance=500\r\nwidth_degrees=0.5\r\n"
        "[controls]\r\na=nonsense\r\nrumble=99\r\ngod_toggle_hold=9\r\n"
        "[debug]\r\nhead_yaw_test=9999\r\n");
    load_config();
    CHECK(config().window_width == 0);
    CHECK(config().window_height == 16384);
    CHECK(config().msaa == 16);
    CHECK(config().max_frame_rate == 1000);
    CHECK(config().vsync == 1);
    CHECK(config().world_scale == 1.0f);
    CHECK(config().cull_fov_degrees == 10.0f);
    CHECK(config().recenter_vk == 0x77);                         // unknown name: the default
    CHECK(config().hud_distance == 100.0f);
    CHECK(config().hud_width_degrees == 2.0f);
    CHECK(config().controls.a == SrcRightPrimary);               // unknown source: the default
    CHECK(config().controls.rumble == 10.0f);
    CHECK(config().controls.god_toggle_hold == 3.0f);
    CHECK(config().debug_head_yaw == 360.0f);






    // [vr] horizon_lock: off / auto / on, numbers too, old 1 = auto, live
    write_ini("[vr]\r\n");
    load_config();
    CHECK(config().horizon_lock == Config::HorizonAuto);
    CHECK(read_ini().find("horizon_lock=auto") != std::string::npos);
    write_ini("[vr]\r\nhorizon_lock=1\r\n");
    load_config();
    CHECK(config().horizon_lock == Config::HorizonAuto);
    write_ini("[vr]\r\nhorizon_lock=0\r\n");
    load_config();
    CHECK(config().horizon_lock == Config::HorizonOff);
    write_ini("[vr]\r\nhorizon_lock=ON\r\n");
    load_config();
    CHECK(config().horizon_lock == Config::HorizonOn);
    write_ini("[vr]\r\nhorizon_lock=sometimes\r\n");
    load_config();
    CHECK(config().horizon_lock == Config::HorizonAuto);
    write_ini("[vr]\r\nhorizon_lock=off\r\n");
    CHECK(config_reload_live());
    CHECK(config().horizon_lock == Config::HorizonOff);

    // [debug] head_wobble: default off, clamped
    write_ini("[debug]\r\n");
    load_config();
    CHECK(config().debug_head_wobble == 0.0f);
    write_ini("[debug]\r\nhead_wobble=500\r\n");
    load_config();
    CHECK(config().debug_head_wobble == 90.0f);

    // [debug] head_sway_test: default off, clamped 0..50, live
    write_ini("[debug]\r\n");
    load_config();
    CHECK(config().debug_head_sway == 0.0f);
    write_ini("[debug]\r\nhead_sway_test=500\r\n");
    load_config();
    CHECK(config().debug_head_sway == 50.0f);
    write_ini("[debug]\r\nhead_sway_test=-3\r\n");
    load_config();
    CHECK(config().debug_head_sway == 0.0f);
    write_ini("[debug]\r\nhead_sway_test=12.5\r\n");
    CHECK(config_reload_live());                                  // live
    CHECK(config().debug_head_sway == 12.5f);




    // [debug] hot_reload: default off, live
    write_ini("[debug]\r\n");
    load_config();
    CHECK(!config().debug_hot_reload);
    write_ini("[debug]\r\nhot_reload=1\r\n");
    CHECK(config_reload_live());
    CHECK(config().debug_hot_reload);

    // [debug] control_pipe: default off
    write_ini("[debug]\r\n");
    load_config();
    CHECK(!config().debug_control_pipe);
    write_ini("[debug]\r\ncontrol_pipe=1\r\n");
    load_config();
    CHECK(config().debug_control_pipe);


    // cull_fov_degrees = 0 is meaningful (leave the game's own), not an error
    write_ini("[vr]\r\ncull_fov_degrees=0\r\n");
    load_config();
    CHECK(config().cull_fov_degrees == 0.0f);

    // [vr] detail_culling: off by default (everything drawn), 1 = the game's own
    write_ini("[vr]\r\n");
    load_config();
    CHECK(!config().detail_culling);
    write_ini("[vr]\r\ndetail_culling=1\r\n");
    load_config();
    CHECK(config().detail_culling);
    // [graphics] fast_fades: on by default, 0 turns it off.
    CHECK(config().fast_fades);
    write_ini("[graphics]\r\nfast_fades=0\r\n");
    load_config();
    CHECK(!config().fast_fades);
    // [graphics] texture_pack_compat: on by default, 0 turns it off.
    write_ini("[graphics]\r\n");
    load_config();
    CHECK(config().texture_pack_compat);
    write_ini("[graphics]\r\ntexture_pack_compat=0\r\n");
    load_config();
    CHECK(!config().texture_pack_compat);
    // [vr] loading_screens: 2 (auto) by default, kept to 0..2.
    write_ini("[vr]\r\n");
    load_config();
    CHECK(config().loading_screens == 2);
    write_ini("[vr]\r\nloading_screens=5\r\n");
    load_config();
    CHECK(config().loading_screens == 2);
    write_ini("[vr]\r\nloading_screens=0\r\n");
    load_config();
    CHECK(config().loading_screens == 0);

    // [hud] menu screen: defaults, clamps, menu_aspect = 0 (whole screen) kept
    write_ini("[hud]\r\n");
    load_config();
    CHECK(config().hud_menu_distance == 4.64f);
    CHECK(config().hud_menu_width_degrees == 67.0f);
    CHECK(config().hud_menu_aspect == 1.97f);
    CHECK(config().hud_aspect == 1.65f);                         // gameplay HUD: the game's 4:3 layout
    write_ini("[hud]\r\nmenu_distance=500\r\nmenu_width_degrees=0.5\r\nmenu_aspect=99\r\n");
    load_config();
    CHECK(config().hud_menu_distance == 100.0f);
    CHECK(config().hud_menu_width_degrees == 2.0f);
    CHECK(config().hud_menu_aspect == 4.0f);
    write_ini("[hud]\r\nmenu_distance=0\r\nmenu_width_degrees=500\r\nmenu_aspect=0.5\r\n");
    load_config();
    CHECK(config().hud_menu_distance == 0.1f);
    CHECK(config().hud_menu_width_degrees == 179.0f);
    CHECK(config().hud_menu_aspect == 1.0f);                      // nonzero below the floor: clamped up
    write_ini("[hud]\r\nmenu_aspect=0\r\n");
    load_config();
    CHECK(config().hud_menu_aspect == 0.0f);                      // 0 is meaningful, not an error
    write_ini("[hud]\r\nmenu_distance=3\r\nmenu_width_degrees=60\r\nmenu_aspect=2\r\n");
    CHECK(config_reload_live());                                  // all three are live
    CHECK(config().hud_menu_distance == 3.0f);
    CHECK(config().hud_menu_width_degrees == 60.0f);
    CHECK(config().hud_menu_aspect == 2.0f);

    // [hud] body_locked: on by default, live
    write_ini("[hud]\r\n");
    load_config();
    CHECK(config().hud_body_locked);
    write_ini("[hud]\r\nbody_locked=0\r\n");
    CHECK(config_reload_live());
    CHECK(!config().hud_body_locked);

    // [hud] lock_reticle / lock_reticle_height: defaults, clamp, live
    write_ini("[hud]\r\n");
    load_config();
    CHECK(config().hud_lock_reticle);
    CHECK(config().hud_lock_reticle_height == 0.6f);
    CHECK(config().hud_lock_reticle_scale == 2.0f);
    write_ini("[hud]\r\nlock_reticle_height=9\r\nlock_reticle_scale=50\r\n");
    load_config();
    CHECK(config().hud_lock_reticle_height == 5.0f);
    CHECK(config().hud_lock_reticle_scale == 10.0f);
    write_ini("[hud]\r\nlock_reticle=0\r\nlock_reticle_height=1.2\r\nlock_reticle_scale=3\r\n");
    CHECK(config_reload_live());
    CHECK(!config().hud_lock_reticle);
    CHECK(config().hud_lock_reticle_height == 1.2f);
    CHECK(config().hud_lock_reticle_scale == 3.0f);

    // [hud] screen_fades / screen_flashes: defaults, clamp, live
    write_ini("[hud]\r\n");
    load_config();
    CHECK(config().hud_screen_fades == 1);
    CHECK(config().hud_screen_flashes == 2);
    write_ini("[hud]\r\nscreen_fades=7\r\nscreen_flashes=-1\r\n");
    load_config();
    CHECK(config().hud_screen_fades == 2);
    CHECK(config().hud_screen_flashes == 0);
    write_ini("[hud]\r\nscreen_fades=0\r\nscreen_flashes=1\r\n");
    CHECK(config_reload_live());
    CHECK(config().hud_screen_fades == 0);
    CHECK(config().hud_screen_flashes == 1);

    // [overlay] the settings panel in the headset: defaults, clamps, live
    write_ini("[overlay]\r\n");
    load_config();
    CHECK(config().overlay_vr_distance == 1.0f);
    CHECK(config().overlay_vr_width_degrees == 38.0f);
    CHECK(config().overlay_vr_yaw_degrees == 15.0f);
    CHECK(config().overlay_vr_pitch_degrees == -10.0f);
    CHECK(config().overlay_vr_open_hold == 0.6f);
    write_ini("[overlay]\r\nvr_distance=50\r\nvr_width_degrees=500\r\nvr_yaw_degrees=500\r\nvr_pitch_degrees=500\r\nvr_open_hold=50\r\n");
    load_config();
    CHECK(config().overlay_vr_distance == 5.0f);
    CHECK(config().overlay_vr_width_degrees == 120.0f);
    CHECK(config().overlay_vr_yaw_degrees == 90.0f);
    CHECK(config().overlay_vr_pitch_degrees == 60.0f);
    CHECK(config().overlay_vr_open_hold == 3.0f);
    write_ini("[overlay]\r\nvr_distance=0\r\nvr_width_degrees=1\r\nvr_yaw_degrees=-500\r\nvr_pitch_degrees=-500\r\nvr_open_hold=-1\r\n");
    load_config();
    CHECK(config().overlay_vr_distance == 0.3f);
    CHECK(config().overlay_vr_width_degrees == 10.0f);
    CHECK(config().overlay_vr_yaw_degrees == -90.0f);
    CHECK(config().overlay_vr_pitch_degrees == -60.0f);
    CHECK(config().overlay_vr_open_hold == 0.0f);
    write_ini("[overlay]\r\nvr_distance=2.5\r\nvr_width_degrees=70\r\nvr_yaw_degrees=-30\r\nvr_pitch_degrees=15\r\nvr_open_hold=1.5\r\n");
    CHECK(config_reload_live());                                  // all are live
    CHECK(config().overlay_vr_distance == 2.5f);
    CHECK(config().overlay_vr_width_degrees == 70.0f);
    CHECK(config().overlay_vr_yaw_degrees == -30.0f);
    CHECK(config().overlay_vr_pitch_degrees == 15.0f);
    CHECK(config().overlay_vr_open_hold == 1.5f);

    // --- live reload keeps start-up-only settings ------------------------
    write_ini("[graphics]\r\nmsaa=4\r\n[vr]\r\nworld_scale=100\r\ncull_fov_degrees=110\r\n");
    load_config();
    CHECK(config().msaa == 4);
    write_ini("[graphics]\r\nmsaa=2\r\n[vr]\r\nworld_scale=50\r\ncull_fov_degrees=110\r\n");
    CHECK(config_reload_live());
    CHECK(config().world_scale == 50.0f);                        // live: applied
    CHECK(config().msaa == 4);                                   // start-up only: kept

    DeleteFileW(ini_path().c_str());
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
