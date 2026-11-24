#include "hook.h"

#include "../common/log.h"

#include <cstdio>
#include <cstring>

// From the Legend VR mod's src/vr/hook.cpp (same author, Apache-2.0).
namespace psyvr
{
    namespace
    {
        // Trampolines have to be executable, and must not move, so they get
        // their own page rather than living on the heap.
        unsigned char* alloc_exec(size_t bytes)
        {
            return (unsigned char*)VirtualAlloc(nullptr, bytes,
                                                MEM_COMMIT | MEM_RESERVE,
                                                PAGE_EXECUTE_READWRITE);
        }

        void write_jmp(unsigned char* at, const void* to)
        {
            at[0] = 0xE9;                                    // jmp rel32
            const intptr_t rel = (intptr_t)to - (intptr_t)at - 5;
            memcpy(at + 1, &rel, 4);
        }
    }

    bool hook_install(void* target, void* detour, void** trampoline,
                      const unsigned char* expect, size_t len,
                      const char* name, bool trusted_prologue)
    {
        if (!target || !detour || !trampoline || !expect || len < 5 || len > 32)
            return false;

        unsigned char* t = (unsigned char*)target;

        // Refuse rather than corrupt. A mismatch means the address is not the
        // function we think it is, and writing anyway would break the game
        // somewhere far away from the cause.
        if (memcmp(t, expect, len) != 0)
        {
            log("hook: %s NOT installed -- the bytes at 0x%p are not what was expected",
                name, target);
            log("      expected:");
            char line[128]{};
            int n = 0;
            for (size_t i = 0; i < len; ++i)
                n += sprintf_s(line + n, sizeof(line) - n, "%02X ", expect[i]);
            log("        %s", line);
            n = 0;
            line[0] = '\0';
            for (size_t i = 0; i < len; ++i)
                n += sprintf_s(line + n, sizeof(line) - n, "%02X ", t[i]);
            log("      found:");
            log("        %s", line);
            return false;
        }

        // A trampoline is a verbatim copy of the original bytes, so anything
        // position-dependent in them is wrong the moment it moves. call rel32,
        // jmp rel32 and jmp rel8 all encode a displacement from their own
        // address; copied elsewhere they point into whatever happens to be
        // there.
        //
        // This cost a crash to learn -- UIScreenManager::Process begins with
        // two relative calls, and the trampoline jumped into the heap
        // (EIP 0x1d584b00). Refusing is the right answer rather than
        // relocating: a hook that cannot be installed safely is a hook that
        // should be moved to a different function, and the log says which.
        //
        // Deliberately crude -- these bytes are only scanned, not decoded, so
        // an immediate or a ModRM byte that happens to be 0xE8 refuses a hook
        // that would have been fine. That is the safe direction to be wrong
        // in, and the fix is to choose a different prologue length.
        for (size_t i = 0; i < len && !trusted_prologue; ++i)
        {
            if (t[i] == 0xE8 || t[i] == 0xE9 || t[i] == 0xEB)
            {
                log("hook: %s NOT installed -- byte %u of the prologue is 0x%02X, "
                    "a relative branch. Copying it into a trampoline would send "
                    "it somewhere else entirely.", name, (unsigned)i, t[i]);
                return false;
            }
        }

        unsigned char* tramp = alloc_exec(len + 5);
        if (!tramp)
        {
            log("hook: %s -- could not allocate a trampoline", name);
            return false;
        }

        memcpy(tramp, t, len);                 // the original instructions
        write_jmp(tramp + len, t + len);       // then back to what follows

        DWORD old = 0;
        if (!VirtualProtect(t, len, PAGE_EXECUTE_READWRITE, &old))
        {
            log("hook: %s -- could not unprotect 0x%p", name, target);
            VirtualFree(tramp, 0, MEM_RELEASE);
            return false;
        }

        write_jmp(t, detour);
        for (size_t i = 5; i < len; ++i)
            t[i] = 0x90;                       // pad the remainder with nops

        VirtualProtect(t, len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), t, len);

        *trampoline = tramp;
        log("hook: %s installed at 0x%p", name, target);
        return true;
    }
}
