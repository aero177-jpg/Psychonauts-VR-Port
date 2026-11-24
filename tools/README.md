# tools

Everything here is for development and diagnosis; none of it ships with the mod.

## Driving the game: `drive.ps1`

Dot-source it in PowerShell (`. tools\drive.ps1`). Every machine-specific path sits in one configuration block at the top of the script and can be overridden by an environment variable set before dot-sourcing: `PSY_GAME_DIR` (default: Steam's install folder; `Psychonauts.exe` is launched directly, it is not SteamStub-wrapped), `PSY_REPO_DIR` (default: this repository), `PSY_WORK_DIR` (screenshots, test saves, kept settings and spots; default `%USERPROFILE%\tools\work`), `PSY_XR_SIM_JSON` (manifest of a 32-bit build of [OpenXR-Simulator](https://github.com/elliotttate/OpenXR-Simulator), which stands in for a headset; default `%USERPROFILE%\tools\openxr-sim\src\bin\openxr_simulator.json`) and, for the `Ghidra` function, `PSY_GHIDRA_DIR`, `PSY_GHIDRA_PROJECT`, `PSY_GHIDRA_NAME`.

The harness needs Astralathe 2.1.1 (a Psychonauts mod loader, installed separately) in the game folder (its test mod, `astralathe_test_mod\PsyVRTest`, picks Continue on the main menu so a test save loads with no keys sent) and your own test saves (docs/SAVES.md). Playing the mod needs neither.

| Function | What it does |
|---|---|
| `Run-Smoke [-Save gpc\|reception\|circus] [-Ini @(...)] [-Settle s] [-Keep] [-Force]` | The unattended check of the whole VR path (about 45 s): loads a test save (docs/SAVES.md; default `gpc`) on the simulator and prints PASS/FAIL for the OpenXR session, the first VR frame, the level, eyes not black, eyes differ, disparity sign (every band's shift <= 0: distant things ~0, nearer ones negative), game responding, and no `HANG` / `failed` / `skipped (code at` log lines. It also reports the simulator's and the mod's fps. Eye pictures, the log and `summary.json` go to `<work dir>\shots\smoke_<time>`. Ends with `Clean-Game` unless `-Keep`, and refuses while the game runs unless `-Force`. |
| `Quick-Vr [-Ini @(...)] [-Save name] [-Continue] [-Wait s] [-Keep]` | Starts the game on the simulator (title screen, or a test save with `-Save`; `-Continue` = `-Save gpc`), waits for the first VR frame / the level, then `-Wait` more seconds, and prints alive + key log lines. |
| `Run-Vr [-Ini @(...)] [-Save name] [-Settle s] [-StopAtMenu] [-ShotWidth n] [-LeftEye]` | Like `Quick-Vr -Save name -Keep` (or the main menu with `-StopAtMenu`) followed by `Shot-All`. |
| `Start-TestGame [-Ini @(...)] [-Save name] [-Continue] [-Force]` | The launch step all of the above share: install, test ini (`control_pipe`, `hot_reload`, `hang_exit_seconds=20` and `skip_fullscreen_passes=1` merged into `[debug]`: the full-screen passes stay off in tests, as the user plays), the Continue test mod, the test save in profile 2 slot 5 with the profile file pointed at it (`Restore-TestSave`, `Set-TestProfile`; `Stop-Psy` puts the player's profile back), fresh `astralathe.log`. Sends no keys: the intro videos play out (main menu ~25 s after launch). |
| `Wait-Log path regex [timeout] [fromLine]`, `Wait-Status CAGP [timeout]` | Wait on a log line (shared read; `$PsyLog`, `$AstraLog`) or on the level the control pipe reports, instead of fixed sleeps; `$null` on timeout or when the game exits. |
| `Compare-Eyes left.png right.png [-Bands 5]` | `MeanDiff` (mean abs RGB difference), `LumL` / `LumR`, and per horizontal band the x shift s with R(x+s) ~ L(x), in eye pixels (search of +-40 steps of 480-wide copies; ties to the smaller shift). Note: in mono (`stereo = 0`) `shot` still cuts the buffer in halves, so the "eyes" are the two halves of one picture. |
| `Get-SimFps` | The simulator's average fps, from its preview window title. |
| `Psy-Head x y z [yaw pitch roll]`, `Psy-Head -Off` | A scripted head pose on top of the tracked head (pipe command `head`): metres in the recentred room (+x right, +y up, +z back), degrees (yaw + = left, pitch + = up, roll + = left ear down). Everything that reads the head sees it (eyes, HUD, god mode). The frame drawn after the command is the first with it. |
| `Psy-Sweep [name] [-Poses basic\|turn\|'label=x,y,z[,yaw,pitch,roll]',...]` | Eye shots (`shot <name> eyes`: no trace or draw log) at each pose, one frame after another, into `<work dir>\shots\sweep_<name>_<time>\` with `manifest.json` and `contact.png`, then clears the head script. Needs a running game (`Quick-Vr -Save name -Keep`). `basic` = centre, x / y +-5 cm, z +-10 and +-25 cm, yaw / pitch +-10 deg (13 poses, ~20 s); `turn` = yaw +-30 / +-70, pitch +-30, roll 20. The manifest has per pose the L/R band shifts and, against the first pose, the L eye's mean difference and band shifts (pixels; horizontal search only, so pitch rows mean nothing). Check: yaw 10 deg should shift the whole picture by f*tan(10 deg) (~80 px with the simulator's 1280-px eyes). |
| `New-ContactSheet sweepDir` | The sweep's `contact.png`: one tile per pose with the L eye, a red/cyan anaglyph against the first pose (grey = aligned, fringes = moved) and the difference x4. The first tile shows its own L/R pair instead (stereo). Consecutive frames: Raz's idle animation shows as small differences. |
| `Push-Logic [-TimeoutSec 20] [-NoWait]` | Runs `build.bat logic` (only `psyvr_logic.dll`), copies it to the game folder and, if the game runs, waits for the swap (`psyvr.log`: `logic: reloaded in N ms` or `reload failed`). Returns `{Ok, Stage, BuildSec, ReloadMs, TotalSec, Line}`; a build error is printed and returned (`Stage = build`) without touching the game. Any game started by `Start-TestGame` / `Quick-Vr` / `Run-Vr` has `[debug] hot_reload=1`. See "Hot reload" below. |
| `Psy-Where [-Name n] [-Note text]` | Reads Raz's position and facing (`RazMatrix`: his rotation rows, `RazYaw`) and the game camera (position, quaternion, yaw / pitch, cull FOV) from the running game's memory. With `-Name` it appends the record to `<work dir>\spots.json` and keeps an eye shot in `<work dir>\spots`. For marking spots while the user drives. |
| `Psy-Teleport name [-Pos x,y,z] [-NoStep]` | Puts Raz at a spot from `spots.json` (its `Raz`, and his facing when the spot has `RazMatrix`; refused if the game is in another level) or at `-Pos`, by writing his world matrix's position. The area there only loads once Raz moves, so a 0.5 s step forward follows and the position is written again (`-NoStep`: just the write). Returns `Psy-Where`. A teleport skips triggers on the way (doors, enemy spawns): see `Psy-GotoEnemy`. |
| `Psy-GotoEnemy` | With `circus` loaded (`Quick-Vr -Save circus -Keep`): Raz into the Tent City tent with an enemy for targeting tests (it barely moves and does not attack). Teleports to `aiming_door`, walks 2 s through the door (throws if he is not inside after it), then teleports to `aiming_tent_enemy` with his recorded facing. About 5 s. |
| `Psy-Walk [-Seconds 1] (-Yaw deg \| -Spot name)` | Walks Raz towards a world heading (`Psy-Where`'s yaw convention), or the spot's recorded camera heading: the stick is turned by the camera's heading at the start. Returns `Psy-Where`. |
| `Clean-Game` | Kills the game, uninstalls the mod, removes the test settings (`psyvr.test.ini`), logs, flag files and `psyvr_shots`, restores the player's `psyvr.ini` (`Restore-UserIni`). Always run it last. |
| `Save-UserIni`, `Restore-UserIni` | Tests never write the player's `psyvr.ini`: their settings go to `psyvr.test.ini`, and only the game the harness starts gets `PSYVR_INI` pointing at it, so a Steam or headset launch always reads the player's file (and runtime). `Save-UserIni` keeps a copy in `<work dir>\settings`; `Restore-UserIni` puts it back if an older harness left a test ini in its place. |
| `Shot-All [prefix] [width] [-LeftEye]` | Screenshots every visible window of the game with `PrintWindow` to `<work dir>\shots`. |
| `Start-Psy`, `Stop-Psy` | Launch and kill. |
| `Key NAME`, `Click x y`, `Unlock-Fg` | Input (virtual-key events after an Alt tap). Do not press keys in gameplay. |
| `Get-Draws` | Draws in the most recent logged frame (`psyvr.log`, every 300 frames). |
| `Pad-Script @('2 ly=1', '1', '1.5 rx=1') [-Wait]` | Scripted gamepad input: writes `pad_script.txt`; the mod reads and deletes it and plays it. One step per line, `<seconds> [token ...]` (up to 200 steps, 600 s each; `#` comments): sticks `lx= ly= rx= ry=` (-1..1), triggers `lt= rt=` (0..1), buttons `a b x y lb rb l3 r3 up down left right start back`. |
| `Psy-Cmd "command"` | Sends one command to the running game over the pipe `\\.\pipe\psyvr` (needs `[debug] control_pipe = 1`). Commands: `ping`, `status`, `trace n`, `shot name`, `drawlog name`, `set section key value`. |
| `Psy-Shot name` | `shot`, then cuts the traced frame's `psyvr.log` lines into `<game>\psyvr_shots\name.trace.txt`; returns the eye PNGs, the trace and the draw log. |
| `Psy-DrawLog name` | The next frame's draw log (JSON lines); summarise with `python -I tools\drawlog_summary.py <file>`. |
| `Ghidra script.java out.txt args...` | Runs `DumpFuncs.java` / `RefsGrep.java` headless against your own Ghidra project of `Psychonauts.exe` (`PSY_GHIDRA_*`). |

## Other tools

- `gen_default_ini.py`: regenerates `src\common\default_ini.h` from `release\psyvr.sample.ini` (`--check` verifies; `test.bat` runs it).
- `drawlog_summary.py`: tables of a draw log (`psyvr_shots\*.draws.jsonl`).
- `DumpFuncs.java`, `RefsGrep.java`: generic Ghidra helpers.
- `dxvk_path.bat`: puts meson, ninja and glslang on PATH for `build_dxvk.bat` when they are in their usual places.

## Hot reload (logic changes without a relaunch)

`psyvr_logic.dll` holds the stereo draw logic (`stereo.cpp`, `sprite_reproject.cpp`) and god mode's per-frame camera logic (`god_frame.cpp`, `god_seat.cpp`: seat, stick, head aim, cull field of view, and which camera fields to write). With `[debug] hot_reload = 1` the shim watches the file and swaps a rebuilt copy in at a frame boundary.

```powershell
. .\tools\drive.ps1
Quick-Vr -Save reception -Keep -Ini @('[god]','enabled=1')   # once: about a minute to gameplay
# edit src\vr\god_frame.cpp (or stereo.cpp, ...)
Push-Logic          # build ~7 s, the swap itself ~50 ms; the game keeps running
Psy-Sweep cull -Poses turn ; Psy-Shot name ; Psy-Cmd status   # look at the result
Clean-Game          # always, at the end
```

God mode's camera changes are a list of writes into the camera (`GodFrameOut.writes`: offset from the camera, up to 64 bytes each, 16 per frame, within 0x1000). The shim applies them after the game's chase update and puts back the bytes they replaced before the next one, so a new camera field can be tried from the logic alone. The seat state (`GodSeatState`) lives in the shim and survives a reload.

Needs a relaunch: anything in the shim (`src\proxy\*`, `vr_session`, `xr_runtime`, hooks, the overlay, the horizon lock in `game_psy.cpp`), a change to `logic_abi.h` or to a struct crossing it (`Config`, `Mat4`, `GodSeatState`, `god_frame.h`: bump `kLogicAbiVersion`; a mismatched DLL is refused with `logic: host does not match` and the old one stays loaded), and start-up-only ini settings. Shaders are patched when created, so a change to the patching code only reaches shaders created after the reload. Measured on the simulator (`reception`, god mode): `Push-Logic` 7 to 8 s in total, the swap 47 ms with 455 shaders replayed.
