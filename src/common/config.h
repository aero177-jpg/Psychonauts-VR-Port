#pragma once
// Settings from psyvr.ini, next to the game executable.
//
// Phase 1 (flat proxy) needs only the proxy's own settings. VR settings join
// as the features that read them arrive, rather than as a copy of the Legend
// mod's hundred-odd keys of which most would mean nothing here.

#include <windows.h>
#include <cstdint>

namespace psyvr
{
    // A physical control on the VR controllers, as named in psyvr.ini.
    enum ControlSource
    {
        SrcNone,
        SrcRightPrimary,    // right_a   (A on Touch / Index)
        SrcRightSecondary,  // right_b
        SrcLeftPrimary,     // left_x
        SrcLeftSecondary,   // left_y
        SrcLeftMenu,        // left_menu
        SrcRightMenu,       // right_menu (WMR / Index; Touch reserves it)
        SrcLeftStick,       // left_stick  (click)
        SrcRightStick,      // right_stick (click)
        SrcLeftGrip,        // left_grip
        SrcRightGrip,       // right_grip
        SrcLeftTrigger,     // left_trigger  (as a button)
        SrcRightTrigger,    // right_trigger (as a button)
    };

    // [controls]: which control presses each button of the virtual Xbox
    // pad. The sticks and triggers are always the sticks and triggers.
    struct ControlMap
    {
        bool enabled = true;
        ControlSource a = SrcRightPrimary;
        ControlSource b = SrcRightSecondary;
        ControlSource x = SrcLeftPrimary;
        ControlSource y = SrcLeftSecondary;
        ControlSource start = SrcLeftMenu;
        ControlSource back = SrcRightMenu;
        ControlSource lb = SrcLeftGrip;
        ControlSource rb = SrcRightGrip;
        ControlSource l3 = SrcLeftStick;
        ControlSource r3 = SrcRightStick;
        // Touch has no D-pad or Back: a click of this arms the left stick as
        // the D-pad (dpad_gesture.h), and while armed `start` presses Back.
        ControlSource dpad_modifier = SrcLeftStick;
        float rumble = 1.0f;            // 0 = no vibration
        // The control that presses R3 (the right stick click), as in the
        // Underworld mod (vr_pad.h): held this many seconds it switches the
        // view (classic -> god -> first person -> classic); a quicker tap is
        // a short R3. 0 = no switching.
        float god_toggle_hold = 0.4f;
        // [controls] first_person_in_cycle (live): the first-person look is
        // one of the views the hold switches through.
        bool first_person_in_cycle = true;
        // [controls] view_transition_seconds (live): seconds the eyes glide
        // from one view to the next (god_frame.cpp). 0 = a cut.
        float view_transition_seconds = 0.3f;
    };

    struct Config
    {
        // [proxy]
        // auto = d3d9_dxvk.dll beside the game if present, else the system
        // d3d9; dxvk; system; or a full path to any d3d9.dll.
        wchar_t backend[MAX_PATH] = L"auto";
        bool logging = true;

        // [window]
        // The game only offers exclusive fullscreen, which a VR compositor
        // and alt-tab both fight. A window is forced instead.
        bool force_windowed = true;
        int window_width = 0;          // 0 = two thirds of the monitor
        int window_height = 0;
        bool window_borderless = false;
        bool release_cursor_unfocused = true;
        // [window] pause_when_unfocused: the game's own pause when its window
        // is not in front. Off by default: the VR runtime's window takes focus.
        bool pause_when_unfocused = false;

        // [graphics]
        // Highest MSAA sample count the game may use (it asks for 8x).
        // 0 or 1 turns multisampling off.
        int msaa = 2;   // 2026-10-21: the game asks for 4x; at a Quest 3's 5376x2304 that costs too much
        // Frame-rate cap applied inside DXVK: 0 = unlocked, N = N fps,
        // -N = N fps with vsync-style pacing as DXVK defines it. DXVK's own
        // profile may cap the rate; this overrides it. Some reports
        // say the game is unstable at very high frame rates, hence a setting.
        int max_frame_rate = 0;
        // [graphics] fast_fades (live): things fading in or out are drawn
        // only where they are, not through two passes over the whole frame
        // each (stereo.cpp, "Fading objects"). The same picture; 0 = the
        // game's own way, for comparison.
        bool fast_fades = true;
        // [graphics] texture_pack_compat (live): allow for texture packs that
        // replace the game's textures with larger ones (Shibanauts HD), where
        // the mod recognises a draw by its texture's size. Each such rule says
        // so where it is (search for texture_pack_compat). Off = the game's
        // own sizes only, in case a rule picks up the wrong texture.
        bool texture_pack_compat = true;
        // Present interval: -1 = whatever the game asks for (its own vsync
        // setting), 0 = off (immediate), 1 = on.
        int vsync = -1;
        // Video memory DXVK reports to the game, in MB (0 = DXVK's own 4096).
        // The game sizes its texture cache from it, and DXVK keeps a
        // system-memory copy of managed textures inside the 32-bit game's
        // 2 GB address space, so less here means less address space used.
        int video_memory_mb = 0;

