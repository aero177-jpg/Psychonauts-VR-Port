#pragma once
// A minimal 5-byte inline hook, from the Legend VR mod.
//
// Psychonauts.exe has no ASLR, so targets are fixed addresses; every install checks
// the bytes it is about to overwrite against a signature first and refuses
// on a mismatch, and refuses prologues containing relative branches, which a
// verbatim trampoline would break.

#include <windows.h>

namespace psyvr
{
    // `expect` must match the `len` bytes at `target`; `len` must land on an
    // instruction boundary and be at least 5. On success `trampoline` gets a
    // callable that runs the original instructions and jumps back.
    // `trusted_prologue` skips only the relative-branch scan, for a prologue
    // the caller has checked by hand holds no relative branch (the scan
    // reads address bytes that happen to be 0xE8 as a call); the signature
    // check still applies.
    bool hook_install(void* target, void* detour, void** trampoline,
                      const unsigned char* expect, size_t len,
                      const char* name, bool trusted_prologue = false);
}
