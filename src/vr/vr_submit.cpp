#include "vr_submit.h"
#include "vr_session.h"
#include "xr_runtime.h"
#include "perf_log.h"

#include "../common/config.h"
#include "../common/log.h"

// The interop interface our DXVK fork exports (dxvk-psyvr/src/d3d9).
#include "d3d9_vr.h"

namespace psyvr
{
    namespace
    {
        typedef HRESULT(__stdcall* PFN_Direct3DCreateVR9)(IDirect3DDevice9*, IDirect3DVR9**);

        IDirect3DDevice9* g_device = nullptr;
        IDirect3DVR9* g_vr = nullptr;
        bool g_ready = false;
        unsigned g_frames = 0;
        unsigned g_failures = 0;
        bool g_no_frame = false;

        // Where the submit's time goes, ms summed over the frames since the
        // last report: 0 resolve, 1 acquire + wait image, 2 copy, 3 release +
        // xrEndFrame.
        double g_part_ms[4] = {};
        unsigned g_part_frames = 0;
        LARGE_INTEGER g_qpf{};
        double ms_since(const LARGE_INTEGER& t0)
        {
            LARGE_INTEGER t1;
            QueryPerformanceCounter(&t1);
            if (!g_qpf.QuadPart)
                QueryPerformanceFrequency(&g_qpf);
            return double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(g_qpf.QuadPart);
        }

        // The game asks for 8x MSAA; the compositor takes single-sampled
        // images, so the back buffer is resolved into this first.
        IDirect3DSurface9* g_resolve = nullptr;
        UINT g_resolve_w = 0, g_resolve_h = 0;

        IDirect3DSurface9* resolved(IDirect3DSurface9* back)
        {
            D3DSURFACE_DESC d{};
            if (!back || FAILED(back->GetDesc(&d)) || d.MultiSampleType == D3DMULTISAMPLE_NONE)
                return back;
            if (g_resolve && (g_resolve_w != d.Width || g_resolve_h != d.Height))
                vr_submit_release_targets();
            if (!g_resolve)
            {
                const HRESULT hr = g_device->CreateRenderTarget(d.Width, d.Height, d.Format,
                    D3DMULTISAMPLE_NONE, 0, FALSE, &g_resolve, nullptr);
                if (FAILED(hr) || !g_resolve)
                {
                    g_resolve = nullptr;
                    if (g_failures++ == 0)
                        log("submit: could not make the MSAA resolve target %ux%u (0x%08lX)",
                            d.Width, d.Height, hr);
                    return nullptr;
                }
                g_resolve_w = d.Width;
                g_resolve_h = d.Height;
                log("submit: back buffer is multisampled (type %d); resolving into a %ux%u copy",
                    (int)d.MultiSampleType, d.Width, d.Height);
            }
            log_set_activity("submit: MSAA resolve StretchRect");
            const bool resolved_ok = SUCCEEDED(g_device->StretchRect(back, nullptr, g_resolve, nullptr, D3DTEXF_NONE));
            log_set_activity(nullptr);
            if (!resolved_ok)
                return nullptr;
            return g_resolve;
        }

