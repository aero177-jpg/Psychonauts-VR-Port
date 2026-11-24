#pragma once
// A performance and memory line in psyvr.log every five seconds.
//
// The first headset run (Quest 3 over Virtual Desktop, 3072x2612 an eye)
// held 90 fps for two and a half minutes, then fell to 10 within a few
// seconds with the same number of draws. That shape is a resource running
// out, and the two candidates are GPU memory (an 8 GB laptop GPU, also
// encoding the stream) and the 32-bit game's 2 GB address space (Psychonauts.exe is
// not large-address-aware). This line says which.

namespace psyvr
{
    // From vr_submit_init, with the Vulkan handles DXVK gave us.
    void perf_log_set_vulkan(void* instance, void* physical_device);

    // Once per submitted frame.
    void perf_log_frame();

    // Where the frame's time goes (2026-10-19: choppy after a few minutes
    // with flat memory and xrWaitFrame at 0, so the game or the GPU is the
    // bottleneck, and which one is not yet known). Stamps taken in
    // ProxySwapChain::Present, in this order. The gaps are: game (last
    // Present returned to this one: the game's update and its draw calls,
    // plus DXVK's draw-call recording), hud (HUD and overlay), submit,
    // wait (xrWaitFrame), mirror (desktop overlay) and present (the real
    // Present, which blocks if DXVK's queue is full, i.e. GPU-bound).
    enum PerfMark
    {
        PerfEnter,
        PerfBeforeSubmit,
        PerfAfterSubmit,
        PerfAfterWait,
        PerfBeforePresent,
        PerfAfterPresent,
    };
    void perf_log_mark(PerfMark mark);
}
