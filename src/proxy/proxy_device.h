#pragma once
// IDirect3DDevice9 wrapper.
//
// Forked from the Legend VR mod, itself forked from TRAWindowed (MIT,
// (c) 2018 chreden). A full wrapper rather than vtable patching: Underworld's
// device vtable is rewritten by d3d9 after creation and Steam's overlay hooks
// Present, and in-place patching broke on both (the Underworld mod's docs/AUDIT.md 9.2).
//
// Almost every method is a plain forward. The ones that are not are declared
// first and live in proxy_device.cpp.

#include "rt_scale.h"
#include "msaa_cap.h"
#include "../vr/stereo.h"
#include "../vr/vr_submit.h"
#include "trace.h"
#include "drawlog.h"
#include "proxy_query.h"
#include "overlay.h"
#include "../common/log.h"
#include <cstdio>
#include <d3d9.h>
#include <intrin.h>
#include <windows.h>

namespace psyvr
{
    // A non-indexed draw's vertex count from its primitive count.
    inline UINT vertex_count(D3DPRIMITIVETYPE type, UINT primitives)
    {
        switch (type)
        {
        case D3DPT_TRIANGLELIST: return primitives * 3;
        case D3DPT_TRIANGLESTRIP:
        case D3DPT_TRIANGLEFAN: return primitives + 2;
        case D3DPT_LINELIST: return primitives * 2;
        case D3DPT_LINESTRIP: return primitives + 1;
        default: return primitives;
        }
    }

    struct ProxyDevice : IDirect3DDevice9
    {
        IDirect3DDevice9* _device;
        IDirect3D9* _parent;
        volatile LONG _ref;
        unsigned _frames = 0;
        unsigned _draws = 0;
        unsigned _draws_last = 0;

        ProxyDevice(IDirect3DDevice9* device, IDirect3D9* parent)
            : _device(device), _parent(parent), _ref(1)
        {
        }

        ~ProxyDevice()
        {
            stereo_fade_release();
            hud_release();
            overlay_device_closing(_device);
            vr_submit_device_closing(_device);
            if (_device)
                _device->Release();
            // CreateDevice took a reference on the wrapper that made us.
            if (_parent)
                _parent->Release();
        }

        static void describe(IDirect3DSurface9* s, char* out, size_t n)
        {
            D3DSURFACE_DESC desc{};
            if (s && SUCCEEDED(s->GetDesc(&desc)))
                _snprintf_s(out, n, _TRUNCATE, "%p %ux%u fmt=%d ms=%d", (void*)s, desc.Width, desc.Height, (int)desc.Format, (int)desc.MultiSampleType);
            else
                _snprintf_s(out, n, _TRUNCATE, "%p", (void*)s);
        }

        // Called once per frame from whichever Present the game uses (the
        // swap chain's, for Underworld).
        void frame_boundary();

        /*** IUnknown ***/
        // Hands back the wrapper for the device interfaces. Forwarding would
        // leak the real device and every call made through it would bypass us.
        STDMETHOD(QueryInterface)(THIS_ REFIID riid, void** ppvObj);

        STDMETHOD_(ULONG, AddRef)(THIS)
        {
            return (ULONG)InterlockedIncrement(&_ref);
        }

        STDMETHOD_(ULONG, Release)(THIS)
        {
            const LONG count = InterlockedDecrement(&_ref);
            if (count == 0)
                delete this;
            return (ULONG)count;
        }

