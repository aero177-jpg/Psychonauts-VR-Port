#include "trace.h"

#include "../common/log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>

namespace psyvr
{
    namespace
    {
        bool g_active = false;
        unsigned g_frames = 0;
        unsigned g_lines = 0;
        unsigned g_frames_left = 0;     // further consecutive frames to trace
        unsigned g_requested = 0;       // frames asked for, taken at the next boundary
        const unsigned kMaxLines = 20000;
    }

    void trace_frame_boundary()
    {
        if (g_active)
        {
            log("trace: end of frame (%u lines)", g_lines);
            g_active = false;
            if (g_frames_left > 0)
            {
                --g_frames_left;
                g_active = true;
                g_lines = 0;
                log("trace: start of frame");
                return;
            }
        }
        if (!g_requested)
        {
            if ((++g_frames % 30) != 0)
                return;
            wchar_t path[MAX_PATH]{};
            swprintf_s(path, L"%strace.flag", exe_dir());
            if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
                return;
            // The file may hold a number of consecutive frames to trace
            // (default 1), for catching something that happens only now and
            // then.
            int n = 1;
            if (FILE* f = _wfopen(path, L"r"))
            {
                if (fscanf(f, "%d", &n) != 1)
                    n = 1;
                fclose(f);
            }
            DeleteFileW(path);
            trace_request(n < 1 ? 1u : (unsigned)n);
        }
        g_frames_left = g_requested - 1;
        g_requested = 0;
        g_active = true;
        g_lines = 0;
        log("trace: start of frame");
    }

    void trace_request(unsigned frames)
    {
        g_requested = frames < 1 ? 1 : (frames > 120 ? 120 : frames);
    }

    bool trace_active()
    {
        return g_active;
    }

    void trace(const char* fmt, ...)
    {
        if (!g_active || g_lines >= kMaxLines)
            return;
        ++g_lines;
        char line[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        log("T %s", line);
    }

    void trace_dump_shader(char kind, const void* handle, const unsigned long* bytecode)
    {
        static int dir_exists = -1;
        wchar_t dir[MAX_PATH]{};
        swprintf_s(dir, L"%spsyvr_shaders", exe_dir());
        if (dir_exists < 0)
            dir_exists = GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
        if (!dir_exists || !handle || !bytecode)
            return;
        // SM1-3 bytecode ends with the token 0x0000FFFF; instruction operands
        // never equal it.
        size_t n = 1;
        while (n < 65536 && bytecode[n - 1] != 0x0000FFFFul)
            ++n;
        wchar_t path[MAX_PATH]{};
        swprintf_s(path, L"%s\\%cs_%p.bin", dir, kind, handle);
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            return;
        DWORD written = 0;
        WriteFile(f, bytecode, static_cast<DWORD>(n * 4), &written, nullptr);
        CloseHandle(f);
    }
}
