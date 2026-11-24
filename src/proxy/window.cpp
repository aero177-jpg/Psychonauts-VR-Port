#include "window.h"
#include "game_psy.h"

#include "../common/config.h"
#include "../common/log.h"

#include <cstring>

namespace psyvr
{
    namespace
    {
        unsigned g_tick = 0;
        bool g_cursor_released = false;
        HWND g_hwnd = nullptr;

        DWORD wanted_style(bool borderless)
        {
            // WS_POPUP is what makes it fill the screen with no frame; drop it.
            return borderless ? (WS_POPUP | WS_VISIBLE)
                              : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        }

        void centre(HWND hwnd, DWORD style, UINT width, UINT height)
        {
            RECT r = { 0, 0, (LONG)width, (LONG)height };
            AdjustWindowRect(&r, style, FALSE);
            const int ww = r.right - r.left;
            const int wh = r.bottom - r.top;

            MONITORINFO mi{ sizeof(mi) };
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            int x = 0, y = 0;
            if (GetMonitorInfoW(mon, &mi))
            {
                const int mw = mi.rcWork.right - mi.rcWork.left;
                const int mh = mi.rcWork.bottom - mi.rcWork.top;
                x = mi.rcWork.left + (mw - ww) / 2;
                y = mi.rcWork.top + (mh - wh) / 2;
                if (x < mi.rcWork.left) x = mi.rcWork.left;
                if (y < mi.rcWork.top)  y = mi.rcWork.top;
            }

            SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, ww, wh,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        }
    }

    void window_choose_size(HWND hwnd, UINT asked_w, UINT asked_h,
                            UINT* out_w, UINT* out_h)
    {
        if (!out_w || !out_h)
            return;

        if (config().window_width > 0 && config().window_height > 0)
        {
            *out_w = (UINT)config().window_width;
            *out_h = (UINT)config().window_height;
            return;
        }

        *out_w = asked_w;
        *out_h = asked_h;
        if (!asked_w || !asked_h)
            return;

        MONITORINFO mi{ sizeof(mi) };
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        if (!GetMonitorInfoW(mon, &mi))
            return;

        const int work_w = mi.rcWork.right - mi.rcWork.left;
        const int work_h = mi.rcWork.bottom - mi.rcWork.top;

        // Two thirds of the work area, capped at 1080p: on a 4K desktop an
        // uncapped window is fullscreen in all but name.
        const double kMaxW = 1920.0, kMaxH = 1080.0;

        double w = work_w * 2.0 / 3.0;
        if (w > kMaxW)
            w = kMaxW;
        double h = w * (double)asked_h / (double)asked_w;

        double max_h = work_h * 2.0 / 3.0;
        if (max_h > kMaxH)
            max_h = kMaxH;
        if (h > max_h)
        {
            h = max_h;
            w = h * (double)asked_w / (double)asked_h;
        }

        *out_w = (UINT)((long)w & ~1L);      // even, some drivers dislike odd
        *out_h = (UINT)((long)h & ~1L);
    }

    void window_configure(HWND hwnd, UINT width, UINT height)
    {
        if (!hwnd || !IsWindow(hwnd))
            return;
        g_hwnd = hwnd;

        const bool borderless = config().window_borderless;
        const DWORD style = wanted_style(borderless);

        DWORD ex = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
        ex &= ~WS_EX_TOPMOST;

        SetWindowLongW(hwnd, GWL_STYLE, (LONG)style);
        SetWindowLongW(hwnd, GWL_EXSTYLE, (LONG)ex);

        // It can arrive minimised, in which case sizing it does nothing visible.
        if (IsIconic(hwnd))
            ShowWindow(hwnd, SW_RESTORE);

        centre(hwnd, style, width, height);
        SetForegroundWindow(hwnd);

        log("window: %ux%u client, %s, topmost cleared",
            width, height, borderless ? "borderless" : "bordered");

    }

    HWND window_handle()
    {
        return g_hwnd;
    }

    void window_block_game_mouse(bool blocked)
    {
        if (blocked)
            ClipCursor(nullptr);    // let the mouse reach the whole window
    }

    void window_tick()
    {
        HWND hwnd = g_hwnd;
        if (!hwnd || !IsWindow(hwnd))
            return;

        // Once every couple of seconds is plenty, and keeps this off the hot path.
        if ((++g_tick % 120) != 0)
            return;

        const DWORD ex = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_TOPMOST)
        {
            SetWindowLongW(hwnd, GWL_EXSTYLE, (LONG)(ex & ~WS_EX_TOPMOST));
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }

        // Left alone, the game's cursor clip follows you out when you alt-tab.
        if (config().release_cursor_unfocused)
        {
            const bool focused = (GetForegroundWindow() == hwnd);
            if (!focused && !g_cursor_released)
            {
                ClipCursor(nullptr);
                g_cursor_released = true;
            }
            else if (focused && g_cursor_released)
            {
                g_cursor_released = false;
            }
        }
    }
}