        /*** instrumented ***/
        STDMETHOD(Present)(THIS_ CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion);
        STDMETHOD(Reset)(THIS_ D3DPRESENT_PARAMETERS* pPresentationParameters);
        STDMETHOD(SetVertexShaderConstantF)(THIS_ UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount);
        STDMETHOD(SetTexture)(THIS_ DWORD Stage, IDirect3DBaseTexture9* pTexture) { stereo_note_texture(Stage, pTexture); if (Stage < 16) g_dls.tex[Stage] = pTexture; if (trace_active() && pTexture && pTexture->GetType() == D3DRTYPE_TEXTURE) { D3DSURFACE_DESC desc{}; if (SUCCEEDED(static_cast<IDirect3DTexture9*>(pTexture)->GetLevelDesc(0, &desc)) && (desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) trace("tex s%lu = target %p %ux%u fmt=%d", Stage, (void*)pTexture, desc.Width, desc.Height, (int)desc.Format); } return _device->SetTexture(Stage, pTexture); }

        // Before a draw, when stereo asks for it (stereo_wants_vertex_box;
        // stereo.h): the x / y bounds of `count` vertices from `first`
        // (absolute, base vertex included); `up` = the draw's own vertex data.
        void note_vertex_box(INT first, UINT count, const void* up, UINT up_stride);
        // Every draw goes through here: the single place stereo will issue a
        // draw once per eye.
        // `caller`: the game's return address into the draw method (taken in
        // the method itself, which is reached through the vtable and so never
        // inlined into the game).
        template <typename Draw>
        HRESULT dispatch_draw(Draw draw, const DrawCall& call, void* caller)
        {
            ++_draws;
            const HRESULT hr = stereo_draw(_device, draw);
            if (trace_active())
                trace("draw #%u %s", _draws, stereo_last_draw_kind());
            if (drawlog_active())
                drawlog_draw(_device, call, caller, stereo_last_draw_kind());
            return hr;
        }
        STDMETHOD(DrawPrimitive)(THIS_ D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) { if (stereo_wants_vertex_box()) note_vertex_box((INT)StartVertex, vertex_count(PrimitiveType, PrimitiveCount), nullptr, 0); const HRESULT hr = dispatch_draw([&] { return _device->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount); }, DrawCall{"DP", (UINT)PrimitiveType, PrimitiveCount, 0, nullptr, 0, StartVertex}, _ReturnAddress()); return hr; }
        STDMETHOD(DrawIndexedPrimitive)(THIS_ D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) { if (stereo_wants_vertex_box()) note_vertex_box(BaseVertexIndex + (INT)MinVertexIndex, NumVertices, nullptr, 0); const HRESULT hr = dispatch_draw([&] { return _device->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount); }, DrawCall{"DIP", (UINT)PrimitiveType, primCount, NumVertices, nullptr, 0, (UINT)(BaseVertexIndex + (INT)MinVertexIndex)}, _ReturnAddress()); return hr; }
        STDMETHOD(DrawPrimitiveUP)(THIS_ D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) { if (stereo_wants_vertex_box()) note_vertex_box(0, vertex_count(PrimitiveType, PrimitiveCount), pVertexStreamZeroData, VertexStreamZeroStride); const HRESULT hr = dispatch_draw([&] { return _device->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride); }, DrawCall{"DPUP", (UINT)PrimitiveType, PrimitiveCount, 0, pVertexStreamZeroData, VertexStreamZeroStride, 0}, _ReturnAddress()); return hr; }
        STDMETHOD(DrawIndexedPrimitiveUP)(THIS_ D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) { if (stereo_wants_vertex_box()) note_vertex_box((INT)MinVertexIndex, NumVertices, pVertexStreamZeroData, VertexStreamZeroStride); const HRESULT hr = dispatch_draw([&] { return _device->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride); }, DrawCall{"DIPUP", (UINT)PrimitiveType, PrimitiveCount, NumVertices, pVertexStreamZeroData, VertexStreamZeroStride, MinVertexIndex}, _ReturnAddress()); return hr; }

        STDMETHOD(TestCooperativeLevel)(THIS) { return _device->TestCooperativeLevel(); }
        STDMETHOD_(UINT, GetAvailableTextureMem)(THIS) { return _device->GetAvailableTextureMem(); }
        STDMETHOD(EvictManagedResources)(THIS) { return _device->EvictManagedResources(); }

        STDMETHOD(GetDirect3D)(THIS_ IDirect3D9** ppD3D9)
        {
            // Hand back the wrapper we were created from, for the same reason
            // QueryInterface does.
            if (!ppD3D9)
                return D3DERR_INVALIDCALL;
            if (_parent)
            {
                _parent->AddRef();
                *ppD3D9 = _parent;
                return D3D_OK;
            }
            return _device->GetDirect3D(ppD3D9);
        }

        STDMETHOD(GetDeviceCaps)(THIS_ D3DCAPS9* pCaps) { return _device->GetDeviceCaps(pCaps); }
        STDMETHOD(GetDisplayMode)(THIS_ UINT iSwapChain, D3DDISPLAYMODE* pMode) { return _device->GetDisplayMode(iSwapChain, pMode); }
        STDMETHOD(GetCreationParameters)(THIS_ D3DDEVICE_CREATION_PARAMETERS* pParameters) { return _device->GetCreationParameters(pParameters); }
        STDMETHOD(SetCursorProperties)(THIS_ UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap) { return _device->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap); }
        STDMETHOD_(void, SetCursorPosition)(THIS_ int X, int Y, DWORD Flags) { _device->SetCursorPosition(X, Y, Flags); }
        STDMETHOD_(BOOL, ShowCursor)(THIS_ BOOL bShow) { return _device->ShowCursor(bShow); }
        // Both wrapped: the game presents through the swap chain, so an
        // unwrapped one hides every frame boundary from us.
        STDMETHOD(CreateAdditionalSwapChain)(THIS_ D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain);
        STDMETHOD(GetSwapChain)(THIS_ UINT iSwapChain, IDirect3DSwapChain9** pSwapChain);
        STDMETHOD_(UINT, GetNumberOfSwapChains)(THIS) { return _device->GetNumberOfSwapChains(); }
        STDMETHOD(GetBackBuffer)(THIS_ UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) { return _device->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer); }
        STDMETHOD(GetRasterStatus)(THIS_ UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) { return _device->GetRasterStatus(iSwapChain, pRasterStatus); }
        STDMETHOD(SetDialogBoxMode)(THIS_ BOOL bEnableDialogs) { return _device->SetDialogBoxMode(bEnableDialogs); }
        STDMETHOD_(void, SetGammaRamp)(THIS_ UINT iSwapChain, DWORD Flags, CONST D3DGAMMARAMP* pRamp) { _device->SetGammaRamp(iSwapChain, Flags, pRamp); }
        STDMETHOD_(void, GetGammaRamp)(THIS_ UINT iSwapChain, D3DGAMMARAMP* pRamp) { _device->GetGammaRamp(iSwapChain, pRamp); }
        STDMETHOD(CreateTexture)(THIS_ UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle);
        STDMETHOD(CreateVolumeTexture)(THIS_ UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle) { return _device->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle); }
        STDMETHOD(CreateCubeTexture)(THIS_ UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle) { return _device->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle); }
        STDMETHOD(CreateVertexBuffer)(THIS_ UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) { return _device->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle); }
        STDMETHOD(CreateIndexBuffer)(THIS_ UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle) { return _device->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle); }
        STDMETHOD(CreateRenderTarget)(THIS_ UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) { msaa_clamp(&MultiSample, &MultisampleQuality); return _device->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle); }
        STDMETHOD(CreateDepthStencilSurface)(THIS_ UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle);
        STDMETHOD(UpdateSurface)(THIS_ IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestinationSurface, CONST POINT* pDestPoint) { return _device->UpdateSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint); }
        STDMETHOD(UpdateTexture)(THIS_ IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture) { return _device->UpdateTexture(pSourceTexture, pDestinationTexture); }
        STDMETHOD(GetRenderTargetData)(THIS_ IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) { return _device->GetRenderTargetData(pRenderTarget, pDestSurface); }
        STDMETHOD(GetFrontBufferData)(THIS_ UINT iSwapChain, IDirect3DSurface9* pDestSurface) { return _device->GetFrontBufferData(iSwapChain, pDestSurface); }
        STDMETHOD(StretchRect)(THIS_ IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter);
        STDMETHOD(ColorFill)(THIS_ IDirect3DSurface9* pSurface, CONST RECT* pRect, D3DCOLOR color) { return _device->ColorFill(pSurface, pRect, color); }
        STDMETHOD(CreateOffscreenPlainSurface)(THIS_ UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) { return _device->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle); }
        STDMETHOD(SetRenderTarget)(THIS_ DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) { if (trace_active()) { char s[128]; describe(pRenderTarget, s, sizeof(s)); trace("rt%lu = %s", RenderTargetIndex, s); } const HRESULT hr = _device->SetRenderTarget(RenderTargetIndex, pRenderTarget); if (SUCCEEDED(hr) && RenderTargetIndex == 0) { rt_scale_note_target(pRenderTarget); stereo_note_render_target(pRenderTarget); g_dls.rt0 = pRenderTarget; g_dls.viewport_valid = false; g_dls.scissor_valid = false; g_dls.scissor_caller = nullptr; /* d3d9 resets both to the new target */ } return hr; }
        STDMETHOD(GetRenderTarget)(THIS_ DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) { return _device->GetRenderTarget(RenderTargetIndex, ppRenderTarget); }
        STDMETHOD(SetDepthStencilSurface)(THIS_ IDirect3DSurface9* pNewZStencil) { if (trace_active()) { char s[128]; describe(pNewZStencil, s, sizeof(s)); trace("ds = %s", s); } g_dls.ds = pNewZStencil; return _device->SetDepthStencilSurface(pNewZStencil); }
        STDMETHOD(GetDepthStencilSurface)(THIS_ IDirect3DSurface9** ppZStencilSurface) { return _device->GetDepthStencilSurface(ppZStencilSurface); }
        STDMETHOD(BeginScene)(THIS) { return _device->BeginScene(); }
        STDMETHOD(EndScene)(THIS) { return _device->EndScene(); }
        STDMETHOD(Clear)(THIS_ DWORD Count, CONST D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) { trace("clear n=%lu flags=0x%lx", Count, Flags); return _device->Clear(Count, pRects, Flags, Color, Z, Stencil); }
        STDMETHOD(SetTransform)(THIS_ D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) { return _device->SetTransform(State, pMatrix); }
        STDMETHOD(GetTransform)(THIS_ D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) { return _device->GetTransform(State, pMatrix); }
        STDMETHOD(MultiplyTransform)(THIS_ D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) { return _device->MultiplyTransform(State, pMatrix); }
        STDMETHOD(SetViewport)(THIS_ CONST D3DVIEWPORT9* pViewport);
        STDMETHOD(GetViewport)(THIS_ D3DVIEWPORT9* pViewport);
        STDMETHOD(SetMaterial)(THIS_ CONST D3DMATERIAL9* pMaterial) { return _device->SetMaterial(pMaterial); }
        STDMETHOD(GetMaterial)(THIS_ D3DMATERIAL9* pMaterial) { return _device->GetMaterial(pMaterial); }
        STDMETHOD(SetLight)(THIS_ DWORD Index, CONST D3DLIGHT9* light) { return _device->SetLight(Index, light); }
        STDMETHOD(GetLight)(THIS_ DWORD Index, D3DLIGHT9* light) { return _device->GetLight(Index, light); }
        STDMETHOD(LightEnable)(THIS_ DWORD Index, BOOL Enable) { return _device->LightEnable(Index, Enable); }
        STDMETHOD(GetLightEnable)(THIS_ DWORD Index, BOOL* pEnable) { return _device->GetLightEnable(Index, pEnable); }
        STDMETHOD(SetClipPlane)(THIS_ DWORD Index, CONST float* pPlane) { return _device->SetClipPlane(Index, pPlane); }
        STDMETHOD(GetClipPlane)(THIS_ DWORD Index, float* pPlane) { return _device->GetClipPlane(Index, pPlane); }
        STDMETHOD(SetRenderState)(THIS_ D3DRENDERSTATETYPE State, DWORD Value) { if ((unsigned)State < 256) g_dls.rs[State] = Value; if (State == D3DRS_ALPHABLENDENABLE) stereo_note_alpha_blend(Value != 0); if (trace_active() && (State == D3DRS_SCISSORTESTENABLE || State == D3DRS_ZENABLE || State == D3DRS_ZWRITEENABLE || State == D3DRS_STENCILENABLE || State == D3DRS_ALPHABLENDENABLE)) trace("rs %d = %lu", (int)State, Value); return _device->SetRenderState(State, Value); }
        STDMETHOD(GetRenderState)(THIS_ D3DRENDERSTATETYPE State, DWORD* pValue) { return _device->GetRenderState(State, pValue); }
        STDMETHOD(CreateStateBlock)(THIS_ D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) { return _device->CreateStateBlock(Type, ppSB); }
        STDMETHOD(BeginStateBlock)(THIS) { return _device->BeginStateBlock(); }
        STDMETHOD(EndStateBlock)(THIS_ IDirect3DStateBlock9** ppSB) { return _device->EndStateBlock(ppSB); }
        STDMETHOD(SetClipStatus)(THIS_ CONST D3DCLIPSTATUS9* pClipStatus) { return _device->SetClipStatus(pClipStatus); }
        STDMETHOD(GetClipStatus)(THIS_ D3DCLIPSTATUS9* pClipStatus) { return _device->GetClipStatus(pClipStatus); }
        STDMETHOD(GetTexture)(THIS_ DWORD Stage, IDirect3DBaseTexture9** ppTexture) { return _device->GetTexture(Stage, ppTexture); }
        STDMETHOD(GetTextureStageState)(THIS_ DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) { return _device->GetTextureStageState(Stage, Type, pValue); }
        STDMETHOD(SetTextureStageState)(THIS_ DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) { return _device->SetTextureStageState(Stage, Type, Value); }
        STDMETHOD(GetSamplerState)(THIS_ DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) { return _device->GetSamplerState(Sampler, Type, pValue); }
        STDMETHOD(SetSamplerState)(THIS_ DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) { return _device->SetSamplerState(Sampler, Type, Value); }
        STDMETHOD(ValidateDevice)(THIS_ DWORD* pNumPasses) { return _device->ValidateDevice(pNumPasses); }
        STDMETHOD(SetPaletteEntries)(THIS_ UINT PaletteNumber, CONST PALETTEENTRY* pEntries) { return _device->SetPaletteEntries(PaletteNumber, pEntries); }
        STDMETHOD(GetPaletteEntries)(THIS_ UINT PaletteNumber, PALETTEENTRY* pEntries) { return _device->GetPaletteEntries(PaletteNumber, pEntries); }
        STDMETHOD(SetCurrentTexturePalette)(THIS_ UINT PaletteNumber) { return _device->SetCurrentTexturePalette(PaletteNumber); }
        STDMETHOD(GetCurrentTexturePalette)(THIS_ UINT* PaletteNumber) { return _device->GetCurrentTexturePalette(PaletteNumber); }
        STDMETHOD(SetScissorRect)(THIS_ CONST RECT* pRect) { if (pRect) { g_dls.scissor = *pRect; g_dls.scissor_valid = true; g_dls.scissor_caller = _ReturnAddress(); } if (pRect) trace("scissor %ld,%ld - %ld,%ld", pRect->left, pRect->top, pRect->right, pRect->bottom); const unsigned f = rt_scale_current(); if (pRect && f > 1) { const RECT r = { pRect->left * (LONG)f, pRect->top * (LONG)f, pRect->right * (LONG)f, pRect->bottom * (LONG)f }; return _device->SetScissorRect(&r); } return _device->SetScissorRect(pRect); }
        STDMETHOD(GetScissorRect)(THIS_ RECT* pRect) { return _device->GetScissorRect(pRect); }
        STDMETHOD(SetSoftwareVertexProcessing)(THIS_ BOOL bSoftware) { return _device->SetSoftwareVertexProcessing(bSoftware); }
        STDMETHOD_(BOOL, GetSoftwareVertexProcessing)(THIS) { return _device->GetSoftwareVertexProcessing(); }
        STDMETHOD(SetNPatchMode)(THIS_ float nSegments) { return _device->SetNPatchMode(nSegments); }
        STDMETHOD_(float, GetNPatchMode)(THIS) { return _device->GetNPatchMode(); }
        STDMETHOD(ProcessVertices)(THIS_ UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags) { return _device->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags); }
        STDMETHOD(CreateVertexDeclaration)(THIS_ CONST D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl) { return _device->CreateVertexDeclaration(pVertexElements, ppDecl); }
        STDMETHOD(SetVertexDeclaration)(THIS_ IDirect3DVertexDeclaration9* pDecl) { g_dls.decl = pDecl; if (pDecl) g_dls.use_fvf = false; return _device->SetVertexDeclaration(pDecl); }
        STDMETHOD(GetVertexDeclaration)(THIS_ IDirect3DVertexDeclaration9** ppDecl) { return _device->GetVertexDeclaration(ppDecl); }
        STDMETHOD(SetFVF)(THIS_ DWORD FVF) { if (FVF) { g_dls.fvf = FVF; g_dls.use_fvf = true; } return _device->SetFVF(FVF); }
        STDMETHOD(GetFVF)(THIS_ DWORD* pFVF) { return _device->GetFVF(pFVF); }
        STDMETHOD(CreateVertexShader)(THIS_ CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader);
        STDMETHOD(SetVertexShader)(THIS_ IDirect3DVertexShader9* pShader) { stereo_note_vertex_shader(pShader); g_dls.vs = pShader; trace("vs %p", (void*)pShader); return _device->SetVertexShader(pShader); }
        STDMETHOD(GetVertexShader)(THIS_ IDirect3DVertexShader9** ppShader) { return _device->GetVertexShader(ppShader); }
        STDMETHOD(GetVertexShaderConstantF)(THIS_ UINT StartRegister, float* pConstantData, UINT Vector4fCount) { return _device->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
        STDMETHOD(SetVertexShaderConstantI)(THIS_ UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount) { return _device->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
        STDMETHOD(GetVertexShaderConstantI)(THIS_ UINT StartRegister, int* pConstantData, UINT Vector4iCount) { return _device->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
        STDMETHOD(SetVertexShaderConstantB)(THIS_ UINT StartRegister, CONST BOOL* pConstantData, UINT BoolCount) { return _device->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
        STDMETHOD(GetVertexShaderConstantB)(THIS_ UINT StartRegister, BOOL* pConstantData, UINT BoolCount) { return _device->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
        STDMETHOD(SetStreamSource)(THIS_ UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride) { if (StreamNumber == 0) { g_dls.vb0 = pStreamData; g_dls.vb0_offset = OffsetInBytes; g_dls.vb0_stride = Stride; } return _device->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride); }
        STDMETHOD(GetStreamSource)(THIS_ UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* pOffsetInBytes, UINT* pStride) { return _device->GetStreamSource(StreamNumber, ppStreamData, pOffsetInBytes, pStride); }
        STDMETHOD(SetStreamSourceFreq)(THIS_ UINT StreamNumber, UINT Setting) { return _device->SetStreamSourceFreq(StreamNumber, Setting); }
        STDMETHOD(GetStreamSourceFreq)(THIS_ UINT StreamNumber, UINT* pSetting) { return _device->GetStreamSourceFreq(StreamNumber, pSetting); }
        STDMETHOD(SetIndices)(THIS_ IDirect3DIndexBuffer9* pIndexData) { return _device->SetIndices(pIndexData); }
        STDMETHOD(GetIndices)(THIS_ IDirect3DIndexBuffer9** ppIndexData) { return _device->GetIndices(ppIndexData); }
        STDMETHOD(CreatePixelShader)(THIS_ CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader);
        STDMETHOD(SetPixelShader)(THIS_ IDirect3DPixelShader9* pShader) { g_dls.ps = pShader; trace("ps %p", (void*)pShader); return _device->SetPixelShader(pShader); }
        STDMETHOD(GetPixelShader)(THIS_ IDirect3DPixelShader9** ppShader) { return _device->GetPixelShader(ppShader); }
        STDMETHOD(SetPixelShaderConstantF)(THIS_ UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount) { stereo_note_ps_constants(StartRegister, pConstantData, Vector4fCount); if (drawlog_active()) drawlog_note_psc(StartRegister, pConstantData, Vector4fCount, _ReturnAddress()); if (trace_active() && pConstantData) trace("psc c%u x%u [%g %g %g %g]", StartRegister, Vector4fCount, pConstantData[0], pConstantData[1], pConstantData[2], pConstantData[3]); return _device->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
        STDMETHOD(GetPixelShaderConstantF)(THIS_ UINT StartRegister, float* pConstantData, UINT Vector4fCount) { return _device->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
        STDMETHOD(SetPixelShaderConstantI)(THIS_ UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount) { return _device->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
        STDMETHOD(GetPixelShaderConstantI)(THIS_ UINT StartRegister, int* pConstantData, UINT Vector4iCount) { return _device->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
        STDMETHOD(SetPixelShaderConstantB)(THIS_ UINT StartRegister, CONST BOOL* pConstantData, UINT BoolCount) { return _device->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
        STDMETHOD(GetPixelShaderConstantB)(THIS_ UINT StartRegister, BOOL* pConstantData, UINT BoolCount) { return _device->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
        STDMETHOD(DrawRectPatch)(THIS_ UINT Handle, CONST float* pNumSegs, CONST D3DRECTPATCH_INFO* pRectPatchInfo) { return _device->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo); }
        STDMETHOD(DrawTriPatch)(THIS_ UINT Handle, CONST float* pNumSegs, CONST D3DTRIPATCH_INFO* pTriPatchInfo) { return _device->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo); }
        STDMETHOD(DeletePatch)(THIS_ UINT Handle) { return _device->DeletePatch(Handle); }
        STDMETHOD(CreateQuery)(THIS_ D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery)
        {
            // Diagnostic: which queries the game makes (occlusion = 9).
            static unsigned counts[32] = {};
            if (ppQuery && (unsigned)Type < 32 && counts[Type]++ < 4)
                log("device: CreateQuery type %d (%u so far)", (int)Type, counts[Type]);
            const HRESULT hr = _device->CreateQuery(Type, ppQuery);
            if (SUCCEEDED(hr) && ppQuery && *ppQuery && Type == D3DQUERYTYPE_OCCLUSION)
                *ppQuery = new ProxyQuery(*ppQuery, this, counts[Type]);
            return hr;
        }
    };
}
