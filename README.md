# Psychonauts VR mod

A VR mod for the Steam version of Psychonauts (2005). It adds side-by-side stereo rendering,
6DoF head tracking, a floating screen for the HUD and menus, and the VR controllers as the
game's Xbox pad.

**Status:** early release (0.1.0-preview). It has been played on a Quest 3 over Virtual Desktop
and run in the OpenXR-Simulator. Expect rough edges; settings and behaviour will keep changing.
Steam build of `Psychonauts.exe` only (PE `TimeDateStamp 0x568C5772`); see [Caveats](#caveats).

Features: stereo with the head pose written into the game's own camera (so culling follows your
head); a level horizon with head look; HUD and menus on a floating screen, with the lock-on
reticle placed on its target in 3D; three views on the right stick click (classic, a tabletop
"god mode" and first person); cutscenes on a floating screen (theatre mode); a sharper water
reflection and glow at VR resolution; loading screens on the floating screen; an in-game settings
overlay (Insert, also shown in the headset) with a button guide for the Quest 3 controllers. The
full list of settings is in [`release/psyvr.sample.ini`](release/psyvr.sample.ini).

## Quick start (players)

Download the zip from the [Releases](../../releases) page, not this repository's source. You need
Psychonauts from Steam, a Vulkan-capable GPU, and a Quest 3 with Virtual Desktop (its 32-bit OpenXR
runtime is found automatically).

1. Extract the whole zip (do not run the installer from inside it).
2. Run `Install.bat`. It finds the Steam copy of the game and copies the mod next to
   `Psychonauts.exe`; `Uninstall.bat` removes it again and restores the files it replaced.
3. Connect the headset with Virtual Desktop, then start the game from Steam (or with
   `Play Psychonauts VR.bat`).

The zip's `READ ME FIRST.txt` ([`release/READ ME FIRST.txt`](release/READ%20ME%20FIRST.txt)) covers
the install in full, controls, settings and troubleshooting. HD texture packs: see
[HD texture packs](#hd-texture-packs-optional).

## How it works

A `d3d9.dll` proxy sits beside `Psychonauts.exe` (the game loads `d3d9.dll` and finds ours first)
and forwards to a DXVK fork (`d3d9_dxvk.dll`, D3D9 to Vulkan), whose Vulkan device an OpenXR
session is created on. The proxy wraps `IDirect3D9`, the device and the swap chain, so every draw
passes through it.

The back buffer is made twice as wide, one half per eye, and the game's resolution
(`DisplaySettings.ini`) is set to match, so it renders its scene targets at headset size. Each
3D draw is classified from what its vertex shader reads and is drawn once per eye with the eye's
projection folded into the game's own WVP (recovered from the constants it uploads; see
[`docs/shaders.md`](docs/shaders.md)). 2D draws go to the floating HUD screen. A small set of
signature-checked patches in the game's code hold the camera level, widen its culling, move the
chase camera for god mode and first person, and read the lock-on target.

The draw logic lives in a second DLL, `psyvr_logic.dll`, that can be rebuilt and swapped while
the game runs (see [`tools/README.md`](tools/README.md), "Hot reload").
[`docs/findings.md`](docs/findings.md) is the reverse-engineering log: what was found in the game,
dated, with the reasoning behind each design decision.

## Layout

- `src/proxy/`: the `d3d9.dll` proxy. D3D9 wrappers, backend loader, window fixes,
  `game_psy.cpp` (everything that touches `Psychonauts.exe` memory, with the addresses),
  `hook.cpp` (signature-checked inline hook), `logic_host` (loads and hot-reloads the logic DLL),
  `overlay` (settings overlay), and the diagnostics `trace`, `drawlog` and `control`.
- `src/logic/`: the logic DLL's entry point (`psyvr_logic.dll`); with `src/vr/stereo.cpp` and
  `god_frame.cpp` it holds the draw classification and god mode's per-frame camera logic.
- `src/vr/`: OpenXR runtime layer, head and eye poses, frame submission, stereo, HUD screen,
  controllers-as-gamepad, theatre, god mode. Pure maths lives in modules that say so in their
  header (no config, no game memory, no Windows calls) so the tests can link them.
- `src/vr/shaders/`: the replacement water pixel shaders (HLSL and their compiled headers).
- `src/common/`: logging (`psyvr.log`, crash minidump `psyvr_crash.dmp`), settings (`psyvr.ini`),
  the logic-DLL interface and the generated `default_ini.h`.
- `src/test/`: unit tests (`mathtest`, `configtest`) and `xrprobe`.
- `release/`: what ships in the package: `READ ME FIRST.txt`, `psyvr.sample.ini`, the installer
  and the Play scripts.
