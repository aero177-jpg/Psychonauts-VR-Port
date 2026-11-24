#include "proxy_device.h"
#include "msaa_cap.h"
#include "proxy_swapchain.h"
#include "window.h"
#include "overlay.h"
#include "logic_host.h"
#include "control.h"
#include "../vr/stereo.h"
#include "../vr/vr_session.h"
#include "../vr/vr_submit.h"
#include "../vr/hud.h"
#include "vr_present_size.h"
#include "trace.h"
#include "game_psy.h"
#include "rt_scale.h"

#include "../common/config.h"
#include "../common/log.h"

#include <unordered_map>

namespace psyvr
{
    // Larger water reflection targets (rt_scale.h).
    namespace
    {
        SRWLOCK g_rt_scale_lock = SRWLOCK_INIT;
        std::unordered_map<const void*, unsigned> g_rt_scaled;     // texture -> factor
        unsigned g_rt_scale_now = 1;

        // The water's reflection targets: 4 times the game's (2048 x 2048,
        // about 40 MB; was [vr] water_reflection_scale, its default).
        unsigned rt_scale_factor()
        {
            return stereo_active() ? 4u : 1u;
        }

        // The glow's last target: the half-size glow target's width (half
        // the back buffer) over 320, rounded up, at most 8 (was [vr]
        // glow_detail's automatic default).
        unsigned glow_scale_factor(UINT back_width)
        {
            if (!stereo_active() || !back_width)
                return 1;
            const unsigned f = (back_width / 2 + 319) / 320;
            return f < 1 ? 1u : f > 8 ? 8u : f;
        }

        bool is_glow_size(UINT width, UINT height)
        {
            return width == 320 && height == 240;
        }
    }

