#pragma once
// Structured per-draw log of one frame, as JSON lines (tools/README.md
// section 5, "drawlog"): evidence for classifying every effect draw.
//
// Armed by the control channel (`shot <name>` for its traced frame, or
// `drawlog <name>` alone). The frame drawn after the arming boundary is
// written to <game folder>\psyvr_shots\<name>.draws.jsonl when it ends.
//
// Device state is tracked always (a few plain assignments in the proxy's
// setters, g_dls); the records, descriptions and constant lists are built
// only while a frame is being logged. Shader bytecode is copied and hashed
// once, at creation.

#include <d3d9.h>

namespace psyvr
{
    // The device state as the proxy's setters have seen it. Raw pointers,
    // never dereferenced except at a draw while logging (the device keeps
    // everything bound alive until then). Unknown = null / invalid flag /
    // 0xFFFFFFFF render state: filled from the device at the first logged draw.
    struct DrawLogState
    {
        IDirect3DVertexDeclaration9* decl;
        DWORD fvf;
        bool use_fvf;               // SetFVF was the last of SetFVF / SetVertexDeclaration
        IDirect3DVertexBuffer9* vb0;
        UINT vb0_offset;
        UINT vb0_stride;
        RECT scissor;
        bool scissor_valid;
        void* scissor_caller;       // return address of the SetScissorRect call
        D3DVIEWPORT9 viewport;
        bool viewport_valid;
        IDirect3DSurface9* rt0;
        IDirect3DSurface9* ds;
        IDirect3DBaseTexture9* tex[16];
        IDirect3DVertexShader9* vs;
        IDirect3DPixelShader9* ps;
        DWORD rs[256];

        // Everything unknown (start-up, after a device Reset).
        void clear();
    };
    extern DrawLogState g_dls;
    extern bool g_drawlog_active;

    inline bool drawlog_active() { return g_drawlog_active; }

    // One draw call as the game made it.
    struct DrawCall
    {
        const char* call;           // "DP" "DIP" "DPUP" "DIPUP"
        UINT prim;                  // D3DPRIMITIVETYPE
        UINT prim_count;
        UINT num_vertices;          // 0 = derive from the primitive type and count
        const void* up;             // user-pointer vertex data (UP draws)
        UINT up_stride;
        UINT up_first_vertex;       // first vertex used (UP: of the user data;
                                    // DP: StartVertex; DIP: Base + MinVertexIndex)
    };

    // After Create*Shader succeeded: copies and hashes the game's bytecode.
    // `patched`: the shim created the vertex shader from patched bytecode.
    void drawlog_shader_created(bool pixel, const void* shader, const DWORD* bytecode, bool patched);

    // Asks for the next frame to be logged under `name`. False if a log is
    // pending or running.
    bool drawlog_request(const char* name);
    bool drawlog_busy();
    // From ProxyDevice::frame_boundary, after control_frame_boundary: ends
    // the running log (writes the file) and starts a requested one.
    void drawlog_frame_boundary();
    // The device was Reset: every resource pointer is stale.
    void drawlog_device_reset();

    struct DrawLogResult
    {
        bool ok = false;            // the file was written
        unsigned draws = 0;
        char path[520] = {};
    };
    const DrawLogResult& drawlog_last_result();

    // After a draw, while drawlog_active(). `caller`: the game's return
    // address into the draw method.
    void drawlog_draw(IDirect3DDevice9* device, const DrawCall& call, void* caller, const char* kind);
    // SetVertexShaderConstantF / SetPixelShaderConstantF, while drawlog_active().
    void drawlog_note_vsc(UINT reg, const float* data, UINT count, void* caller);
    void drawlog_note_psc(UINT reg, const float* data, UINT count, void* caller);
    // While drawlog_active(): appends one finished JSON line (with its "\n")
    // at this point of the frame, between the draws. The serial changes with
    // every logged frame (probes reset their per-frame state on it).
    void drawlog_raw(const char* text, size_t len);
    unsigned drawlog_serial();
    // While on, each logged draw also writes its stream-0 vertex range to a
    // .bin beside the log (once per range a frame; record field "vdump").
    void drawlog_dump_vertices(bool on);
}
