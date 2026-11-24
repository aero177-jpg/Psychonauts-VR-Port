#pragma once

// Stereo draws for the Tomb Raider: Underworld VR mod (truvr).
//
// The mod draws every camera draw once per eye. Doing that through the D3D9
// API (Set* per eye, the draw, Set* again, restore) cost about as much CPU on
// the game's render thread as the whole mono frame (docs/AUDIT.md 43). With
// this interface the mod describes what differs per eye as data, arms the
// device, and the game's next draw call runs once per eye inside DXVK, which
// puts the game's own state back as it was afterwards.
//
// Instanced: the armed draw becomes ONE Vulkan draw with two instances; the
// vertex shader takes the eye from InstanceIndex, reads c0..c31 from a
// per-eye constant table (the game's values with each eye's VS ranges laid
// over them) and maps x into its half of a double-width viewport. That only
// works for simple draws, so ArmStereoDraw refuses (D3DERR_NOTAVAILABLE,
// nothing armed, the caller draws each eye itself) unless: neither eye sets
// pixel constants, clip planes or render states; every VS range lies within
// registers 0..31; the eyes' viewports sit side by side (eye 1 starts where
// eye 0 ends, same Width/Y/Height/MinZ/MaxZ); a programmable vertex shader
// is bound, with hardware vertex processing and no instanced vertex streams.

#include <d3d9.h>

// A run of vec4 float registers an eye overrides (VS or PS).
struct D3D9_STEREO_RANGE {
  UINT Start;
  UINT Count;
};

// What one eye changes. Anything not listed keeps the game's own value.
struct D3D9_STEREO_EYE {
  D3DVIEWPORT9      Viewport;

  // Float constants: VsRangeCount VS ranges, then PsRangeCount PS ranges,
  // their values packed back to back in Data in that order.
  UINT              VsRangeCount;
  D3D9_STEREO_RANGE VsRanges[8];
  UINT              PsRangeCount;
  D3D9_STEREO_RANGE PsRanges[8];
  float             Data[64][4];

  // User clip planes this eye sets (bit i = plane i, values in ClipPlanes[i]).
  DWORD             ClipPlaneMask;
  float             ClipPlanes[6][4];

  // Render states this eye sets (index i = RenderStates[i]); at most 4.
  UINT              RenderStateCount;
  D3DRENDERSTATETYPE RenderStates[4];
  DWORD             RenderStateValues[4];
};

MIDL_INTERFACE("6f0d2b7e-91c4-4b8a-a3e5-7c2d14b9e0f2")
IDirect3DStereo9 : public IUnknown {
  // The next DrawPrimitive / DrawIndexedPrimitive / DrawPrimitiveUP /
  // DrawIndexedPrimitiveUP on this device draws both eyes as one instanced
  // draw (see above); the game's constants and viewport are unchanged
  // afterwards. pEyes points to two eyes; they are copied, so the caller's
  // storage may be reused at once. Call on the thread that draws, right
  // before the draw. D3DERR_INVALIDCALL if the description is out of range;
  // D3DERR_NOTAVAILABLE if this draw cannot be instanced (the caller draws
  // per eye itself). Nothing is armed in either case.
  virtual HRESULT STDMETHODCALLTYPE ArmStereoDraw(
    const D3D9_STEREO_EYE* pEyes) = 0;

  // Drops an armed draw that will not happen (e.g. the draw was skipped).
  virtual HRESULT STDMETHODCALLTYPE DisarmStereoDraw() = 0;

  // The next draw draws one eye (0 or 1) alone, with the same vertex maths
  // as an instanced stereo draw (eye's c0..c31 from pEyes, the double-width
  // viewport, x squeezed into the eye's half), so passes of one mesh drawn
  // either way match exactly (a depth-EQUAL pass after an instanced one).
  // Only the eyes' vertex constants and viewports are used: the caller sets
  // that eye's pixel constants, clip planes and render states on the device
  // itself. D3DERR_NOTAVAILABLE when the draw cannot go this way (fixed-
  // function VS, instanced streams, VS ranges past c31, odd viewports).
  virtual HRESULT STDMETHODCALLTYPE ArmStereoEyeDraw(
    const D3D9_STEREO_EYE* pEyes,
          UINT             Eye) = 0;
};

#ifdef _MSC_VER
struct __declspec(uuid("6f0d2b7e-91c4-4b8a-a3e5-7c2d14b9e0f2")) IDirect3DStereo9;
#else
__CRT_UUID_DECL(IDirect3DStereo9, 0x6f0d2b7e, 0x91c4, 0x4b8a, 0xa3, 0xe5, 0x7c, 0x2d, 0x14, 0xb9, 0xe0, 0xf2);
#endif

// pDevice must be the DXVK device itself (not a wrapper). The result is
// AddRef'd; the caller releases it.
extern "C" HRESULT __stdcall Direct3DCreateStereo9(
        IDirect3DDevice9*  pDevice,
        IDirect3DStereo9** ppInterface);