- `mod/PsyVR/`: the optional Astralathe part (see [Caveats](#caveats)).
- `dxvk-psyvr/`: the DXVK fork with its OpenXR interop (vendored source).
- `third_party/`: `openxr` (headers and the static 32-bit loader), `imgui` (Dear ImGui, for the
  overlay), `vulkan` (headers).
- `docs/`: [`findings.md`](docs/findings.md) (reverse engineering and decisions),
  [`shaders.md`](docs/shaders.md) (the game's shader register layout),
  [`SAVES.md`](docs/SAVES.md) (test saves for the harness).
- `tools/`: test driver, Ghidra scripts, generators. See [`tools/README.md`](tools/README.md).

## Build and install

Requirements: Windows, Visual Studio (or the Build Tools) with the C++ x86 tools (found through
`msvc.bat`), Python 3 for the generators. Building the DXVK fork also needs meson, ninja
(`py -3 -m pip install --user meson ninja`) and glslang (a
[KhronosGroup/glslang](https://github.com/KhronosGroup/glslang/releases) release in
`%USERPROFILE%\tools\glslang`, or the Vulkan SDK on PATH); `tools\dxvk_path.bat` looks for them in
the usual places. Run the scripts by full path (`cmd /c "C:\...\build.bat"`).

- `build.bat`: builds `dist\d3d9.dll` (32-bit, static CRT) and `dist\psyvr_logic.dll`.
  `build.bat logic` builds only the latter.
- `build_dxvk.bat`: builds `dist\d3d9_dxvk.dll` from `dxvk-psyvr` (several minutes the first time).
- `install.bat [game folder]` / `uninstall.bat [game folder]`: a developer install from `dist\`;
  the default is Steam's install location. A foreign `d3d9.dll` is moved aside to
  `d3d9.dll.psyvr-backup`, not overwritten, and put back on uninstall, as is the player's
  `DisplaySettings.ini`.
- `package.bat [folder] [zip]`: assembles the drag-and-drop release (installer, `READ ME FIRST.txt`,
  `psyvr.sample.ini`, the three DLLs, the Play scripts, licences) into
  `%USERPROFILE%\Desktop\PsyVRmod`; it refuses to write into a folder that is not its own.
- `test.bat`: offline checks, no game needed: the maths (projection recovery, pose mirroring,
  recentring, the god-mode seat, theatre geometry, shader scanning, the D-pad gesture), the settings
  loader (first-run file, clamping, live reload) and that the embedded default ini matches
  `release\psyvr.sample.ini`. Exits non-zero on failure. After editing the sample ini run
  `py -3 -I tools\gen_default_ini.py`.
- `xrprobe.bat [runtime.json]`: builds and runs a probe that reports what the 32-bit OpenXR
  runtime offers.

A 32-bit OpenXR runtime is needed (`Psychonauts.exe` is 32-bit). Virtual Desktop's
(`virtualdesktop-openxr-32.json`) is found automatically; otherwise set `[vr] xr_runtime_json`.

## Settings: `psyvr.ini`

`psyvr.ini` is created beside the game on first run with every key and a comment for each; it is
the same text as [`release/psyvr.sample.ini`](release/psyvr.sample.ini), which is the full
reference. Values outside their sensible range are clamped, and each is noted in `psyvr.log`.
Keys marked *live* in that file apply without a restart: edit the file, then create an empty
file named `reload.flag` beside the game. The in-game overlay (Insert) edits the same settings.

A short selection (defaults from the sample ini):

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[proxy]` | `backend` | `auto` | `auto` (`d3d9_dxvk.dll` if present, else system d3d9), `dxvk`, `system`, or a full path. |
| `[graphics]` | `msaa` | `2` | Highest MSAA sample count offered to the game (it asks for 4). |
| | `max_frame_rate` | `0` | `0` = unlocked, `N` = cap at N fps inside DXVK. |
| | `fast_fades` | `1` | *live* Things fading in or out are drawn only where they are (the game's way costs two passes over the whole frame each); same picture. |
| | `texture_pack_compat` | `1` | *live* Allow for texture packs whose replacement textures are larger (see below). |
| `[vr]` | `enabled` | `1` | `0` = a plain windowed-mode proxy. |
| | `loading_screens` | `2` | *live* Loading screens on the floating screen: `1` always, `0` never, `2` auto (off in the OpenXR Simulator, which would hang). |
| | `xr_runtime_json` | | A specific OpenXR runtime manifest for this game only. |
| | `resolution_scale` | `1` | Each eye's render size as a fraction of what the runtime asks for. |
| | `horizon_lock` | `auto` | *live* `off`, `auto` (off in `horizon_lock_off_levels`) or `on`. |
| | `world_scale` | `100` | *live* Game units per metre (Raz is 125 units tall). |
| | `cull_fov_degrees` | `99` | *live* Field of view the game culls with. |
| | `recenter_key` | `F8` | *live* Keyboard recentre. Both stick clicks always recentre. |
| `[camera]` | `chase_radius_scale` | `0.756` | *live* The chase camera's distance from Raz, times this. |
| `[god]` | `raz_height_cm` | `15` | *live* How tall Raz looks in god mode. |
| `[theatre]` | `mode` | `1` | *live* `0` off, `1` cutscenes on a floating screen, `2` always. |
| `[hud]` | `distance`, `width_degrees` | `2.13`, `71` | *live* Where the gameplay HUD floats. |
| | `lock_reticle` | `1` | *live* The lock-on reticle on its target in 3D. |
| `[controls]` | `a b x y lb rb ...` | | *live* Which VR control presses each pad button. |
| | `god_toggle_hold` | `0.4` | *live* Seconds the right stick click is held to switch views. |

The overlay groups what a frame costs under **Performance** (resolution scale, MSAA, fast fades,
skipping the game's full-screen effects, detail culling, texture pack compatibility) and has a
**Controls (button guide)** section that draws the Quest 3 controllers with what each control does,
built from `[controls]` so it follows a remap.

### HD texture packs (optional)

Packs such as *Shibanauts HD Mod* are loaded by [Astralathe](https://gitlab.com/scrunguscrungus/astralathe),
a separate Psychonauts mod loader, which works alongside this mod. Neither the pack nor Astralathe is
part of this repository. It was tried with Astralathe 2.1.1 and that pack on a Quest 3; the steps (it
ships without the `mod.json` Astralathe needs) are in `READ ME FIRST.txt` under *HD texture packs*.
Two things to know: a pack this size (about 10 GB unpacked) leaves the 32-bit game little spare memory
(about 1.1 of its 2 GB in testing; `psyvr.log` prints it), and some effects are recognised by the size
of their texture, which packs change: `[graphics] texture_pack_compat` allows for that and can be turned
off if a pack makes something look wrong.

### Flag files and environment variables

Create an empty file with one of these names beside the game; it is consumed at the next frame
boundary (except `psyvr_off.flag`, which is checked at start-up and stays).

- `reload.flag`: re-read the *live* settings from `psyvr.ini`.
- `psyvr_off.flag`: the proxy hands Direct3D straight to Windows (no VR, no DXVK, no log).
- `trace.flag`: log one frame's draws in detail to `psyvr.log` (diagnostics).

Environment: `PSYVR_DISABLE=1` does what `psyvr_off.flag` does (the release's
`Play Psychonauts UNMODDED.bat` sets it); `PSYVR_INI=<path>` reads the settings from another
file (the test harness uses it, so a test never writes the player's `psyvr.ini`).

## Caveats

- **Steam build only.** The addresses are from the Steam `Psychonauts.exe` (build stamp
  `0x568C5772`). Every hook and code patch checks the original bytes first and is skipped (logged)
  if they differ, so another build (GOG, disc) runs with those features missing, or without VR.
  The installer warns and asks.
- **Astralathe is optional.** The mod holds its camera settings (no camera shake, the wider cull
  field of view) every frame in native code. With Astralathe installed it also writes a small
  Astralathe mod (`ModResource\PsyVR\Scripts\Dart_hooks.lua`) that sets the same at each level
  start; nothing is lost without it. Texture packs do need it (see above). The **test harness** does need Astralathe 2.1.1 (its test
  mod picks Continue so a test save loads unattended), plus your own test saves
  ([`docs/SAVES.md`](docs/SAVES.md)); none are included.
- **It is a `d3d9.dll`.** Only one can be in the game folder: ReShade and other d3d9 mods are
  moved aside on install and put back on uninstall.
- **It changes `DisplaySettings.ini`.** The game's resolution is set to the headset's (windowed),
  with the player's file kept as `DisplaySettings.ini.psyvr-backup` and restored on uninstall.
  Changing the resolution while the game runs is not handled.
- **A 32-bit OpenXR runtime and Vulkan.** Tested with Virtual Desktop (Quest 3) and the
  OpenXR-Simulator only.
- **Not yet tried much in a headset:** god mode (no seat collision), the theatre's window style
  (`[theatre] style = 1`), the 3D lock-on reticle, the Touch-controller mapping.
- **The DXVK fork** carries code for the Underworld mod (an instanced-stereo path and its
  application profile) that this mod does not use.

## Licence

Apache-2.0 (see [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE)). Third-party code keeps its own
licence: see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). Psychonauts is a trademark of
its owners; this is an unofficial fan modification and contains no game code or data.
