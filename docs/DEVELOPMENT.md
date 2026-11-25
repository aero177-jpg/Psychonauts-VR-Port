# Development

Building the mod, how it works, and where things live. Players do not need any of this: see the
[README](../README.md).

## How it works

A `d3d9.dll` proxy sits beside `Psychonauts.exe` (the game loads `d3d9.dll` and finds ours first)
and forwards to a DXVK fork (`d3d9_dxvk.dll`, D3D9 to Vulkan), whose Vulkan device an OpenXR
session is created on. The proxy wraps `IDirect3D9`, the device and the swap chain, so every draw
passes through it.

The back buffer is made twice as wide, one half per eye, and the game's resolution
(`DisplaySettings.ini`) is set to match, so it renders its scene targets at headset size. Each
3D draw is classified from what its vertex shader reads and is drawn once per eye with the eye's
projection folded into the game's own WVP (recovered from the constants it uploads; see
[`shaders.md`](shaders.md)). 2D draws go to the floating HUD screen. A small set of
signature-checked patches in the game's code hold the camera level, widen its culling, move the
chase camera for god mode and first person, and read the lock-on target.

The draw logic lives in a second DLL, `psyvr_logic.dll`, that can be rebuilt and swapped while
the game runs (see [`../tools/README.md`](../tools/README.md), "Hot reload").
[`findings.md`](findings.md) is the reverse-engineering log: what was found in the game, dated,
with the reasoning behind each design decision.

## Layout

- `src/proxy/`: the `d3d9.dll` proxy. D3D9 wrappers, backend loader, window fixes,
  `game_psy.cpp` (everything that touches `Psychonauts.exe` memory, with the addresses),
  `hook.cpp` (signature-checked inline hook), `logic_host` (loads and hot-reloads the logic DLL),
  `overlay` (settings overlay), and the diagnostics `trace`, `drawlog` and `control`.
- `src/logic/`: the logic DLL's entry point; with `src/vr/stereo.cpp` and `god_frame.cpp` it holds
  the draw classification and god mode's per-frame camera logic.
- `src/vr/`: OpenXR runtime layer, head and eye poses, frame submission, stereo, HUD screen,
  controllers-as-gamepad, theatre, god mode. Pure maths lives in modules that say so in their
  header, so the tests can link them.
- `src/vr/shaders/`: the replacement pixel shaders (HLSL and compiled headers).
- `src/common/`: logging, settings (`psyvr.ini`), the logic-DLL interface, the generated
  `default_ini.h`.
- `src/test/`: unit tests (`mathtest`, `configtest`) and `xrprobe`.
- `release/`: what ships in the package: `READ ME FIRST.txt`, `psyvr.sample.ini`, the installer
  and the Play scripts.
- `mod/PsyVR/`: the optional Astralathe part.
- `dxvk-psyvr/`: the DXVK fork with its OpenXR interop (vendored source).
- `third_party/`: OpenXR (headers, static 32-bit loader), Dear ImGui, Vulkan headers.
- `tools/`: test driver, Ghidra scripts, generators ([`../tools/README.md`](../tools/README.md)).

## Build

Requirements: Windows, Visual Studio (or the Build Tools) with the C++ x86 tools (found through
`msvc.bat`), Python 3. Building the DXVK fork also needs meson, ninja
(`py -3 -m pip install --user meson ninja`) and glslang (a
[KhronosGroup/glslang](https://github.com/KhronosGroup/glslang/releases) release in
`%USERPROFILE%\tools\glslang`, or the Vulkan SDK on PATH). Run the scripts by full path
(`cmd /c "C:\...\build.bat"`).

- `build.bat`: builds `dist\d3d9.dll` (32-bit, static CRT) and `dist\psyvr_logic.dll`.
  `build.bat logic` builds only the latter.
- `build_dxvk.bat`: builds `dist\d3d9_dxvk.dll` from `dxvk-psyvr` (several minutes the first time).
- `install.bat [game folder]` / `uninstall.bat [game folder]`: a developer install from `dist\`.
- `package.bat [folder] [zip]`: assembles the release package into `%USERPROFILE%\Desktop\PsyVRmod`.
- `test.bat`: offline checks, no game needed (maths, settings loader, and that the embedded default
  ini matches `release\psyvr.sample.ini`; after editing the sample run
  `py -3 -I tools\gen_default_ini.py`).
- `xrprobe.bat [runtime.json]`: reports what the 32-bit OpenXR runtime offers.

## Testing

`tools\drive.ps1` drives the game in the OpenXR Simulator (`Run-Smoke` is the standard check). It
needs [Astralathe](https://gitlab.com/scrunguscrungus/astralathe) 2.1.1 (its test mod loads a save
unattended) and your own test saves ([`SAVES.md`](SAVES.md)); none are included. See
[`../tools/README.md`](../tools/README.md).

## Flag files and environment variables

Empty files beside the game, consumed at the next frame boundary (except `psyvr_off.flag`):

- `reload.flag`: re-read the live settings from `psyvr.ini`.
- `psyvr_off.flag`: hand Direct3D straight to Windows (no VR, no DXVK, no log). Stays.
- `trace.flag`: log one frame's draws in detail to `psyvr.log`.

`PSYVR_DISABLE=1` does what `psyvr_off.flag` does. `PSYVR_INI=<path>` reads the settings from
another file (the test harness uses it).

## Notes

- Every hook and code patch checks the original bytes first and is skipped (logged) if they
  differ, so another game build runs with those features missing.
- The mod holds its camera settings (no shake, the wider cull FOV) every frame in native code; with
  Astralathe installed it also writes a small mod (`ModResource\PsyVR`) that sets them at level start.
