#pragma once
// One-frame trace of the game's D3D9 calls, for working out its passes.
//
// Create a file named trace.flag beside the game: at the next frame boundary
// it is deleted and the following frame is written to psyvr.log, call by
// call. Costs nothing while no trace is running.

namespace psyvr
{
    // From the frame boundary (the swap chain Present).
    void trace_frame_boundary();
    bool trace_active();
    // Traces the next `frames` (1..120) consecutive frames, as trace.flag
    // holding that number does; taken at the next frame boundary.
    void trace_request(unsigned frames);
    void trace(const char* fmt, ...);

    // If a folder named psyvr_shaders exists beside the game, writes the
    // bytecode of each shader as it is created, named by the handle the trace
    // prints (vs_<handle>.bin, ps_<handle>.bin; "fxc /dumpbin" reads them).
    void trace_dump_shader(char kind, const void* handle, const unsigned long* bytecode);
}
