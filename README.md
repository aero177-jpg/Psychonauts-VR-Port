# Psychonauts VR mod

A VR mod for the Steam version of Psychonauts (2005): stereo rendering with 6DoF head tracking,
your VR controllers as the game's pad, and the HUD and menus on a floating screen.

**Status:** 0.1.0 preview. Steam build of `Psychonauts.exe` only. If something looks wrong, it may come from one of the performance options
(all can be switched off in the settings overlay), or simply from this being an old game with its
own original optimisations.

## Features

- Head-tracked stereo, with a level horizon and head look
- Three views on the right stick click: classic chase camera, tabletop "god mode", first person
- HUD, menus and loading screens on a floating screen; a 3D lock-on reticle
- Cutscenes on a floating theatre screen (adjustable shape)
- In-game settings overlay (Insert, or hold the left menu button) with a Quest 3 button guide

## Quick start

You need Psychonauts from Steam, a Vulkan-capable GPU, and a headset with a 32-bit OpenXR
runtime.

1. Download the zip from [Releases](../../releases) (not the source code) and extract all of it.
2. Run `Install.bat`. It finds the game and copies the mod next to `Psychonauts.exe`.
   `Uninstall.bat` removes it again.
3. Connect your headset and start the game from Steam.

`READ ME FIRST.txt` in the zip ([copy](release/READ%20ME%20FIRST.txt)) has the controls, settings
and troubleshooting. Settings live in `psyvr.ini` beside the game; the full list with comments is
[`release/psyvr.sample.ini`](release/psyvr.sample.ini).

## HD texture packs (optional)

Packs such as *Shibanauts HD Mod* are loaded by
[Astralathe](https://gitlab.com/scrunguscrungus/astralathe), a separate mod loader that works
alongside this mod. Neither is included. The pack ships without the `mod.json` Astralathe needs; the
steps are in `READ ME FIRST.txt` under *HD texture packs*.

- A pack this size (about 10 GB unpacked) leaves the 32-bit game little spare memory. If it crashes
  after a while, lower `resolution_scale` or MSAA.
- Some effects are recognised by texture size, which packs change. `texture_pack_compat` (on by
  default, in the overlay's Performance section) allows for that; turn it off if a pack makes
  something look wrong.

## Settings worth knowing

| Key | Default | Meaning |
|---|---|---|
| `[vr] resolution_scale` | `1` | Each eye's render size. Lower is faster. |
| `[graphics] msaa` | `2` | `4` is sharper and slower, `0` is off. |
| `[graphics] fast_fades` | `1` | Cheaper fading of distant things, same picture. |
| `[vr] cull_fov_degrees` | `99` | How wide the game draws. Raise it if things pop out at the edges. |
| `[camera] chase_radius_scale` | `0.756` | Camera distance behind Raz. |
| `[theatre] mode` | `1` | `0` off, `1` cutscenes on a screen, `2` always. |
| `[god] raz_height_cm` | `15` | How tall Raz looks in god mode. |

Most settings apply live: edit `psyvr.ini`, then create an empty file named `reload.flag` in the
game folder. The overlay changes the same settings while you play.

## Caveats

- **Steam build only** (`Psychonauts.exe` stamp `0x568C5772`). On another build, patches that do not
  match are skipped, so features may be missing. The installer warns.
- **It is a `d3d9.dll`.** ReShade and other d3d9 mods are moved aside on install and put back on
  uninstall.
- **It changes `DisplaySettings.ini`** (the game's resolution is set to the headset's); your copy is
  restored on uninstall. Do not change the resolution while the game runs.
- **Astralathe is optional** and only needed for texture packs.
- Less tested: god mode (the seat can pass through walls), the theatre's window style, the lock-on
  reticle.

## For developers

Build instructions, how it works, source layout, tests and diagnostics:
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md). The reverse-engineering log is
[`docs/findings.md`](docs/findings.md).

## Licence

Apache-2.0 (see [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE)). Third-party code keeps its own
licence: see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). Psychonauts is a trademark of its
owners; this is an unofficial fan modification and contains no game code or data.