        // The locking rule, from the Legend mod: DXVK's submit thread needs
        // the submission queue to reach the GPU, so nothing may wait for the
        // GPU while holding it. The queue is held only round the OpenXR calls
        // that may use the VkQueue themselves; the copy happens between them.
        bool submit(IDirect3DSurface9* surface, bool side_by_side)
        {
            log_set_activity("submit: start");
            g_no_frame = !xr_frame_begun();
            if (g_no_frame)
                return false;

            D3DSURFACE_DESC d{};
            bool ok = SUCCEEDED(surface->GetDesc(&d)) && d.Width && d.Height &&
                      xr_frame_should_render();
            const unsigned w = d.Width, h = d.Height;

            if (ok && !xr_swapchain_matches(w, h))
            {
                g_vr->ReleaseExternalImages();
                g_vr->LockSubmissionQueue();
                ok = xr_swapchain_create(w, h);
                g_vr->UnlockSubmissionQueue();
                if (ok)
                    log("submit: swapchain %ux%u", w, h);
            }

            uint64_t image = 0;
            bool acquired = false;
            if (ok)
            {
                LARGE_INTEGER t_acq;
                QueryPerformanceCounter(&t_acq);
                // Only the acquire holds DXVK's queue: OpenXR's Vulkan
                // binding lists acquire, release, xrBeginFrame and
                // xrEndFrame as the calls that may use it. The wait for the
                // image can take most of a frame, and holding the queue
                // through it kept the GPU idle while the game's draws were
                // ready (CPU and GPU took turns instead of overlapping).
                log_set_activity("submit: lock queue + xr acquire image");
                g_vr->LockSubmissionQueue();
                acquired = ok = xr_swapchain_acquire();
                g_vr->UnlockSubmissionQueue();
                log_set_activity("submit: xr wait image");
                ok = ok && xr_swapchain_wait(&image);
                g_part_ms[1] += ms_since(t_acq);
            }
            if (ok)
            {
                D3D9_VR_EXTERNAL_IMAGE dst{};
                dst.Image = image;
                dst.Format = (VkFormat)xr_swapchain_format();
                dst.Width = w;
                dst.Height = h;
                LARGE_INTEGER t_copy;
                QueryPerformanceCounter(&t_copy);
                log_set_activity("DXVK CopySurfaceToImage");
                ok = SUCCEEDED(g_vr->CopySurfaceToImage(surface, &dst, 0, 0, FALSE));
                g_part_ms[2] += ms_since(t_copy);
                log_set_activity(nullptr);
                if (!ok && g_failures++ == 0)
                    log("submit: DXVK could not copy the %ux%u frame into the swapchain", w, h);
            }

            XrRect left{ 0, 0, (int)w, (int)h };
            XrRect right = left;
            if (side_by_side)
            {
                left.width = right.width = (int)(w / 2);
                right.x = left.width;
            }

            // Always ended, with no layer if anything failed: a frame begun
            // and never ended stalls the session.
            LARGE_INTEGER t_end;
            QueryPerformanceCounter(&t_end);
            log_set_activity("submit: lock queue + xr release/end frame");
            g_vr->LockSubmissionQueue();
            if (acquired)
                xr_swapchain_release();
            const bool ended = xr_frame_end(ok ? &left : nullptr, ok ? &right : nullptr);
            g_vr->UnlockSubmissionQueue();
            g_part_ms[3] += ms_since(t_end);
            log_set_activity(nullptr);
            return ok && ended;
        }
    }

    bool vr_submit_ready()
    {
        return g_ready;
    }

    namespace { DWORD g_thread = 0; }

    DWORD vr_submit_thread()
    {
        return g_thread;
    }

    void vr_submit_release_targets()
    {
        if (g_resolve)
        {
            g_resolve->Release();
            g_resolve = nullptr;
        }
    }