        // [vr]
        // Off = the proxy is a plain windowed-mode shim.
        bool vr_enabled = true;
        // A specific runtime manifest for this game only (sets
        // XR_RUNTIME_JSON for the process); empty = the registered runtime.
        wchar_t xr_runtime_json[MAX_PATH] = L"";
        // Each eye's render size as a fraction of what the runtime asks for
        // (Virtual Desktop can ask for 3072x2612 an eye). Lower = faster and
        // less memory; the runtime scales the image up.
        float resolution_scale = 1.0f;
        // Game units per metre (Raz is 125 units tall: 100 = life size).
        float world_scale = 100.0f;    // Psychonauts units look like centimetres (camera target height 110)
        // The vertical field of view the game culls with, in degrees (its
        // own is 45, far narrower than a headset). 0 = leave it alone.
        float cull_fov_degrees = 99.0f;
        // [vr] detail_culling (live): the game's DetailCulling flag (game +
        // 0xA7), held every frame in VR. Off = far small things never fade out.
        bool detail_culling = false;
        // [vr] loading_screens (live): the game's loading screens are drawn
        // and presented by a loader thread (docs/findings.md, "Present moves
        // between threads"). 0 = the headset keeps its last frame meanwhile;
        // 1 = they go to the headset too; 2 = auto: 1, except on the OpenXR
        // Simulator, whose window makes the loader thread's submit deadlock.
        int loading_screens = 2;
        // [vr] horizon_lock: the eyes keep the game camera's position and yaw;
        // its pitch and roll are dropped (the head supplies them). In the ini
        // "off", "auto" or "on" (or 0, 1, 2; the old 1 = on reads as auto).
        // Auto = on except in horizon_lock_off_levels; on = everywhere.
        enum HorizonLock { HorizonOff = 0, HorizonAuto = 1, HorizonOn = 2 };
        int horizon_lock = HorizonAuto;
        // [vr] horizon_lock_off_levels (live): levels where horizon lock
        // "auto" is off, by name or the start of one (the game's names: STMU = the start
        // menu, MMI1 / MMI2 / MMDM = the Milkman Conspiracy), separated by
        // spaces or commas.
        wchar_t horizon_lock_off_levels[128] = L"STMU MM";
        // [theatre] (live): the game as in the flat game, on a screen in a dark
        // room, in stereo (psy_theatre.h). mode: 0 = off, 1 = cutscenes,
        // 2 = always (horizon lock and god mode then show the game's own camera).
        int theatre_mode = 1;
        int theatre_style = 0;              // 0 = screen, 1 = window (psy_theatre.h TheatreStyle)
        float theatre_depth = 1.0f;         // the eyes' separation in the game, x this
        float theatre_distance = 1.93f;     // metres
        float theatre_width_degrees = 57.0f;
        float theatre_aspect = 1.333f;      // width / height
        // Keyboard key that recentres the view (desktop testing; the
        // controllers' both-stick-click works too). A virtual-key code,
        // read from a name: F1..F24, A..Z, 0..9 or none.
        int recenter_vk = 0x77;             // F8

        // [camera] (held every frame, game_psy.cpp; also set at level start through
        // the optional PsyVR Astralathe mod)
        float chase_radius_scale = 0.756f;   // the chase camera's distance from Raz, x this
        bool camera_shake = false;         // the game's camera shakes
        // [camera] first_person_stays (live): the first-person look is not
        // left when Raz moves (the jump at 0x4DBDFB, game_psy.cpp).
        bool first_person_stays = true;
        bool zoom_input = true;            // D-pad up/down and the VR right stick zoom live
        // [camera] start_menu_zoom (live): in the start menu (level STMU) the
        // view is moved this many metres forward, nearer the menu's 3D scene.
        float start_menu_zoom = 0.0f;
        // [camera] start_menu_near_limit (live): in the start menu, metres
        // (as seen) no 3D draw comes nearer than: its eyes are moved back
        // until its centre is this far away (the floating layers between
        // the game camera and the brain). 0 = off.
        float start_menu_near_limit = 2.0f;

        // [god] (live): god mode. The world is shown small, Raz a few
        // centimetres tall, seen from a seat above and behind him on a
        // leash. "Table metres" are metres at the shown (small) scale.
        bool god_mode = false;
        float god_raz_height_cm = 15.0f;    // how tall Raz looks
        float god_distance = 0.6f;          // table metres back from the centre
        float god_height = 0.35f;           // table metres up
        float god_leash = 0.35f;            // table metres Raz walks from the centre before it follows; 0 = always on him
        float god_leash_height = 0.1f;      // the same, up and down
        float god_turn_speed = 90.0f;       // degrees/second at full right stick, turning the view round Raz
        bool god_steer_camera = true;       // the game's chase camera is moved onto the seat's line, so it culls from there
        bool god_aim_with_head = true;      // ...and aimed where the head looks (with the head's offset), so it culls for that

