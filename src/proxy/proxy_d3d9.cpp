#include "proxy_d3d9.h"
#include "msaa_cap.h"
#include "proxy_device.h"
#include "window.h"
#include "game_psy.h"
#include "vr_present_size.h"
#include "../vr/vr_submit.h"
#include "../vr/hud.h"
#include "../vr/xr_runtime.h"
#include "backend.h"

#include "../common/config.h"
#include "../common/log.h"

namespace psyvr
{
    namespace
    {
        const char* devtype_name(D3DDEVTYPE t)
        {
            switch (t)
            {
            case D3DDEVTYPE_HAL:      return "HAL";
            case D3DDEVTYPE_REF:      return "REF";
            case D3DDEVTYPE_SW:       return "SW";
            case D3DDEVTYPE_NULLREF:  return "NULLREF";
            default:                  return "?";
            }
        }

        // One block, once, with everything a bug report needs: the build,
        // the game, what VR came up as (or why not) and the effective
        // settings, after clamping.
        void log_startup_summary(const D3DPRESENT_PARAMETERS* pp)
        {
            static bool done = false;
            if (done)
                return;
            done = true;

            const Config& c = config();
            wchar_t exe[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);

            log("---- summary ------------------------------------------------");
            log("mod build %s %s", __DATE__, __TIME__);
            log("game %S: %s", exe,
                game_is_supported_build() ? "the Steam build the mod was made for"
                                          : "not the Steam build the mod was made for; "
                                            "hooks whose code differs are skipped (see below)");
            log("backend %S", backend_path());
            if (vr_submit_ready())
            {
                unsigned w = 0, h = 0;
                vr_recommended_eye_size(&w, &h);
                log("vr: up (%s), %ux%u an eye, OpenXR session running", xr_system_name(), w, h);
            }
            else if (!c.vr_enabled)
            {
                log("vr: off ([vr] enabled = 0); a plain windowed proxy");
            }
            else if (!vr_ready())
            {
                log("vr: not available (%s); the game runs flat", xr_last_error());
            }
            else
            {
                log("vr: runtime found (%s) but no session yet; the game runs flat", xr_system_name());
            }
            if (pp)
                log("display: back buffer %ux%u, %s, MSAA %d, present interval 0x%X",
                    pp->BackBufferWidth, pp->BackBufferHeight,
                    pp->Windowed ? "windowed" : "fullscreen", (int)pp->MultiSampleType,
                    pp->PresentationInterval);
            log("graphics: msaa cap %d, vsync %d (-1 = game), frame cap %d (0 = unlocked)",
                c.msaa, c.vsync, c.max_frame_rate);
            log("view: cull fov %.0f deg, "
                "world scale %g, recenter key vk 0x%02X",
                c.cull_fov_degrees, c.world_scale,
                c.recenter_vk);
            log("hud: %s, %.2f m, %.0f deg wide; menus %.2f m, %.0f deg wide, shape %.2f",
                c.hud_enabled ? "virtual screen" : "in each eye", c.hud_distance,
                c.hud_width_degrees, c.hud_menu_distance, c.hud_menu_width_degrees,
                c.hud_menu_aspect);
            log("controls: %s, rumble %.2f", c.controls.enabled ? "as pad 0" : "off", c.controls.rumble);
            log("-------------------------------------------------------------");
        }

        void log_present_parameters(const char* what, const D3DPRESENT_PARAMETERS* pp)
        {
            log("  %s: backbuffer %ux%u fmt=%d count=%u windowed=%d refresh=%u interval=0x%X",
                what, pp->BackBufferWidth, pp->BackBufferHeight, (int)pp->BackBufferFormat,
                pp->BackBufferCount, pp->Windowed, pp->FullScreen_RefreshRateInHz,
                pp->PresentationInterval);
            log("  %s: depth fmt=%d autodepth=%d swapeffect=%d multisample=%d quality=%lu flags=0x%lX",
                what, (int)pp->AutoDepthStencilFormat, pp->EnableAutoDepthStencil,
                (int)pp->SwapEffect, (int)pp->MultiSampleType, pp->MultiSampleQuality,
                pp->Flags);
        }
    }

    HRESULT ProxyD3D9::QueryInterface(REFIID riid, void** ppvObj)
    {
        if (!ppvObj)
            return E_POINTER;

        if (riid == IID_IUnknown || riid == IID_IDirect3D9)
        {
            AddRef();
            *ppvObj = this;
            return S_OK;
        }

        // IDirect3D9Ex would let the caller create a device we never see.
        HRESULT hr = _d3d9->QueryInterface(riid, ppvObj);
        if (SUCCEEDED(hr))
        {
            ((IUnknown*)*ppvObj)->Release();
            *ppvObj = nullptr;
            return E_NOINTERFACE;
        }
        return hr;
    }

