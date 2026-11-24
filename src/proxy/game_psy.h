#pragma once
#include <cstdint>
// Everything that reads or writes Psychonauts.exe's own memory, kept in one
// place. Addresses are from the Steam build; each hook and code patch checks
// the original bytes before writing.

namespace psyvr
{
    // True when the host is the Steam Psychonauts.exe the addresses were taken
    // from (PE TimeDateStamp 0x568C5772). Informational (log, `status`): the
    // hooks check their own bytes.
    bool game_is_supported_build();

    // In VR, before the game creates its device: makes the game's own idea of
    // the screen size (DisplaySettings.ini beside the exe: ScreenWidth,
    // ScreenHeight, FullScreen) the VR back buffer. The game sizes its scene
    // render targets from it; at its default 800x600 the world was drawn into
    // an 800x600 target and only copied to the screen. Keeps other keys; the
    // first change keeps a copy as DisplaySettings.ini.psyvr-backup. The game
    // has read the file by then, so the size it read is also replaced in its
    // renderer (memory), before the device exists: right on this launch too.
    void game_sync_display_settings(unsigned width, unsigned height);

    // Optional, with Astralathe: at startup, before any level script loads,
    // writes the PsyVR Astralathe mod's Scripts/Dart_hooks.lua
    // (ModResource\PsyVR, put there by the installer) from psyvr.ini
    // [camera]: on every level start the player's onPostBeginLevel sets the
    // camera shake and cull field of view. The mod holds both every frame
    // anyway (hold_settings_frame), so without Astralathe nothing is lost;
    // skipped (logged) without the ModResource\PsyVR folder.
    void game_write_lua_mod(bool vr);

    // Once, after CreateDevice (main thread, before the game loop): code
    // patches, each checked against the exact original bytes first.
    void game_apply_patches();

    // The game's active camera (Lua GetCameraPosition / GetCameraOrientation,
    // 0x5690F0 / 0x569330, read the same place): *(*(0x78BC20) + 0x8C98),
    // world position at +0x08, orientation quaternion at +0x20. False if the
    // memory is not readable (no game object yet).
    bool game_camera_read(float pos[3], float quat[4]);

    // Lock-on (left trigger), from Raz's entity *(*(0x78BC20) + 0x818C): the
    // byte +0x184 is set while it is held, +0x80 is the target entity (kept
    // after letting go). True while held with a target: `pos` = the target's
    // position (its feet: *(target + 0x10) + 0x40, as Raz's) and `view` = the
    // active camera's world -> view matrix (+0x50, rows, row vectors), the
    // one this frame's 3D was drawn with. Game thread.
    bool game_lock_target(float pos[3], float view[4][4]);

    // The level now loaded, as the game names it (Lua GetCurrentLevelName,
    // 0x5B4DE0: the string at *(0x78BC20) + 0x989A). Empty when unknown.
    const char* game_level_name();

    // The player's controls are paused (Lua ControlsArePaused, 0x5BA210: the
    // counter at 0x78BB14 above 0): menus over gameplay, cutscenes.
    bool game_controls_paused();

    // Per frame, from the swap chain Present.
    void game_frame();

    // Theatre ([theatre] mode, psy_theatre.h): whether it shows for the frame
    // being drawn (decided at Present, for the next frame). Mode 1 counts a
    // cutscene when the camera the game draws with, *(game + 0x8C98), is the
    // script camera (type *(int*)camera == 1, kCAMERA_SCRIPT); rail (3) and
    // sphere (5) cameras are gameplay. Any thread.
    bool game_theatre_showing();
    // How many times it has opened; a change places the window again.
    unsigned game_theatre_open_count();
    // While it shows: the eyes the stereo draws with (psy_theatre.h). False
    // when it does not show.
    struct TheatreEyes;
    bool game_theatre_eyes(TheatreEyes* out);

    // Addresses of the game's stored XInput GetState / SetState function
    // pointers, for the VR pad hook (vr_pad.cpp): the exe's import slots for
    // xinput1_3, made writable. nullptr = not the expected exe, no hook.
    void* game_xinput_get_state_slot();
    void* game_xinput_set_state_slot();
}
