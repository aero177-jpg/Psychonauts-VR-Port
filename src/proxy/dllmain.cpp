// d3d9.dll proxy for Psychonauts. NOTE: the comments below (and many in
// stereo/vr_math/hud) are inherited from the Tomb Raider Underworld mod and
// may still describe TRU where the two games behave alike.
//
// Psychonauts.exe has no static d3d9 import: it calls LoadLibrary("d3d9.dll") and
// GetProcAddress("Direct3DCreate9") at run time, and LoadLibrary searches the
// game's own folder first. This file sitting next to Psychonauts.exe is the whole
// injection story.
//
// Nothing here patches game code at DLL load. Psychonauts.exe is wrapped by SteamStub,
// which decrypts .text only after this DLL is loaded; anything that touches
// game code waits until Direct3DCreate9 (the Underworld mod's docs/AUDIT.md, section 5).

#include "backend.h"
#include "proxy_d3d9.h"
#include "logic_host.h"
#include "control.h"
#include "game_psy.h"
#include "../vr/vr_session.h"
#include "../vr/vr_pad.h"

#include "../common/config.h"
#include "../common/log.h"

#include <cwchar>
#include <d3d9.h>
#include <windows.h>

using namespace psyvr;

namespace
{
    template <typename T>
    T backend_fn(const char* name)
    {
        return reinterpret_cast<T>(backend_proc(name));
    }
}

extern "C"
{
    IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion)
    {
        typedef IDirect3D9*(WINAPI * fn)(UINT);
        fn real = backend_fn<fn>("Direct3DCreate9");
        if (!real)
        {
            log("Direct3DCreate9: backend has no Direct3DCreate9");
            return nullptr;
        }

        // Unmodded: the system d3d9's own object, unwrapped and untouched.
        if (mod_disabled())
            return real(SDKVersion);

        // The first frame is far off, but by now SteamStub has run: this is
        // the earliest point where the game's code is readable.
        log_install_crash_handler();

        // Before the backend, not after: DXVK creates its Vulkan instance
        // inside Direct3DCreate9, and by then it has to know which Vulkan
        // extensions the OpenXR runtime needs. vr_init leaves them where the
        // DXVK fork looks. Only once: the game may create Direct3D twice.
        static bool vr_tried = false;
        if (!vr_tried)
        {
            vr_tried = true;
            vr_init();
            game_write_lua_mod(vr_ready());
            // The game's own screen size = the VR back buffer, before it reads it.
            if (vr_ready())
            {
                unsigned ew = 0, eh = 0;
                vr_recommended_eye_size(&ew, &eh);
                game_sync_display_settings(ew * 2, eh);
            }
            // The draw logic (psyvr_logic.dll), before any device exists.
            // Not from DllMain: LoadLibrary under the loader lock.
            logic_load_initial();
            // The debug control pipe, if enabled (control.h).
            control_start();
        }

        // DXVK reads $DXVK_CONFIG when it creates its factory, after its
        // built-in profile for Psychonauts.exe (a 60 fps cap), so this wins.
        {
            wchar_t existing[512]{};
            const DWORD n = GetEnvironmentVariableW(L"DXVK_CONFIG", existing, 512);
            wchar_t value[640]{};
            swprintf_s(value, L"%s%sd3d9.maxFrameRate = %d", n ? existing : L"", n ? L"; " : L"",
                       config().max_frame_rate);
            if (config().video_memory_mb > 0)
            {
                wchar_t more[64]{};
                swprintf_s(more, L"; d3d9.maxAvailableMemory = %d", config().video_memory_mb);
                wcscat_s(value, more);
            }
            SetEnvironmentVariableW(L"DXVK_CONFIG", value);
            log("dxvk: DXVK_CONFIG = %S", value);
        }

        IDirect3D9* d3d9 = real(SDKVersion);
        if (!d3d9)
        {
            log("Direct3DCreate9: backend returned null (SDKVersion=%u)", SDKVersion);
            return nullptr;
        }

        log("Direct3DCreate9: SDKVersion=%u, wrapped", SDKVersion);
        return new ProxyD3D9(d3d9);
    }

    // The game never asks for this, but overlays do. It is deliberately not
    // wrapped: an Ex device would bypass the proxy, so anything that takes
    // this path is on its own.
    HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D)
    {
        typedef HRESULT(WINAPI * fn)(UINT, IDirect3D9Ex**);
        fn real = backend_fn<fn>("Direct3DCreate9Ex");
        if (!real)
            return E_NOTIMPL;
        log("Direct3DCreate9Ex: passed through unwrapped");
        return real(SDKVersion, ppD3D);
    }

    int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR wszName)
    {
        typedef int(WINAPI * fn)(D3DCOLOR, LPCWSTR);
        fn real = backend_fn<fn>("D3DPERF_BeginEvent");
        return real ? real(col, wszName) : 0;
    }

    int WINAPI D3DPERF_EndEvent(void)
    {
        typedef int(WINAPI * fn)(void);
        fn real = backend_fn<fn>("D3DPERF_EndEvent");
        return real ? real() : 0;
    }

    void WINAPI D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR wszName)
    {
        typedef void(WINAPI * fn)(D3DCOLOR, LPCWSTR);
        fn real = backend_fn<fn>("D3DPERF_SetMarker");
        if (real)
            real(col, wszName);
    }

    void WINAPI D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR wszName)
    {
        typedef void(WINAPI * fn)(D3DCOLOR, LPCWSTR);
        fn real = backend_fn<fn>("D3DPERF_SetRegion");
        if (real)
            real(col, wszName);
    }

    BOOL WINAPI D3DPERF_QueryRepeatFrame(void)
    {
        typedef BOOL(WINAPI * fn)(void);
        fn real = backend_fn<fn>("D3DPERF_QueryRepeatFrame");
        return real ? real() : FALSE;
    }

    void WINAPI D3DPERF_SetOptions(DWORD dwOptions)
    {
        typedef void(WINAPI * fn)(DWORD);
        fn real = backend_fn<fn>("D3DPERF_SetOptions");
        if (real)
            real(dwOptions);
    }

    DWORD WINAPI D3DPERF_GetStatus(void)
    {
        typedef DWORD(WINAPI * fn)(void);
        fn real = backend_fn<fn>("D3DPERF_GetStatus");
        return real ? real() : 0;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(module);
        if (mod_disabled())
            break;                  // unmodded: no config, no log, nothing
        load_config();
        if (config().logging)
        {
            log_open();
            config_flush_warnings();
            wchar_t exe[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            log("host: %S", exe);
            if (config_defaults_added() != 0)
                log("config: added %u missing setting%s to psyvr.ini",
                    config_defaults_added(), config_defaults_added() == 1 ? "" : "s");
            log("config: backend=%S force_windowed=%d window=%dx%d borderless=%d msaa=%d",
                config().backend, config().force_windowed, config().window_width,
                config().window_height, config().window_borderless, config().msaa);
        }
        vr_pad_install();
        break;
    }
    case DLL_PROCESS_DETACH:
        if (mod_disabled())
            break;
        log("unloading");
        control_shutdown();
        logic_unload();
        log_close();
        break;
    }
    return TRUE;
}