        // [hud]
        // The game's 2D (HUD, menus) on a screen floating in front of the
        // head. Off = drawn into each eye as the game laid it out.
        bool hud_enabled = true;
        float hud_distance = 2.13f;         // metres
        float hud_width_degrees = 71.0f;    // how wide it looks (in-game HUD, visible part)
        // The screen that shows menus and loading screens (2D with no 3D
        // scene behind it).
        float hud_menu_distance = 4.64f;         // metres, 0.1..100
        float hud_menu_width_degrees = 67.0f;   // visible part, 2..179
        float hud_menu_aspect = 1.97f;        // 0 = whole screen, or 1..4
        // [hud] aspect: the gameplay HUD's visible part, likewise. Psychonauts
        // lays its 2D out 640x480 (4:3) centred in the wider screen.
        float hud_aspect = 1.65f;
        // Menus and loading screens are placed in the room in front of you
        // when they appear (and at a recentre) and left there; off = fixed
        // to the head.
        bool hud_world_locked = true;
        // [hud] body_locked: the gameplay HUD keeps its place as you turn
        // your head (it stays straight ahead of the room's forward, which a
        // recentre sets) but moves with the head's position. Off = fixed to
        // the head (the aiming reticle then always marks where you look).
        bool hud_body_locked = true;
        // [hud] lock_reticle: while locked on, the game's lock-on reticle is
        // drawn on the target in 3D (both eyes) instead of on the HUD screen.
        // lock_reticle_height: metres above the target's feet (its position).
        bool hud_lock_reticle = true;
        float hud_lock_reticle_height = 0.6f;
        // [hud] lock_reticle_scale: its size against the HUD's (1 = as big
        // as it would look on the HUD screen).
        float hud_lock_reticle_scale = 2.0f;
        // [hud] screen_fades / screen_flashes: the game's full-screen tints
        // (stereo.cpp: fades to black between areas; colour flashes such as
        // the ground pound's). 0 = on the HUD screen, as the game drew them;
        // 1 = over the whole view, in both eyes; 2 = left out.
        int hud_screen_fades = 1;
        int hud_screen_flashes = 2;

        // [overlay] The settings overlay (Insert) as a panel in the headset:
        // placed in the room in front of you when it opens (and at a
        // recentre), this far away and this wide, this many degrees left of
        // and above straight ahead. Holding the pause button opens it.
        float overlay_vr_distance = 1.0f;       // metres, 0.3..5
        float overlay_vr_width_degrees = 38.0f; // 10..120
        float overlay_vr_yaw_degrees = 15.0f;   // -90..90, positive = left
        float overlay_vr_pitch_degrees = -10.0f; // -60..60, positive = up
        float overlay_vr_open_hold = 0.6f;      // seconds Start is held, 0 = off, 0..3


        // [controls]
        ControlMap controls;

        // [debug]
        // A fixed extra head yaw in degrees, for testing the head path with
        // a runtime whose head never moves (the simulator).
        float debug_head_yaw = 0.0f;
        // On top of it, a yaw swinging this many degrees each way once a
        // second: a moving head for the simulator.
        float debug_head_wobble = 0.0f;
        // On top of both, a sideways sway of the head's position, in cm, going
        // back and forth once every two seconds: a leaning head for the simulator.
        float debug_head_sway = 0.0f;
        // [debug] hang_exit_seconds: end the process when a frame has been
        // stuck this long (0 = never). For unattended test runs.
        float debug_hang_exit = 0.0f;
        // [debug] skip_fullscreen_passes (live): leave out the full-screen
        // Sprite.vsh passes (and the scene-copy blends drawn the same way), to
        // measure what they cost and see what they draw.
        bool debug_skip_fullscreen = false;
        // psyvr_logic.dll is swapped for a rebuilt one while the game runs
        // (logic_host.h). Read by the shim every 30 frames.
        bool debug_hot_reload = false;
        // A named pipe ("psyvr" in the pipe namespace) takes test commands while
        // the game runs (control.h). Start-up only.
        bool debug_control_pipe = false;
    };

    const Config& config();

    // Reads psyvr.ini, writing any missing keys back with their defaults so
    // the file documents itself.
    void load_config();

    // Re-reads psyvr.ini and applies only the settings that are safe to
    // change while the game runs (view, HUD, controls, debug); returns true
    // if any changed. Start-up-only ones keep their values.
    bool config_reload_live();

    // The settings overlay's way in: writes one value to psyvr.ini (the file
    // may be ANSI or UTF-16; the profile API copes with both), and asks for
    // config_reload_live() to run at the next frame boundary on the game
    // thread. The Config itself is never written from the overlay.
    void config_save_setting(const wchar_t* section, const wchar_t* key,
                             const wchar_t* value);
    void config_request_reload();

    // True once per request; the frame boundary calls it.
    bool config_take_reload_request();

    // After the log is open: writes what load_config noticed (a first-run
    // file, values that were out of range and were clamped).
    void config_flush_warnings();

    // How many keys load_config had to add, for the log.
    unsigned config_defaults_added();
}
