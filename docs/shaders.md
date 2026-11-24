# Psychonauts vertex shader constants

Source: the game ships its vertex shader source in
`<game>\WorkResource\VertexShaders` (530 `.vsh`, shared header `VertexShader.h`,
assembled combos in `cache\vs_<hash>.vsh` with compiled `.vpo` beside them).
Pixel shaders are compiled only (`PixelShaders\*.fpo`, 59 files).
PC builds compile with `_PCBUILD` (see `PreprocessVertexShaders.bat`).

## Fixed register layout (from VertexShader.h)

| Register | Name | Use |
| --- | --- | --- |
| c0 / c1 | C_ZERO / C_ONE | constants |
| c2 | C_MATERIAL_COLOR | |
| c3 | C_AMBIENT_DIFFUSE | |
| c4 | C_FOG_RANGE | distance fog x,y; height fog z,w |
| c5 | C_DETAIL_FACTOR | |
| **c6–c9** | **C_PROJECTION_MATRIX** | **full object→clip matrix (world·view·proj). The only camera matrix.** |
| c10–c48 | per-shader | sprites: c10 scale, c11 offset; particles: c10 billboard X, c14 billboard Y, c17 threshold; water: c10 matrix |
| c49 | C_VIEWPORT_SCALE | |
| c50 | C_VIEWPORT_OFFSET | PC: `add oPos, r4, c50` after every transform (half-pixel fixup) |
| c51 | C_MODEL_EYE_POS | eye position in model space (fog, specular) — view dependent |
| c52 | C_WORLD_HEIGHT_TRANS | height fog plane |
| c64–c93 | lights | 8 point lights (pos/color/atten), 3 spot directions/cones |
| c94 | C_TEXCOORD_DECOMPRESS | |

Every geometry path ends `m4x4 OPOS, <pos>, c6` then `FIXUP_OPOS`
(460 of 530 .vsh reference it; the rest are lighting/fog/texture fragments
with no position output, plus `Sprite.vsh`).

## What this means for stereo

There is no separate view matrix in the shader, only WVP = P·V·W. A per-eye
matrix can be built in clip space without knowing V or W:

    WVP_eye = P_eye · E · P_game⁻¹ · WVP

- `P_game` = the game's projection (from FOV/aspect/near/far; capture or derive).
- `E` = eye transform relative to the game camera (IPD offset × head pose delta).
- `P_eye` = runtime asymmetric projection from `xrLocateViews`.

Let `M = P_eye · E · P_game⁻¹`. Apply M to c6–c9 on every draw that
uses the standard transform.

Other view-dependent constants:
- **Particles (ParticleBB)**: billboard offsets c10/c14 are clip-space vectors
  added after the c6 transform → transform them by M as well.
- **c51 model eye pos**: shifts slightly per eye; ignorable at first (fog/specular).
- **c50 viewport offset**: half-pixel; leave as-is.
- **Sprite.vsh** (`mad oPos, v0, c10, c11`): screen-space 2D → HUD candidate.
- **CheapWater / Water**: project screen-space reflection coords from r0 (clip
  pos) into oT1. The reflection texture (512x512, shrunk to 256x256) is drawn
  once through the game camera, so these two shaders are created patched to
  take oT1 from the game's own WVP in c56..c59 (`psy_patch_screen_coords`,
  docs/findings.md 2026-11-16), and to write that clip position to oT4,
  which the replacement pixel shaders (src/vr/shaders/water_ps.hlsl) divide
  per pixel.

Horizon lock needs the camera's view (to remove pitch/roll), which the shader
doesn't expose; that comes from the engine camera (Lua `GetCameraOrientation`
or native camera struct) later.

Open: how the game builds `P_game` (FOV units: plan doc suggests degrees,
chase default 110). Capture it per frame from a draw whose world is identity,
or from the engine.