    unsigned rt_scale_for_texture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
                                  UINT back_width)
    {
        if (!(usage & D3DUSAGE_RENDERTARGET) || levels != 1 ||
            (format != D3DFMT_A8R8G8B8 && format != D3DFMT_X8R8G8B8))
            return 1;
        if (is_glow_size(width, height))
            return glow_scale_factor(back_width);
        if (width != height || (width != 512 && width != 256))
            return 1;
        return rt_scale_factor();
    }

    unsigned rt_scale_for_depth(UINT width, UINT height, UINT back_width)
    {
        // A depth buffer may be larger than its target, so this is safe for
        // any other target of these sizes.
        if (is_glow_size(width, height))
            return glow_scale_factor(back_width);
        return width == height && (width == 512 || width == 256) ? rt_scale_factor() : 1u;
    }

    namespace
    {
        UINT back_buffer_width(IDirect3DDevice9* device)
        {
            IDirect3DSurface9* bb = nullptr;
            D3DSURFACE_DESC d{};
            if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
                return 0;
            const bool ok = SUCCEEDED(bb->GetDesc(&d));
            bb->Release();
            return ok ? d.Width : 0;
        }
    }

    void rt_scale_note_texture(IDirect3DTexture9* texture, unsigned scale)
    {
        if (!texture)
            return;
        AcquireSRWLockExclusive(&g_rt_scale_lock);
        if (scale > 1)
            g_rt_scaled[texture] = scale;
        else
            g_rt_scaled.erase(texture);     // a new texture where an old one was
        ReleaseSRWLockExclusive(&g_rt_scale_lock);
    }

    unsigned rt_scale_of(IDirect3DSurface9* surface)
    {
        if (!surface)
            return 1;
        AcquireSRWLockShared(&g_rt_scale_lock);
        const bool any = !g_rt_scaled.empty();
        ReleaseSRWLockShared(&g_rt_scale_lock);
        if (!any)
            return 1;
        IDirect3DTexture9* tex = nullptr;
        if (FAILED(surface->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&tex))) || !tex)
            return 1;
        AcquireSRWLockShared(&g_rt_scale_lock);
        const auto it = g_rt_scaled.find(tex);
        const unsigned f = it != g_rt_scaled.end() ? it->second : 1u;
        ReleaseSRWLockShared(&g_rt_scale_lock);
        tex->Release();
        return f;
    }

    void rt_scale_note_target(IDirect3DSurface9* rt0)
    {
        g_rt_scale_now = rt_scale_of(rt0);
    }

    unsigned rt_scale_current()
    {
        return g_rt_scale_now;
    }

    // Fading objects, the shim's side (stereo.h, plans 6..8; how they are
    // recognised: stereo.cpp, g_fade_rt). One stencil bit of the back
    // buffer's depth-stencil marks where the object is; the composite is
    // tested against it and clears it again, so the game never sees it set
    // (it clears the stencil at each frame start anyway).
    namespace
    {
        // The back buffer's depth-stencil, learnt at a composite (AddRef'd,
        // dropped before Reset). Until then nothing is marked.
        IDirect3DSurface9* g_fade_ds = nullptr;
        // Every object since the clear was marked into g_fade_ds.
        bool g_fade_marks_ok = false;
        const DWORD kFadeBit = 0x80;
        const D3DRENDERSTATETYPE kFadeStates[] = {
            D3DRS_COLORWRITEENABLE, D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_STENCILENABLE,
            D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK,
            D3DRS_STENCILPASS, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_TWOSIDEDSTENCILMODE,
        };
        const unsigned kFadeStateCount = sizeof(kFadeStates) / sizeof(kFadeStates[0]);
        DWORD g_fade_saved[kFadeStateCount] = {};
        DWORD g_fade_saved_scissor_on = FALSE;
        RECT g_fade_saved_scissor{};
        IDirect3DSurface9* g_fade_saved_rt = nullptr;
        IDirect3DSurface9* g_fade_saved_ds = nullptr;
        D3DVIEWPORT9 g_fade_saved_vp{};

        void fade_save_states(IDirect3DDevice9* device)
        {
            for (unsigned i = 0; i < kFadeStateCount; ++i)
                device->GetRenderState(kFadeStates[i], &g_fade_saved[i]);
        }

        void fade_restore_states(IDirect3DDevice9* device)
        {
            for (unsigned i = 0; i < kFadeStateCount; ++i)
                device->SetRenderState(kFadeStates[i], g_fade_saved[i]);
        }

        void fade_set_stencil(IDirect3DDevice9* device, DWORD func, DWORD pass)
        {
            device->SetRenderState(D3DRS_STENCILENABLE, TRUE);
            device->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
            device->SetRenderState(D3DRS_STENCILFUNC, func);
            device->SetRenderState(D3DRS_STENCILREF, kFadeBit);
            device->SetRenderState(D3DRS_STENCILMASK, kFadeBit);
            device->SetRenderState(D3DRS_STENCILWRITEMASK, kFadeBit);
            device->SetRenderState(D3DRS_STENCILPASS, pass);
            device->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
            device->SetRenderState(D3DRS_STENCILZFAIL, pass);
        }
    }

    void stereo_fade_clear(IDirect3DDevice9* device)
    {
        // What the game's triangle drew: colour 0 over the whole target,
        // depth untouched (its triangle had z off).
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
        g_fade_marks_ok = g_fade_ds != nullptr;
    }

    bool stereo_fade_mark_begin(IDirect3DDevice9* device)
    {
        if (!g_fade_marks_ok)
            return false;
        IDirect3DSurface9* bb = nullptr;
        if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
        {
            g_fade_marks_ok = false;
            return false;
        }
        g_fade_saved_rt = nullptr;
        g_fade_saved_ds = nullptr;
        device->GetRenderTarget(0, &g_fade_saved_rt);
        device->GetDepthStencilSurface(&g_fade_saved_ds);
        device->GetViewport(&g_fade_saved_vp);
        fade_save_states(device);
        device->SetRenderTarget(0, bb);
        bb->Release();
        device->SetDepthStencilSurface(g_fade_ds);
        // The whole outline, whatever is in front of it: a pixel the target
        // left at 0 changes nothing in the composite anyway.
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        fade_set_stencil(device, D3DCMP_ALWAYS, D3DSTENCILOP_REPLACE);
        return true;
    }

    void stereo_fade_mark_end(IDirect3DDevice9* device)
    {
        device->SetRenderTarget(0, g_fade_saved_rt);
        device->SetDepthStencilSurface(g_fade_saved_ds);
        device->SetViewport(&g_fade_saved_vp);
        fade_restore_states(device);
        if (g_fade_saved_rt)
            g_fade_saved_rt->Release();
        if (g_fade_saved_ds)
            g_fade_saved_ds->Release();
        g_fade_saved_rt = g_fade_saved_ds = nullptr;
    }

    bool stereo_fade_composite_begin(IDirect3DDevice9* device)
    {
        IDirect3DSurface9* ds = nullptr;
        device->GetDepthStencilSurface(&ds);
        D3DSURFACE_DESC d{};
        const bool stencil = ds && SUCCEEDED(ds->GetDesc(&d)) &&
                             (d.Format == D3DFMT_D24S8 || d.Format == D3DFMT_D24FS8 || d.Format == D3DFMT_D24X4S4);
        const bool use = g_fade_marks_ok && ds && ds == g_fade_ds;
        g_fade_marks_ok = false;
        fade_save_states(device);
        device->GetRenderState(D3DRS_SCISSORTESTENABLE, &g_fade_saved_scissor_on);
        device->GetScissorRect(&g_fade_saved_scissor);
        if (ds != g_fade_ds)
        {
            // Learnt here, for the objects after this one.
            if (g_fade_ds)
                g_fade_ds->Release();
            g_fade_ds = stencil ? ds : nullptr;
            if (!stencil && ds)
                ds->Release();
            if (g_fade_ds)
                log("fade: back buffer depth-stencil %p (%ux%u, format %d) used to mark fading objects",
                    (void*)g_fade_ds, d.Width, d.Height, (int)d.Format);
        }
        else if (ds)
            ds->Release();
        if (!use)
            return false;
        fade_set_stencil(device, D3DCMP_EQUAL, D3DSTENCILOP_ZERO);
        return true;
    }

    void stereo_fade_composite_end(IDirect3DDevice9* device)
    {
        fade_restore_states(device);
        device->SetScissorRect(&g_fade_saved_scissor);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, g_fade_saved_scissor_on);
    }

    void stereo_fade_release()
    {
        if (g_fade_ds)
            g_fade_ds->Release();
        g_fade_ds = nullptr;
        g_fade_marks_ok = false;
    }

    HRESULT ProxyDevice::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format,
                                       D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle)
    {
        const unsigned f = rt_scale_for_texture(Width, Height, Levels, Usage, Format,
                                                (Usage & D3DUSAGE_RENDERTARGET) ? back_buffer_width(_device) : 0);
        HRESULT hr = _device->CreateTexture(Width * f, Height * f, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
        unsigned used = f;
        if (FAILED(hr) && f > 1)
        {
            used = 1;
            hr = _device->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
        }
        if (SUCCEEDED(hr) && ppTexture && *ppTexture)
        {
            rt_scale_note_texture(*ppTexture, used);
            if (used > 1)
            {
                static unsigned told = 0;
                if (told++ < 6)
                    log("rt scale: %ux%u render target created %ux%u (%s)", Width, Height, Width * used,
                        Height * used, is_glow_size(Width, Height) ? "glow" : "water reflection");
            }
        }
        return hr;
    }

    HRESULT ProxyDevice::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                                   D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
                                                   BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
    {
        msaa_clamp(&MultiSample, &MultisampleQuality);
        const unsigned f = rt_scale_for_depth(Width, Height, back_buffer_width(_device));
        HRESULT hr = _device->CreateDepthStencilSurface(Width * f, Height * f, Format, MultiSample, MultisampleQuality,
                                                        Discard, ppSurface, pSharedHandle);
        if (FAILED(hr) && f > 1)
            hr = _device->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard,
                                                    ppSurface, pSharedHandle);
        return hr;
    }

    HRESULT ProxyDevice::SetViewport(CONST D3DVIEWPORT9* pViewport)
    {
        if (!pViewport)
            return _device->SetViewport(pViewport);
        stereo_note_viewport(*pViewport);
        g_dls.viewport = *pViewport;
        g_dls.viewport_valid = true;
        trace("viewport %lu,%lu %lux%lu z %.2f-%.2f", pViewport->X, pViewport->Y, pViewport->Width, pViewport->Height,
              pViewport->MinZ, pViewport->MaxZ);
        const unsigned f = rt_scale_current();
        if (f <= 1)
            return _device->SetViewport(pViewport);
        D3DVIEWPORT9 vp = *pViewport;
        vp.X *= f;
        vp.Y *= f;
        vp.Width *= f;
        vp.Height *= f;
        return _device->SetViewport(&vp);
    }

    HRESULT ProxyDevice::GetViewport(D3DVIEWPORT9* pViewport)
    {
        const HRESULT hr = _device->GetViewport(pViewport);
        const unsigned f = rt_scale_current();
        if (SUCCEEDED(hr) && pViewport && f > 1)
        {
            pViewport->X /= f;
            pViewport->Y /= f;
            pViewport->Width /= f;
            pViewport->Height /= f;
        }
        return hr;
    }

    HRESULT ProxyDevice::StretchRect(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect,
                                     IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter)
    {
        if (trace_active())
        {
            char a[128], b[128];
            describe(pSourceSurface, a, sizeof(a));
            describe(pDestSurface, b, sizeof(b));
            trace("stretch %s -> %s rects %d/%d", a, b, pSourceRect != nullptr, pDestRect != nullptr);
        }
        RECT src, dst;
        const unsigned fs = pSourceRect ? rt_scale_of(pSourceSurface) : 1u;
        const unsigned fd = pDestRect ? rt_scale_of(pDestSurface) : 1u;
        if (fs > 1)
        {
            src = { pSourceRect->left * (LONG)fs, pSourceRect->top * (LONG)fs, pSourceRect->right * (LONG)fs,
                    pSourceRect->bottom * (LONG)fs };
            pSourceRect = &src;
        }
        if (fd > 1)
        {
            dst = { pDestRect->left * (LONG)fd, pDestRect->top * (LONG)fd, pDestRect->right * (LONG)fd,
                    pDestRect->bottom * (LONG)fd };
            pDestRect = &dst;
        }
        return _device->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);
    }

    HRESULT ProxyDevice::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain)
    {
        if (!pSwapChain)
            return D3DERR_INVALIDCALL;

        IDirect3DSwapChain9* chain = nullptr;
        const HRESULT hr = _device->GetSwapChain(iSwapChain, &chain);
        if (FAILED(hr) || !chain)
        {
            *pSwapChain = nullptr;
            return hr;
        }
        *pSwapChain = new ProxySwapChain(chain, this);
        return hr;
    }

    HRESULT ProxyDevice::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                   IDirect3DSwapChain9** pSwapChain)
    {
        if (!pSwapChain)
            return D3DERR_INVALIDCALL;

        IDirect3DSwapChain9* chain = nullptr;
        const HRESULT hr = _device->CreateAdditionalSwapChain(pPresentationParameters, &chain);
        if (FAILED(hr) || !chain)
        {
            *pSwapChain = nullptr;
            return hr;
        }
        log("device: CreateAdditionalSwapChain wrapped");
        *pSwapChain = new ProxySwapChain(chain, this);
        return hr;
    }

    HRESULT ProxyDevice::QueryInterface(REFIID riid, void** ppvObj)
    {
        if (!ppvObj)
            return E_POINTER;

        if (riid == IID_IUnknown || riid == IID_IDirect3DDevice9)
        {
            AddRef();
            *ppvObj = this;
            return S_OK;
        }

        // Anything else -- IDirect3DDevice9Ex in particular -- we do not wrap.
        // Handing back the real object for those would let a caller drive the
        // device behind our back, so refuse instead of lying.
        HRESULT hr = _device->QueryInterface(riid, ppvObj);
        if (SUCCEEDED(hr))
        {
            ((IUnknown*)*ppvObj)->Release();
            *ppvObj = nullptr;
            log("device: refused QueryInterface for an interface we do not wrap");
            return E_NOINTERFACE;
        }
        return hr;
    }

    // Psychonauts presents through the device. The frame work (HUD
    // composite, VR submit, frame pacing) lives in ProxySwapChain::Present,
    // so the implicit swap chain's wrapper does it here too; that Present is
    // equivalent to this one.
    HRESULT ProxyDevice::Present(CONST RECT* pSourceRect, CONST RECT* pDestRect,
                                 HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion)
    {
        IDirect3DSwapChain9* chain = nullptr;
        if (SUCCEEDED(GetSwapChain(0, &chain)) && chain)
        {
            const HRESULT hr = chain->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, 0);
            chain->Release();
            return hr;
        }
        frame_boundary();
        return _device->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    }

    namespace
    {
        // A reload.flag file beside the game re-reads psyvr.ini and applies
        // the live-safe settings, without a restart (like trace.flag).
        void reload_if_flagged()
        {
            static unsigned frames = 0;
            ++frames;
            // The settings overlay asks for a reload after saving a change.
            bool reload = config_take_reload_request();
            if (!reload && (frames % 30) == 0)
            {
                wchar_t path[MAX_PATH]{};
                swprintf_s(path, L"%sreload.flag", exe_dir());
                if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
                {
                    DeleteFileW(path);
                    reload = true;
                }
            }
            if (!reload)
                return;
            config_reload_live();
        }
    }

    namespace
    {
        // Stream 0's POSITION offset (FLOAT3 or FLOAT4) in the current
        // vertex declaration; -1 = none.
        int position_offset(IDirect3DDevice9* device)
        {
            IDirect3DVertexDeclaration9* decl = nullptr;
            if (FAILED(device->GetVertexDeclaration(&decl)) || !decl)
                return -1;
            D3DVERTEXELEMENT9 elems[MAXD3DDECLLENGTH + 1];
            UINT count = 0;
            int offset = -1;
            if (SUCCEEDED(decl->GetDeclaration(elems, &count)))
                for (UINT i = 0; i < count && elems[i].Stream != 0xFF; ++i)
                    if (elems[i].Stream == 0 && elems[i].Usage == D3DDECLUSAGE_POSITION &&
                        (elems[i].Type == D3DDECLTYPE_FLOAT3 || elems[i].Type == D3DDECLTYPE_FLOAT4))
                        offset = elems[i].Offset;
            decl->Release();
            return offset;
        }

        // memcpy that survives a bad pointer (the game's own vertex/index
        // data of a *UP draw).
        bool safe_copy(void* dst, const void* src, size_t n)
        {
            __try
            {
                memcpy(dst, src, n);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // box = (min x, min y, max x, max y, min z, max z) (stereo.h).
        void box_add(float box[6], const float* p, bool* any)
        {
            if (!*any)
            {
                box[0] = box[2] = p[0];
                box[1] = box[3] = p[1];
                box[4] = box[5] = p[2];
                *any = true;
                return;
            }
            box[0] = p[0] < box[0] ? p[0] : box[0];
            box[1] = p[1] < box[1] ? p[1] : box[1];
            box[2] = p[0] > box[2] ? p[0] : box[2];
            box[3] = p[1] > box[3] ? p[1] : box[3];
            box[4] = p[2] < box[4] ? p[2] : box[4];
            box[5] = p[2] > box[5] ? p[2] : box[5];
        }
    }

    // Before a draw whose plan wants it (only the lock-on reticle: one draw
    // a frame while locked): the x / y bounds of stream 0's POSITION over
    // `count` vertices from `first` (absolute), from the game's own array
    // `up` or the stream's buffer. Any failure hands the plan null.
    void ProxyDevice::note_vertex_box(INT first, UINT count, const void* up, UINT up_stride)
    {
        float box[6] = {};
        bool any = false;
        const int offset = first >= 0 && count > 0 && count <= 4096 ? position_offset(_device) : -1;
        if (offset >= 0 && up)
        {
            for (UINT i = 0; i < count; ++i)
            {
                float p[3];
                if (!safe_copy(p, static_cast<const BYTE*>(up) + (size_t)(first + (INT)i) * up_stride + offset,
                               sizeof(p)))
                {
                    any = false;
                    break;
                }
                box_add(box, p, &any);
            }
        }
        else if (offset >= 0)
        {
            IDirect3DVertexBuffer9* vb = nullptr;
            UINT base = 0, stride = 0;
            if (SUCCEEDED(_device->GetStreamSource(0, &vb, &base, &stride)) && vb && stride)
            {
                // Dynamic buffers: NOOVERWRITE so the lock never waits for the
                // GPU (the game wrote these vertices this frame).
                D3DVERTEXBUFFER_DESC desc{};
                DWORD flags = D3DLOCK_READONLY;
                if (SUCCEEDED(vb->GetDesc(&desc)) && (desc.Usage & D3DUSAGE_DYNAMIC))
                    flags |= D3DLOCK_NOOVERWRITE;
                void* data = nullptr;
                const UINT start = base + (UINT)first * stride;
                if (SUCCEEDED(vb->Lock(start, count * stride, &data, flags)) && data)
                {
                    for (UINT i = 0; i < count; ++i)
                    {
                        float p[3];
                        if (!safe_copy(p, static_cast<const BYTE*>(data) + (size_t)i * stride + offset, sizeof(p)))
                        {
                            any = false;
                            break;
                        }
                        box_add(box, p, &any);
                    }
                    vb->Unlock();
                }
                vb->Release();
            }
        }
        stereo_note_vertex_box(any ? box : nullptr);
    }

    void ProxyDevice::frame_boundary()
    {
        stereo_new_frame();
        logic_poll_frame_boundary();
        control_frame_boundary();
        drawlog_frame_boundary();
        control_drawlog_done();
        trace_frame_boundary();
        reload_if_flagged();
        if (config().force_windowed)
            window_tick();
        ++_frames;
        if (_frames == 1 || (_frames % 300) == 0)
            log("frame %u: %u draws in the last frame", _frames, _draws_last);
        _draws_last = _draws;
        _draws = 0;
    }

    HRESULT ProxyDevice::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters)
    {
        if (pPresentationParameters)
        {
            msaa_clamp(&pPresentationParameters->MultiSampleType,
                       &pPresentationParameters->MultiSampleQuality);
            if (config().vsync >= 0)
                pPresentationParameters->PresentationInterval =
                    config().vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
            log("device: Reset to %ux%u windowed=%d",
                pPresentationParameters->BackBufferWidth,
                pPresentationParameters->BackBufferHeight,
                pPresentationParameters->Windowed);

            if (vr_ready())
            {
                // As at CreateDevice (proxy_d3d9.cpp): the headset's eye
                // size, both eyes side by side, windowed.
                pPresentationParameters->Windowed = TRUE;
                pPresentationParameters->FullScreen_RefreshRateInHz = 0;
                size_for_vr(pPresentationParameters);
            }
            else if (config().force_windowed)
            {
                if (!pPresentationParameters->Windowed)
                {
                    pPresentationParameters->Windowed = TRUE;
                    pPresentationParameters->FullScreen_RefreshRateInHz = 0;
                }
                UINT w = 0, h = 0;
                window_choose_size(pPresentationParameters->hDeviceWindow,
                                   pPresentationParameters->BackBufferWidth,
                                   pPresentationParameters->BackBufferHeight,
                                   &w, &h);
                if (w && h)
                {
                    pPresentationParameters->BackBufferWidth = w;
                    pPresentationParameters->BackBufferHeight = h;
                }
            }
        }

        vr_submit_release_targets();
        hud_release();
        overlay_before_reset();
        stereo_fade_release();
        const HRESULT hr = _device->Reset(pPresentationParameters);
        drawlog_device_reset();
        if (pPresentationParameters && vr_ready())
            hud_device_created(_device, pPresentationParameters->BackBufferWidth,
                               pPresentationParameters->BackBufferHeight);
        log("device: Reset -> 0x%08lX", hr);

        // A reset can put the fullscreen styling back, so redo it.
        if (SUCCEEDED(hr) && config().force_windowed && pPresentationParameters && !vr_ready())
        {
            HWND target = pPresentationParameters->hDeviceWindow
                        ? pPresentationParameters->hDeviceWindow
                        : window_handle();
            if (target)
                window_configure(target,
                                 pPresentationParameters->BackBufferWidth,
                                 pPresentationParameters->BackBufferHeight);
        }
        return hr;
    }

    // The water pixel shaders are created replaced (stereo.h,
    // stereo_patch_pixel_shader); the rest as the game gave them.
    HRESULT ProxyDevice::CreatePixelShader(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader)
    {
        static DWORD patched[4096];     // creation is serialised by the device
        size_t length = 0;
        const bool use = stereo_active() && stereo_patch_pixel_shader(pFunction, patched, 4096, &length);
        HRESULT hr = _device->CreatePixelShader(use ? patched : pFunction, ppShader);
        const bool patched_ok = use && SUCCEEDED(hr);
        if (FAILED(hr) && use)
            hr = _device->CreatePixelShader(pFunction, ppShader);   // never worse than unpatched
        else if (use)
        {
            static unsigned told = 0;
            if (told++ < 3)
                log("stereo: created a pixel shader from replaced bytecode");
        }
        if (SUCCEEDED(hr) && ppShader)
        {
            trace_dump_shader('p', *ppShader, pFunction);
            drawlog_shader_created(true, *ppShader, pFunction, patched_ok);
        }
        return hr;
    }

    // The engine's matrix flush (FUN_00526A20) writes c0..c15 here as
    // [World x ViewProj, World, View, ViewProj]; see the Underworld mod's docs/AUDIT.md 9.4.
    // Stereo substitutes per eye at this point in phase 2.
    // The logic may create a shader patched (stereo.h,
    // stereo_patch_vertex_shader: water's reflection coordinates); the rest
    // as the game gave them.
    HRESULT ProxyDevice::CreateVertexShader(CONST DWORD* pFunction,
                                            IDirect3DVertexShader9** ppShader)
    {
        static DWORD patched[16384];    // creation is serialised by the device
        size_t length = 0;
        const bool use = stereo_active() &&
                         stereo_patch_vertex_shader(pFunction, patched, 16384, &length);
        HRESULT hr = _device->CreateVertexShader(use ? patched : pFunction, ppShader);
        bool patched_ok = use && SUCCEEDED(hr);
        if (FAILED(hr) && use)
            hr = _device->CreateVertexShader(pFunction, ppShader);  // never worse than unpatched
        else if (use)
        {
            static unsigned told = 0;
            if (told++ < 3)
                log("stereo: created a vertex shader from patched bytecode");
        }
        if (SUCCEEDED(hr) && ppShader)
        {
            stereo_note_vertex_shader_created(*ppShader, pFunction, patched_ok);
            drawlog_shader_created(false, *ppShader, pFunction, patched_ok);   // the game's bytecode
            trace_dump_shader('v', *ppShader, pFunction);
            if (patched_ok)
                trace_dump_shader('q', *ppShader, patched);   // the patched copy
        }
        return hr;
    }

    HRESULT ProxyDevice::SetVertexShaderConstantF(UINT StartRegister,
                                                  CONST float* pConstantData,
                                                  UINT Vector4fCount)
    {
        stereo_note_constants(StartRegister, pConstantData, Vector4fCount);
        if (drawlog_active())
            drawlog_note_vsc(StartRegister, pConstantData, Vector4fCount, _ReturnAddress());
        // TEMPORARY (first-person research): who uploads Lara's skin palette
        // (c85, 42 bones x 4 rows), and from where.
        if (StartRegister == 85 && Vector4fCount >= 160 && pConstantData)
        {
            static unsigned told = 0;
            if (told < 4)
            {
                ++told;
                void* frames[16] = {};
                const USHORT n = RtlCaptureStackBackTrace(0, 16, frames, nullptr);
                char line[512];
                int len = snprintf(line, sizeof(line), "research: palette x%u from %p; stack",
                                   Vector4fCount, (const void*)pConstantData);
                for (USHORT i = 0; i < n && len < (int)sizeof(line) - 12; ++i)
                    len += snprintf(line + len, sizeof(line) - len, " %p", frames[i]);
                log("%s", line);
            }
        }
        if (trace_active() && pConstantData)
        {
            trace("vsc c%u x%u [%g %g %g %g]%s", StartRegister, Vector4fCount, pConstantData[0],
                  pConstantData[1], pConstantData[2], pConstantData[3],
                  StartRegister == 8 ? " (pass camera)" : "");
            // The rest of a matrix upload (c16 ScreenMatrix and the like).
            // The skin palette (c85..) prints in full: bone matrices are measured from it.
            const UINT rows = StartRegister == 85 ? Vector4fCount : 8;
            for (UINT i = 1; i < Vector4fCount && i < rows; ++i)
                trace("    c%u [%g %g %g %g]", StartRegister + i, pConstantData[i * 4],
                      pConstantData[i * 4 + 1], pConstantData[i * 4 + 2], pConstantData[i * 4 + 3]);
        }
        const HRESULT hr = _device->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
        return hr;
    }
}
