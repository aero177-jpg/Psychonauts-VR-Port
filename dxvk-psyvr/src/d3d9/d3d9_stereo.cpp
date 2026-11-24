#include "../dxvk/dxvk_include.h"

#include "d3d9_stereo.h"

#include "d3d9_include.h"
#include "d3d9_device.h"

namespace dxvk {

  // truvr stereo draws (d3d9_stereo.h): arms the device so its next draw
  // runs once per eye inside DXVK (D3D9DeviceEx::StereoDraw).
  class D3D9Stereo final : public ComObjectClamp<IDirect3DStereo9> {

  public:

    D3D9Stereo(IDirect3DDevice9* pDevice)
      : m_device(static_cast<D3D9DeviceEx*>(pDevice)) {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID  riid,
            void**  ppvObject) {
      if (ppvObject == nullptr)
        return E_POINTER;

      *ppvObject = nullptr;

      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(IDirect3DStereo9)) {
        *ppvObject = ref(this);
        return S_OK;
      }

      Logger::warn("D3D9Stereo::QueryInterface: Unknown interface query");
      return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE ArmStereoDraw(
      const D3D9_STEREO_EYE* pEyes) {
      if (unlikely(pEyes == nullptr || !Valid(pEyes[0]) || !Valid(pEyes[1])))
        return D3DERR_INVALIDCALL;

      D3D9DeviceLock lock = m_device->LockDevice();
      return m_device->ArmStereoDraw(pEyes, -1);
    }

    HRESULT STDMETHODCALLTYPE ArmStereoEyeDraw(
      const D3D9_STEREO_EYE* pEyes,
            UINT             Eye) {
      if (unlikely(pEyes == nullptr || Eye > 1u || !Valid(pEyes[0]) || !Valid(pEyes[1])))
        return D3DERR_INVALIDCALL;

      D3D9DeviceLock lock = m_device->LockDevice();
      return m_device->ArmStereoDraw(pEyes, int32_t(Eye));
    }

    HRESULT STDMETHODCALLTYPE DisarmStereoDraw() {
      D3D9DeviceLock lock = m_device->LockDevice();
      m_device->DisarmStereoDraw();
      return D3D_OK;
    }

  private:

    // Within the header's arrays and D3D9's register files (VS 256 and
    // PS 224 float registers), and few enough states for StereoDraw.
    static bool Valid(const D3D9_STEREO_EYE& eye) {
      if (eye.VsRangeCount > 8 || eye.PsRangeCount > 8
       || eye.RenderStateCount > 4 || eye.ClipPlaneMask > 0x3Fu)
        return false;

      uint32_t total = 0;
      for (uint32_t r = 0; r < eye.VsRangeCount; r++) {
        const D3D9_STEREO_RANGE& range = eye.VsRanges[r];
        if (range.Count == 0 || range.Start >= 256 || range.Count > 256 - range.Start)
          return false;
        total += range.Count;
      }
      for (uint32_t r = 0; r < eye.PsRangeCount; r++) {
        const D3D9_STEREO_RANGE& range = eye.PsRanges[r];
        if (range.Count == 0 || range.Start >= 224 || range.Count > 224 - range.Start)
          return false;
        total += range.Count;
      }
      return total <= 64;
    }

    D3D9DeviceEx* m_device;

  };

}

extern "C" HRESULT __stdcall Direct3DCreateStereo9(
        IDirect3DDevice9*  pDevice,
        IDirect3DStereo9** ppInterface) {
  if (pDevice == nullptr || ppInterface == nullptr)
    return D3DERR_INVALIDCALL;

  // As Direct3DCreateVR9: handed out owned (DXVK COM objects start at zero).
  *ppInterface = dxvk::ref(new dxvk::D3D9Stereo(pDevice));
  return D3D_OK;
}
