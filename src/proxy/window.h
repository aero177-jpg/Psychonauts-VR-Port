#pragma once
// Making the game's window behave like a window.
//
// Setting D3DPRESENT_PARAMETERS::Windowed gives a windowed *device*. It does
// nothing to the window, which the game creates as a borderless popup over
// the whole screen. This gives it a frame, sizes and centres it, clears
// topmost, and fixes the mouse, which the game handles as if its client area
// were the screen (the same fixes chreden/trawindowed makes for this game).

#include <windows.h>

namespace psyvr
{
    // Pick a client size: whatever the config pins, or about two thirds of the
    // monitor (capped at 1080p) keeping the aspect the game asked for.
    void window_choose_size(HWND hwnd, UINT asked_w, UINT asked_h,
                            UINT* out_w, UINT* out_h);

    // Restyle and reposition to the given client size. The window is
    // remembered, so window_tick needs no argument.
    void window_configure(HWND hwnd, UINT width, UINT height);

    // Called every frame; real work only every couple of seconds. Undoes a
    // re-applied topmost and frees the cursor when the game is not in front.
    void window_tick();

    // While the settings overlay is open: releases the cursor clip, so the
    // mouse reaches the whole window.
    void window_block_game_mouse(bool blocked);

    // The game window once window_configure has seen it, else null.
    HWND window_handle();
}
