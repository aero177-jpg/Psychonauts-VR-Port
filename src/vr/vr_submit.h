#pragma once
// Getting finished frames to the headset.
//
// The back buffer holds both eyes side by side (stereo.h). At each swap
// chain Present it is resolved (the game asks for MSAA) and copied by DXVK
// into the OpenXR swapchain, then the next frame is waited for, which paces
// the game to the headset.

#include <d3d9.h>

namespace psyvr
{
    // After CreateDevice, with the real (unwrapped) device.
    bool vr_submit_init(IDirect3DDevice9* real_device);
    bool vr_submit_ready();
    // The thread vr_submit_init ran on (it made the OpenXR session); only it
    // submits frames and waits on the runtime.
    DWORD vr_submit_thread();

    // From the swap chain Present, before the real Present.
    void vr_submit_frame(IDirect3DSurface9* back_buffer);
    void vr_submit_wait();

    // D3DPOOL_DEFAULT resources go before a Reset.
    void vr_submit_release_targets();
    // The session lives on this device's VkDevice, so it ends first.
    void vr_submit_device_closing(IDirect3DDevice9* real_device);
}
