#pragma once
// Loading the real d3d9 that we forward to.
//
// The game finds Direct3D by LoadLibrary("d3d9.dll"), which searches its own
// folder first, so dropping this DLL beside Psychonauts.exe is enough to get in front
// of it. That also means we must never load "d3d9.dll" by name ourselves --
// we would find ourselves. Everything here loads by full path.

#include <windows.h>

namespace psyvr
{
    // True when the game is to run exactly as unmodded: the environment
    // variable PSYVR_DISABLE is set (and not "0"), or a file psyvr_off.flag is
    // beside the game. This DLL then forwards straight to the system d3d9 and
    // touches nothing: no psyvr.ini or log is written, no DXVK, no OpenXR, no
    // window or input hooks. ("Play ... UNMODDED.bat" sets the variable.)
    bool mod_disabled();

    // Loads the backend if it is not loaded yet. Returns nullptr on failure.
    HMODULE backend();

    // Full path of whatever got loaded, for the log.
    const wchar_t* backend_path();

    // GetProcAddress against the backend, or nullptr.
    FARPROC backend_proc(const char* name);
}