    // The qualities offered to the game stop at the [graphics] msaa cap
    // (msaa_cap.h), so it never asks for more.
    HRESULT ProxyD3D9::CheckDeviceMultiSampleType(UINT Adapter,
        D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed,
        D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels)
    {
        const int max_quality = msaa_max_quality();
        if (MultiSampleType != D3DMULTISAMPLE_NONE)
        {
            if (max_quality < 0)
                return D3DERR_NOTAVAILABLE;
            if (MultiSampleType != D3DMULTISAMPLE_NONMASKABLE &&
                int(MultiSampleType) > (1 << max_quality))
                return D3DERR_NOTAVAILABLE;
        }
        const HRESULT hr = _d3d9->CheckDeviceMultiSampleType(Adapter,
            DeviceType, SurfaceFormat, Windowed, MultiSampleType,
            pQualityLevels);
        if (SUCCEEDED(hr) && pQualityLevels &&
            MultiSampleType == D3DMULTISAMPLE_NONMASKABLE &&
            *pQualityLevels > DWORD(max_quality + 1))
            *pQualityLevels = DWORD(max_quality + 1);
        return hr;
    }

    HRESULT ProxyD3D9::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                    DWORD BehaviorFlags,
                                    D3DPRESENT_PARAMETERS* pPresentationParameters,
                                    IDirect3DDevice9** ppReturnedDeviceInterface)
    {
        log("CreateDevice (thread %lu): adapter=%u type=%s hwnd=0x%p flags=0x%08lX", GetCurrentThreadId(),
            Adapter, devtype_name(DeviceType), (void*)hFocusWindow, BehaviorFlags);

        if (pPresentationParameters)
        {
            D3DPRESENT_PARAMETERS* pp = pPresentationParameters;
            log_present_parameters("game asked", pp);

            if (config().vsync >= 0)
            {
                pp->PresentationInterval = config().vsync ? D3DPRESENT_INTERVAL_ONE
                                                          : D3DPRESENT_INTERVAL_IMMEDIATE;
                log("  present interval forced to %s by [graphics] vsync",
                    config().vsync ? "ONE" : "IMMEDIATE");
            }

            if (msaa_clamp(&pp->MultiSampleType, &pp->MultiSampleQuality))
                log("  multisample capped by [graphics] msaa = %d: type %d, quality %lu",
                    config().msaa, (int)pp->MultiSampleType, pp->MultiSampleQuality);

            if (vr_ready())
            {
                // The headset, not the monitor, decides the size; and a
                // fullscreen mode would only fight the compositor.
                pp->Windowed = TRUE;
                pp->FullScreen_RefreshRateInHz = 0;
                size_for_vr(pp);
            }
            else if (config().force_windowed)
            {
                if (!pp->Windowed)
                {
                    log("  forcing windowed: exclusive fullscreen fights alt-tab and the VR compositor");
                    pp->Windowed = TRUE;
                    pp->FullScreen_RefreshRateInHz = 0;
                }
                // Render at the window's size rather than the desktop's.
                UINT w = 0, h = 0;
                window_choose_size(hFocusWindow, pp->BackBufferWidth, pp->BackBufferHeight, &w, &h);
                if (w && h)
                {
                    pp->BackBufferWidth = w;
                    pp->BackBufferHeight = h;
                }
            }
        }

        // The VR submit path touches the device from the present path, and
        // DXVK's device lock is a no-op without this flag, which the game
        // does not ask for (its flags are PUREDEVICE | HARDWARE_VERTEXPROCESSING).
        if (vr_ready() && !(BehaviorFlags & D3DCREATE_MULTITHREADED))
        {
            BehaviorFlags |= D3DCREATE_MULTITHREADED;
            log("  forcing D3DCREATE_MULTITHREADED for the VR submit path");
        }
        // A pure device refuses state queries and state blocks, which the
        // HUD redirect and composite use (hud.cpp).
        if (vr_ready() && (BehaviorFlags & D3DCREATE_PUREDEVICE))
        {
            BehaviorFlags &= ~D3DCREATE_PUREDEVICE;
            log("  dropping D3DCREATE_PUREDEVICE: the HUD needs state queries");
        }

        IDirect3DDevice9* device = nullptr;
        const HRESULT hr = _d3d9->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                               pPresentationParameters, &device);
        if (FAILED(hr) || !device)
        {
            log("CreateDevice: FAILED hr=0x%08lX", hr);
            if (ppReturnedDeviceInterface)
                *ppReturnedDeviceInterface = nullptr;
            return hr;
        }

        // The wrapper owns the reference the runtime just gave us.
        AddRef();
        *ppReturnedDeviceInterface = new ProxyDevice(device, this);
        if (pPresentationParameters)
            log_present_parameters("created", pPresentationParameters);
        log("CreateDevice: ok, device wrapped");
        game_apply_patches();

        // hDeviceWindow is the one actually presented to; the focus window
        // is only the one D3D listens to for mode changes.
        HWND target = pPresentationParameters && pPresentationParameters->hDeviceWindow
                    ? pPresentationParameters->hDeviceWindow
                    : hFocusWindow;
        if (vr_ready() && pPresentationParameters)
        {
            // A desktop mirror of the side-by-side image, sized to the monitor.
            UINT w = 0, h = 0;
            window_choose_size(target, pPresentationParameters->BackBufferWidth,
                               pPresentationParameters->BackBufferHeight, &w, &h);
            window_configure(target, w, h);
            vr_submit_init(device);        // the real device, not the wrapper
            hud_device_created(device, pPresentationParameters->BackBufferWidth,
                               pPresentationParameters->BackBufferHeight);
        }
        else if (config().force_windowed && pPresentationParameters)
        {
            window_configure(target,
                             pPresentationParameters->BackBufferWidth,
                             pPresentationParameters->BackBufferHeight);
        }
        log_startup_summary(pPresentationParameters);
        return hr;
    }
}
