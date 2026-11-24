#pragma once
// Diagnostic wrapper of the game's IDirect3DQuery9 (occlusion queries,
// docs/findings.md 2026-11-15): forwards everything; while a frame is being
// draw-logged, Issue and GetData leave a line in the draw log, so the draws
// inside each query and its result can be read off.

#include "drawlog.h"
#include "../vr/stereo.h"
#include <d3d9.h>
#include <intrin.h>
#include <cstdio>

namespace psyvr
{
    class ProxyQuery : public IDirect3DQuery9
    {
    public:
        ProxyQuery(IDirect3DQuery9* q, IDirect3DDevice9* device, unsigned id)
            : _q(q), _device(device), _id(id) {}

        STDMETHOD(QueryInterface)(THIS_ REFIID riid, void** ppvObj)
        {
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DQuery9))
            {
                AddRef();
                *ppvObj = this;
                return S_OK;
            }
            *ppvObj = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHOD_(ULONG, AddRef)(THIS) { return _q->AddRef(); }
        STDMETHOD_(ULONG, Release)(THIS)
        {
            const ULONG n = _q->Release();
            if (n == 0)
                delete this;
            return n;
        }
        STDMETHOD(GetDevice)(THIS_ IDirect3DDevice9** ppDevice)
        {
            if (!ppDevice)
                return D3DERR_INVALIDCALL;
            _device->AddRef();
            *ppDevice = _device;
            return D3D_OK;
        }
        STDMETHOD_(D3DQUERYTYPE, GetType)(THIS) { return _q->GetType(); }
        STDMETHOD_(DWORD, GetDataSize)(THIS) { return _q->GetDataSize(); }
        STDMETHOD(Issue)(THIS_ DWORD dwIssueFlags)
        {
            if (drawlog_active())
            {
                char t[160];
                const int n = snprintf(t, sizeof(t), "{\"query\":%u,\"issue\":\"%s\",\"caller\":\"%p\"}\n", _id,
                                       (dwIssueFlags & D3DISSUE_BEGIN) ? "begin" : "end", _ReturnAddress());
                drawlog_raw(t, (size_t)n);
            }
            return _q->Issue(dwIssueFlags);
        }
        STDMETHOD(GetData)(THIS_ void* pData, DWORD dwSize, DWORD dwGetDataFlags)
        {
            const HRESULT hr = _q->GetData(pData, dwSize, dwGetDataFlags);
            // The game fades objects by visible / total pixels of a box
            // (0x449C72 per object, 0x517949 lights), and total = 0 means
            // hidden. In stereo the box is drawn with the eyes' views, so a
            // head turn can take it out of both eyes while the object (a
            // tree's canopy above its box) is in the middle of the view: 0
            // becomes 1, so off-screen counts as visible; a box on screen
            // and behind something still gives a ratio near 0.
            if (hr == S_OK && pData && dwSize >= 4 && *(DWORD*)pData == 0 && stereo_active())
                *(DWORD*)pData = 1;
            if (drawlog_active())
            {
                char t[200];
                const int n = snprintf(t, sizeof(t),
                                       "{\"query\":%u,\"getdata\":%ld,\"pixels\":%lu,\"caller\":\"%p\"}\n", _id,
                                       (long)hr, (hr == S_OK && pData && dwSize >= 4) ? *(DWORD*)pData : 0ul,
                                       _ReturnAddress());
                drawlog_raw(t, (size_t)n);
            }
            return hr;
        }

    private:
        IDirect3DQuery9* _q;
        IDirect3DDevice9* _device;
        unsigned _id;
    };
}
