#include "proxy_swapchain.h"
#include "proxy_device.h"
#include "game_psy.h"
#include "overlay.h"
#include "control.h"
#include "../vr/vr_submit.h"
#include "../vr/hud.h"
#include "../vr/perf_log.h"
#include "../vr/xr_runtime.h"

#include "../common/config.h"
#include "../common/log.h"

#include <cstring>

namespace psyvr
{
    HRESULT ProxySwapChain::QueryInterface(REFIID riid, void** ppvObj)
    {
        if (!ppvObj)
            return E_POINTER;

        if (riid == IID_IUnknown || riid == IID_IDirect3DSwapChain9)
        {
            AddRef();
            *ppvObj = this;
            return S_OK;
        }

        HRESULT hr = _chain->QueryInterface(riid, ppvObj);
        if (SUCCEEDED(hr))
        {
            ((IUnknown*)*ppvObj)->Release();
            *ppvObj = nullptr;
            return E_NOINTERFACE;
        }
        return hr;
    }

    namespace
    {
        // [vr] loading_screens for a Present from a thread other than the
        // session's (a loading screen).
        bool loading_frames_to_headset()
        {
            const int mode = config().loading_screens;
            if (mode == 0)
                return false;
            if (mode == 1)
                return true;
            return strstr(xr_runtime_name(), "Simulator") == nullptr;
        }
    }

    HRESULT ProxySwapChain::Present(CONST RECT* pSourceRect, CONST RECT* pDestRect,
                                    HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion,
                                    DWORD dwFlags)
    {
        {
            static DWORD last_tid = 0;
            static unsigned changes = 0;
            const DWORD tid = GetCurrentThreadId();
            if (tid != last_tid && changes++ < 20)
                log("present: now called from thread %lu", tid);
            last_tid = tid;
        }
        perf_log_mark(PerfEnter);
        static_cast<ProxyDevice*>(_device)->frame_boundary();
        game_frame();
        // The thread that made the OpenXR session (the game's main thread)
        // always talks to the runtime. During loading a loader thread
        // presents while holding the game's allocator lock; the simulator's
        // window belongs to the main thread, which then waits on that lock,
        // and its submit deadlocked (2026-10-21, docs/findings.md). Other
        // runtimes get the loading screens too ([vr] loading_screens); one
        // thread at a time, and a frame that finds the other thread inside
        // goes to the monitor only.
        static SRWLOCK submit_lock = SRWLOCK_INIT;
        const bool main_thread = GetCurrentThreadId() == vr_submit_thread();
        bool to_headset = vr_submit_ready() && (main_thread || loading_frames_to_headset());
        if (to_headset && !TryAcquireSRWLockExclusive(&submit_lock))
            to_headset = false;
        if (to_headset && !main_thread)
        {
            static bool told = false;
            if (!told)
            {
                told = true;
                log("present: a loader thread's frame (loading screen) sent to the headset ([vr] loading_screens)");
            }
        }
        if (to_headset)
        {
            IDirect3DSurface9* back = nullptr;
            if (SUCCEEDED(_chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back)) && back)
            {
                IDirect3DDevice9* real = static_cast<ProxyDevice*>(_device)->_device;
                hud_theatre_screen(real, back);
                hud_composite(real, back, false);
                // The settings overlay as a panel in the room, over the HUD.
                overlay_vr_frame(_chain, back);
                // A pending `shot` (control.h): the eyes as the headset gets
                // them, settings panel included.
                if (control_shot_pending())
                    control_shot_capture(real, back);
                perf_log_mark(PerfBeforeSubmit);
                vr_submit_frame(back);
                perf_log_mark(PerfAfterSubmit);
                back->Release();
            }
            // After the frame is in: this blocks until the runtime wants the
            // next one, which paces the game to the headset.
            vr_submit_wait();
            perf_log_mark(PerfAfterWait);
            ReleaseSRWLockExclusive(&submit_lock);
        }
        // The settings overlay on the desktop mirror, drawn after the frame
        // has gone to the headset, so the headset never shows it twice.
        overlay_frame(_chain);
        perf_log_mark(PerfBeforePresent);
        const HRESULT presented = _chain->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
        perf_log_mark(PerfAfterPresent);
        return presented;
    }
}