    bool vr_submit_init(IDirect3DDevice9* real_device)
    {
        if (g_ready && real_device != g_device)
        {
            log("submit: the game created a new device; rebinding");
            vr_submit_release_targets();
            if (g_vr)
                g_vr->Release();
            g_vr = nullptr;
            g_ready = false;
        }
        if (g_ready)
            return true;
        if (!real_device || !vr_ready())
            return false;

        // Direct3DCreateVR9 only exists in our DXVK fork.
        HMODULE backend = GetModuleHandleW(L"d3d9_dxvk.dll");
        if (!backend)
        {
            log("submit: d3d9_dxvk.dll is not loaded (backend = dxvk needed) -- flat");
            return false;
        }
        auto create = (PFN_Direct3DCreateVR9)GetProcAddress(backend, "Direct3DCreateVR9");
        if (!create)
        {
            log("submit: this d3d9_dxvk.dll has no Direct3DCreateVR9 -- not our fork");
            return false;
        }
        IDirect3DVR9* created = nullptr;
        HRESULT hr = create(real_device, &created);
        if (FAILED(hr) || !created)
        {
            log("submit: Direct3DCreateVR9 failed (0x%08lX)", hr);
            return false;
        }
        // The interface grew for OpenXR and its IID changed with it, so an
        // older fork is refused rather than called through a short vtable.
        IDirect3DVR9* checked = nullptr;
        if (FAILED(created->QueryInterface(__uuidof(IDirect3DVR9), (void**)&checked)) || !checked)
        {
            created->Release();
            log("submit: d3d9_dxvk.dll predates the OpenXR interop");
            return false;
        }
        checked->Release();

        D3D9_VR_DEVICE_DESC desc{};
        if (FAILED(created->GetDeviceDesc(&desc)) || !desc.Instance || !desc.Device)
        {
            created->Release();
            log("submit: DXVK did not describe its Vulkan device");
            return false;
        }
        XrVulkanHandles vk{};
        vk.instance = desc.Instance;
        vk.physical_device = desc.PhysicalDevice;
        vk.device = desc.Device;
        vk.queue_family = desc.QueueFamilyIndex;
        vk.queue_index = desc.QueueIndex;
        created->LockSubmissionQueue();
        const bool session = xr_session_create(vk);
        created->UnlockSubmissionQueue();
        if (!session)
        {
            created->Release();
            log("submit: no OpenXR session (%s) -- flat", xr_last_error());
            return false;
        }

        g_device = real_device;
        g_vr = created;
        g_ready = true;
        g_thread = GetCurrentThreadId();
        log("submit: Vulkan interop and OpenXR session up");
        perf_log_set_vulkan(desc.Instance, desc.PhysicalDevice);
        return true;
    }

    void vr_submit_device_closing(IDirect3DDevice9* real_device)
    {
        if (!g_vr || real_device != g_device)
            return;
        log("submit: the device is going; ending the OpenXR session");
        vr_submit_release_targets();
        if (xr_session_exists())
        {
            g_vr->ReleaseExternalImages();
            g_vr->LockSubmissionQueue();
            xr_session_destroy();
            g_vr->UnlockSubmissionQueue();
        }
        g_vr->Release();
        g_vr = nullptr;
        g_device = nullptr;
        g_ready = false;
    }

    void vr_submit_frame(IDirect3DSurface9* back_buffer)
    {
        if (!g_ready || !back_buffer)
            return;
        perf_log_frame();
        LARGE_INTEGER t_res;
        QueryPerformanceCounter(&t_res);
        IDirect3DSurface9* surface = resolved(back_buffer);
        g_part_ms[0] += ms_since(t_res);
        if (++g_part_frames == 300)
        {
            log("submit parts (ms/frame avg over 300): resolve %.2f, acquire+wait image %.2f, copy %.2f, release+xrEndFrame %.2f",
                g_part_ms[0] / 300, g_part_ms[1] / 300, g_part_ms[2] / 300, g_part_ms[3] / 300);
            g_part_frames = 0;
            for (double& p : g_part_ms)
                p = 0.0;
        }
        const bool ok = surface && submit(surface, true);
        if (!ok)
        {
            if (!g_no_frame && g_failures++ == 0)
                log("submit: the runtime did not take the frame -- reporting once");
            return;
        }
        if (++g_frames == 1)
            log("submit: first frame accepted (side by side)");
        else if ((g_frames % 5400) == 0)
            log("submit: %u frames", g_frames);
    }

    void vr_submit_wait()
    {
        if (!g_ready)
            return;
        // A frame the submit path did not end is ended empty before the
        // next one begins.
        if (xr_frame_begun())
        {
            g_vr->LockSubmissionQueue();
            xr_frame_end(nullptr, nullptr);
            g_vr->UnlockSubmissionQueue();
        }
        XrFrame frame{};
        if (xr_frame_wait(&frame))
            vr_set_frame(frame);
    }
}
