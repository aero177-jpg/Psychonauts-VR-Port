#pragma once
// A debug control channel: a named pipe (\\.\pipe\psyvr) that lets a test
// script drive the running game (tools\README.md, tools\drive.ps1
// Psy-Cmd). Only with [debug] control_pipe = 1, read at start.
//
// The client sends one command line, the shim answers with lines ending in a
// line holding just ".". The pipe thread only parses text and waits: it never
// touches game memory or D3D. It queues the command for the thread that may
// run it and waits up to 10 s for the answer ("error timeout" otherwise):
//   frame commands   the render thread, at the frame boundary
//   game commands    the game thread, inside the camera hook
// Every command and the first line of its answer go to psyvr.log.

#include <d3d9.h>

namespace psyvr
{
    // Once, after the config is loaded; does nothing unless enabled.
    void control_start();
    // Process detach: stops the pipe thread from answering. Does not wait
    // for it (it may be blocked in ConnectNamedPipe).
    void control_shutdown();

    // From ProxyDevice::frame_boundary (render thread), after
    // logic_poll_frame_boundary.
    void control_frame_boundary();
    // From ProxyDevice::frame_boundary, after drawlog_frame_boundary: answers
    // a `drawlog` command once its frame has been written.
    void control_drawlog_done();
    // From the camera hook (game thread), right after the game's own camera
    // update.
    void control_game_update();

    // `shot`: true while a screenshot waits for the end of its frame. From
    // ProxySwapChain::Present, after the HUD is composed and before the frame
    // goes to the headset, `control_shot_capture` writes the eyes' images.
    bool control_shot_pending();
    void control_shot_capture(IDirect3DDevice9* device, IDirect3DSurface9* back);
}
