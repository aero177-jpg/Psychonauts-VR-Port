# Third-party notices

## DXVK (`dxvk-psyvr/`, built into `d3d9_dxvk.dll`)

A fork of [DXVK](https://github.com/doitsujin/dxvk) 3.1 with a D3D9 VR
interop extension (`dxvk-psyvr/src/d3d9/d3d9_vr.h`); the same fork as the
Tomb Raider: Underworld VR mod's `dxvk-truvr`. DXVK's built-in application
profile is not relied on; the mod overrides the frame-rate cap through
`DXVK_CONFIG`. zlib/libpng licence; see `dxvk-psyvr/LICENSE`. Its bundled
headers and subprojects (Vulkan-Headers, SPIRV-Headers, mingw-directx-headers,
dxbc-spirv, libdisplay-info) keep their own licences in their folders.

## Dear ImGui (`third_party/imgui/`)

The settings overlay, linked into `d3d9.dll`. MIT, Copyright (c) 2014-2025
Omar Cornut; see `third_party/imgui/LICENSE.txt`.

## Vulkan headers (`third_party/vulkan/`)

The Khronos Vulkan headers, for the proxy's build. Apache-2.0.

## TRAWindowed


The D3D9 proxy wrapper in `src/proxy/` started from
[TRAWindowed](https://github.com/chreden/trawindowed) by chreden.

MIT License

Copyright (c) 2018 chreden

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## OpenXR SDK (`third_party/openxr/`)

The Khronos OpenXR headers and the static 32-bit loader (`openxr_loader.lib`),
linked into `d3d9.dll`. Apache-2.0; see `third_party/openxr/LICENSE`.

## The Tomb Raider VR mods

Parts of this mod come from the Legend and Underworld VR mods by the same author
(Apache-2.0): the OpenXR runtime layer (`src/vr/xr_runtime.*`), the inline
hook (`src/proxy/hook.*`), the logger, the backend loader and the shape of
the D3D9 wrappers.

## Reference only

- **Astralathe** (a Psychonauts mod loader) is optional. When it is installed,
  the mod adds a small Astralathe mod (`mod/PsyVR`, written at start-up) and
  the test harness uses it to load test saves. It is not part of this
  repository or the package.
- **OpenXR-Simulator** (elliotttate, MIT) is used as a test runtime on the
  development machine. It is not part of this repository or the package.
- **Ghidra** (NSA, Apache-2.0) may be used to analyse
  the game on the development machine. It is not distributed, and no game
  code or data is included in this repository.
