#include "perf_log.h"
#include "xr_runtime.h"

#include "../common/log.h"
#include "../common/config.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cwchar>
#include <utility>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

namespace psyvr
{
    namespace
    {
        VkInstance g_instance = VK_NULL_HANDLE;
        VkPhysicalDevice g_physical = VK_NULL_HANDLE;
        PFN_vkGetPhysicalDeviceMemoryProperties2 g_get_memory = nullptr;

        LARGE_INTEGER g_freq{}, g_window_start{}, g_last_frame{};
        unsigned g_frames = 0;
        double g_worst_ms = 0.0, g_wait_ms = 0.0;

        // Per-stage frame timing, see PerfMark. Stages: game, hud, submit,
        // wait, mirror, present.
        LARGE_INTEGER g_stamp[PerfAfterPresent + 1]{};
        LARGE_INTEGER g_prev_present_end{}, g_first_mark{};
        double g_sum[6]{}, g_worst[6]{}, g_worst_total = 0.0;
        unsigned g_split_frames = 0, g_hitches = 0, g_hitch_lines = 0;
        double g_last_kernel_s = 0.0;


        // Sampling profiler for the slow "game" stage (the frames in the
        // 2026-10-19 log spent 100+ ms there at one core of CPU with flat
        // memory). While the game thread has been inside a frame for more
        // than 30 ms, a watchdog thread suspends it for a moment, reads its
        // instruction pointer and counts it by module and 32-byte block.
        // The five-second report lists the busiest blocks.
        struct Sample { HMODULE module; DWORD rva; unsigned count; };
        constexpr int kSampleSlots = 192;
        Sample g_samples[kSampleSlots]{};
        int g_sample_used = 0;
        unsigned g_sample_total = 0, g_sample_dropped = 0;
        // Initialised at load: the first report can come before the first
        // Present has started the sampler.
        struct SampleLock
        {
            CRITICAL_SECTION cs;
            SampleLock() { InitializeCriticalSection(&cs); }
        } g_sample_lock_holder;
        CRITICAL_SECTION& g_sample_lock = g_sample_lock_holder.cs;
        HANDLE g_game_thread = nullptr;
        DWORD g_game_tid = 0;
        volatile LONGLONG g_frame_start_qpc = 0;
        volatile LONGLONG g_last_mark_qpc = 0;   // any perf mark: for the hang check
        volatile LONG g_last_mark_id = -1;
        volatile LONG g_sampler_stop = 0;

        DWORD WINAPI sampler_main(void*)
        {
            LARGE_INTEGER freq{};
            QueryPerformanceFrequency(&freq);
            while (!g_sampler_stop)
            {
                Sleep(3);
                const LONGLONG start = g_frame_start_qpc;
                const LONGLONG mark = g_last_mark_qpc;
                if (!g_game_thread)
                    continue;
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                const bool sample = start && (now.QuadPart - start) * 1000 >= 30 * freq.QuadPart;
                // No perf mark for over 2 s (stuck in the game or in our own
                // stages): once per stall, log the stack (a scan for return
                // addresses into loaded modules).
                static LONGLONG hang_logged_for = 0;
                const bool hang = mark && (now.QuadPart - mark) > 2 * freq.QuadPart && hang_logged_for != mark;
                const float exit_after = config().debug_hang_exit;
                if (mark && exit_after > 0.0f && (now.QuadPart - mark) > LONGLONG(exit_after * freq.QuadPart))
                {
                    const char* act = log_get_activity();
                    log("HANG: still stuck after %.0f s (activity: %s); ending the game ([debug] hang_exit_seconds)",
                        exit_after, act ? act : "game code");
                    log_close();
                    TerminateProcess(GetCurrentProcess(), 0xDEAD);
                }
                if (!sample && !hang)
                    continue;
                static DWORD stack_copy[8192];
                DWORD stack_words = 0;
                // Suspended for as short a time as possible: nothing that
                // can take a lock runs between Suspend and Resume.
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL;
                bool got = false;
                if (SuspendThread(g_game_thread) != (DWORD)-1)
                {
                    got = GetThreadContext(g_game_thread, &ctx) != 0;
                    if (got && hang)
                    {
                        SIZE_T read = 0;
                        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(static_cast<uintptr_t>(ctx.Esp)),
                                              stack_copy, sizeof(stack_copy), &read) ||
                            read > 0)
                            stack_words = DWORD(read / 4);
                        else if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(static_cast<uintptr_t>(ctx.Esp)),
                                                   stack_copy, 1024, &read))
                            stack_words = DWORD(read / 4);
                    }
                    ResumeThread(g_game_thread);
                }
                if (!got)
                    continue;
                if (hang)
                {
                    hang_logged_for = mark;
                    auto name_of = [](DWORD addr, char* out, size_t cap) -> bool {
                        HMODULE m = nullptr;
                        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(addr)), &m) || !m)
                            return false;
                        MEMORY_BASIC_INFORMATION mbi{};
                        if (!VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), &mbi, sizeof(mbi)) ||
                            !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
                            return false;
                        wchar_t path[MAX_PATH] = L"?";
                        GetModuleFileNameW(m, path, MAX_PATH);
                        const wchar_t* n = wcsrchr(path, L'\\');
                        n = n ? n + 1 : path;
                        char narrow[64];
                        WideCharToMultiByte(CP_UTF8, 0, n, -1, narrow, sizeof(narrow), nullptr, nullptr);
                        sprintf_s(out, cap, "%s+0x%X", narrow, addr - DWORD(reinterpret_cast<uintptr_t>(m)));
                        return true;
                    };
                    // Frame-pointer walk (the game and ntdll keep EBP frames):
                    // accurate where the scan above also lists stale values.
                    auto walk = [&](DWORD esp, DWORD ebp, DWORD eip, const DWORD* stk, DWORD words, char* out, int len,
                                    int cap) -> int {
                        char one_f[96];
                        len += sprintf_s(out + len, cap - len, " | frames:");
                        if (name_of(eip, one_f, sizeof(one_f)))
                            len += sprintf_s(out + len, cap - len, " %s", one_f);
                        for (int i = 0; i < 40 && len < cap - 100; ++i)
                        {
                            if (ebp < esp || ebp + 8 > esp + words * 4 || (ebp & 3))
                                break;
                            const DWORD idx = (ebp - esp) / 4;
                            const DWORD next = stk[idx], ret = stk[idx + 1];
                            if (name_of(ret, one_f, sizeof(one_f)))
                                len += sprintf_s(out + len, cap - len, " %s", one_f);
                            else
                                len += sprintf_s(out + len, cap - len, " 0x%08X", ret);
                            if (next <= ebp)
                                break;
                            ebp = next;
                        }
                        return len;
                    };
                    char text[2600];
                    char one[96];
                    int len = sprintf_s(text, "HANG: game thread %.1f s past perf mark %ld; eip ",
                                        double(now.QuadPart - mark) / double(freq.QuadPart), g_last_mark_id);
                    if (name_of(ctx.Eip, one, sizeof(one)))
                        len += sprintf_s(text + len, sizeof(text) - len, "%s", one);
                    else
                        len += sprintf_s(text + len, sizeof(text) - len, "0x%08X", ctx.Eip);
                    {
                        const char* act = log_get_activity();
                        len += sprintf_s(text + len, sizeof(text) - len, "; activity: %s; stack:", act ? act : "game code");
                    }
                    int found = 0;
                    for (DWORD i = 0; i < stack_words && found < 12 && len < 1300; ++i)
                        if (name_of(stack_copy[i], one, sizeof(one)))
                        {
                            len += sprintf_s(text + len, sizeof(text) - len, " %s", one);
                            ++found;
                        }
                    len = walk(ctx.Esp, ctx.Ebp, ctx.Eip, stack_copy, stack_words, text, len, (int)sizeof(text));
                    log("%s", text);

                    // The game thread's windows and where their procedures live
                    // (game code reached through a window callback deadlocked).
                    {
                        struct Ctx { char (*name_of_buf)[96]; } dummy{};
                        (void)dummy;
                        // The game's global allocator lock (Psychonauts.exe has no
                        // ASLR): who holds it.
                        MEMORY_BASIC_INFORMATION mi{};
                        const auto* cs = reinterpret_cast<const RTL_CRITICAL_SECTION*>(0x791490);
                        if (VirtualQuery(cs, &mi, sizeof(mi)) && mi.State == MEM_COMMIT)
                            log("HANG lock 0x791490: owner thread %lu, recursion %ld, lock count %ld (game thread %lu)",
                                (DWORD)(uintptr_t)cs->OwningThread, cs->RecursionCount, cs->LockCount, g_game_tid);
                        EnumThreadWindows(g_game_tid, [](HWND hwnd, LPARAM) -> BOOL {
                            wchar_t cls[64] = L"", title[64] = L"";
                            GetClassNameW(hwnd, cls, 64);
                            InternalGetWindowText(hwnd, title, 64);   // no message: the owner may be hung
                            const DWORD proc = (DWORD)GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
                            HMODULE m = nullptr;
                            wchar_t path[MAX_PATH] = L"?";
                            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                   reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(proc)), &m) && m)
                                GetModuleFileNameW(m, path, MAX_PATH);
                            const wchar_t* n = wcsrchr(path, L'\\');
                            log("HANG window %p class \"%ls\" title \"%ls\" proc 0x%08X (%ls+0x%X)", (void*)hwnd, cls,
                                title, proc, n ? n + 1 : path, m ? proc - DWORD(reinterpret_cast<uintptr_t>(m)) : 0);
                            return TRUE;
                        }, 0);
                    }

                    // Every other thread too: one line each, eip and the
                    // return addresses on its stack. Each is suspended only
                    // while its context and stack are copied.
                    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                    if (snap != INVALID_HANDLE_VALUE)
                    {
                        THREADENTRY32 te{ sizeof(te) };
                        const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
                        const DWORD game_tid = g_game_tid;
                        for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te))
                        {
                            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || te.th32ThreadID == game_tid)
                                continue;
                            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
                            if (!th)
                                continue;
                            CONTEXT tc{};
                            tc.ContextFlags = CONTEXT_CONTROL;
                            DWORD words = 0;
                            bool tok = false;
                            if (SuspendThread(th) != (DWORD)-1)
                            {
                                tok = GetThreadContext(th, &tc) != 0;
                                SIZE_T rd = 0;
                                if (tok)
                                {
                                    ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(static_cast<uintptr_t>(tc.Esp)),
                                                      stack_copy, sizeof(stack_copy), &rd);
                                    if (!rd)
                                        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(static_cast<uintptr_t>(tc.Esp)),
                                                          stack_copy, 1024, &rd);
                                }
                                words = DWORD(rd / 4);
                                ResumeThread(th);
                            }
                            CloseHandle(th);
                            if (!tok)
                                continue;
                            int tl = sprintf_s(text, "HANG thread %lu: eip ", te.th32ThreadID);
                            if (name_of(tc.Eip, one, sizeof(one)))
                                tl += sprintf_s(text + tl, sizeof(text) - tl, "%s", one);
                            else
                                tl += sprintf_s(text + tl, sizeof(text) - tl, "0x%08X", tc.Eip);
                            tl += sprintf_s(text + tl, sizeof(text) - tl, "; stack:");
                            int n = 0;
                            for (DWORD i = 0; i < words && n < 10 && tl < 1300; ++i)
                                if (name_of(stack_copy[i], one, sizeof(one)))
                                {
                                    tl += sprintf_s(text + tl, sizeof(text) - tl, " %s", one);
                                    ++n;
                                }
                            tl = walk(tc.Esp, tc.Ebp, tc.Eip, stack_copy, words, text, tl, (int)sizeof(text));
                            log("%s", text);
                        }
                        CloseHandle(snap);
                    }
                }
                if (!sample)
                    continue;
                HMODULE module = nullptr;
                DWORD rva = 0;
                const void* ip = reinterpret_cast<const void*>(static_cast<uintptr_t>(ctx.Eip));
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       static_cast<LPCWSTR>(ip), &module) && module)
                    rva = DWORD(reinterpret_cast<const char*>(ip) - reinterpret_cast<const char*>(module)) & ~31u;
                EnterCriticalSection(&g_sample_lock);
                ++g_sample_total;
                int i = 0;
                for (; i < g_sample_used; ++i)
                    if (g_samples[i].module == module && g_samples[i].rva == rva)
                        break;
                if (i < g_sample_used)
                    ++g_samples[i].count;
                else if (g_sample_used < kSampleSlots)
                    g_samples[g_sample_used++] = { module, rva, 1 };
                else
                    ++g_sample_dropped;
                LeaveCriticalSection(&g_sample_lock);
            }
            return 0;
        }

        void sampler_start()
        {
            g_game_tid = GetCurrentThreadId();
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_game_thread,
                            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
            HANDLE t = CreateThread(nullptr, 0, sampler_main, nullptr, 0, nullptr);
            if (t)
                CloseHandle(t);
        }

        void sampler_report()
        {
            EnterCriticalSection(&g_sample_lock);
            if (g_sample_total)
            {
                Sample top[6]{};
                int n = 0;
                for (int i = 0; i < g_sample_used; ++i)
                {
                    int at = n < 6 ? n++ : 5;
                    if (at == 5 && n == 6 && g_samples[i].count <= top[5].count)
                        continue;
                    top[at] = g_samples[i];
                    for (int k = at; k > 0 && top[k].count > top[k - 1].count; --k)
                        std::swap(top[k], top[k - 1]);
                }
                char text[900];
                int len = sprintf_s(text, "perf where (%u samples of the game thread while a frame ran over 30 ms%s):",
                                    g_sample_total, g_sample_dropped ? ", some dropped" : "");
                for (int i = 0; i < n && len > 0 && len < 760; ++i)
                {
                    wchar_t path[MAX_PATH] = L"?";
                    if (top[i].module)
                        GetModuleFileNameW(top[i].module, path, MAX_PATH);
                    const wchar_t* name = wcsrchr(path, L'\\');
                    name = name ? name + 1 : path;
                    char narrow[64];
                    WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr);
                    len += sprintf_s(text + len, sizeof(text) - len, " %s+0x%X x%u (%.0f%%);", narrow,
                                     top[i].rva, top[i].count, top[i].count * 100.0 / g_sample_total);
                }
                log("%s", text);
            }
            g_sample_used = 0;
            g_sample_total = g_sample_dropped = 0;
            LeaveCriticalSection(&g_sample_lock);
        }

        double ms_between(const LARGE_INTEGER& a, const LARGE_INTEGER& b)
        {
            return g_freq.QuadPart ? double(b.QuadPart - a.QuadPart) * 1000.0 / double(g_freq.QuadPart) : 0.0;
        }

        // GPU memory in device-local heaps: used by this process, and the
        // budget the driver gives it (VK_EXT_memory_budget). False if the
        // query is not available.
        bool gpu_memory(double* used_mb, double* budget_mb)
        {
            if (!g_get_memory || !g_physical)
                return false;
            VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
            VkPhysicalDeviceMemoryProperties2 props{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 };
            props.pNext = &budget;
            g_get_memory(g_physical, &props);
            double used = 0, total = 0;
            for (uint32_t i = 0; i < props.memoryProperties.memoryHeapCount; ++i)
            {
                if (!(props.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
                    continue;
                used += (double)budget.heapUsage[i];
                total += (double)budget.heapBudget[i];
            }
            *used_mb = used / (1024.0 * 1024.0);
            *budget_mb = total / (1024.0 * 1024.0);
            return total > 0;
        }
    }

    void perf_log_set_vulkan(void* instance, void* physical_device)
    {
        g_instance = static_cast<VkInstance>(instance);
        g_physical = static_cast<VkPhysicalDevice>(physical_device);
        // vulkan-1.dll is already loaded by DXVK; never load it ourselves.
        HMODULE vulkan = GetModuleHandleW(L"vulkan-1.dll");
        auto get_proc = vulkan ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                                     GetProcAddress(vulkan, "vkGetInstanceProcAddr"))
                               : nullptr;
        if (get_proc && g_instance)
            g_get_memory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(
                get_proc(g_instance, "vkGetPhysicalDeviceMemoryProperties2"));
        log("perf: GPU memory budget %s", g_get_memory ? "available" : "not available");
    }

    void perf_log_mark(PerfMark mark)
    {
        if (!g_freq.QuadPart)
            QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_stamp[mark]);
        g_last_mark_qpc = g_stamp[mark].QuadPart;
        g_last_mark_id = (LONG)mark;
        if (mark == PerfEnter)
            g_frame_start_qpc = 0;      // inside our own stages: not sampled
        static bool s_sampler_early = false;
        if (!s_sampler_early)       // from the first mark, so a hang in the first frame is caught
        {
            s_sampler_early = true;
            sampler_start();
        }
        if (mark != PerfAfterPresent)
            return;
        if (!g_first_mark.QuadPart)
            g_first_mark = g_stamp[PerfAfterPresent];
        if (g_prev_present_end.QuadPart)
        {
            const double stage[6] = {
                ms_between(g_prev_present_end, g_stamp[PerfEnter]),
                ms_between(g_stamp[PerfEnter], g_stamp[PerfBeforeSubmit]),
                ms_between(g_stamp[PerfBeforeSubmit], g_stamp[PerfAfterSubmit]),
                ms_between(g_stamp[PerfAfterSubmit], g_stamp[PerfAfterWait]),
                ms_between(g_stamp[PerfAfterWait], g_stamp[PerfBeforePresent]),
                ms_between(g_stamp[PerfBeforePresent], g_stamp[PerfAfterPresent]),
            };
            double total = 0.0;
            for (int i = 0; i < 6; ++i)
                if (stage[i] < 0.0)     // a frame without the VR stages
                {
                    g_prev_present_end = g_stamp[PerfAfterPresent];
                    g_frame_start_qpc = g_prev_present_end.QuadPart;
                    return;
                }
            for (int i = 0; i < 6; ++i)
            {
                g_sum[i] += stage[i];
                total += stage[i];
            }
            ++g_split_frames;
            if (total > g_worst_total)
            {
                g_worst_total = total;
                for (int i = 0; i < 6; ++i)
                    g_worst[i] = stage[i];
            }
            if (total > 33.0)
            {
                ++g_hitches;
                // Every slow frame, so the log shows when the collapse
                // starts and what the first bad frame was made of.
                if (g_hitch_lines < 400 && ms_between(g_first_mark, g_stamp[PerfAfterPresent]) > 20000.0)
                {
                    ++g_hitch_lines;
                    log("hitch: %.1f ms = game %.1f, hud %.1f, submit %.1f, xrWait %.1f, mirror %.1f, present %.1f",
                        total, stage[0], stage[1], stage[2], stage[3], stage[4], stage[5]);
                }
            }
        }
        g_prev_present_end = g_stamp[PerfAfterPresent];
        g_frame_start_qpc = g_prev_present_end.QuadPart;
    }

    void perf_log_frame()
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (!g_freq.QuadPart)
        {
            QueryPerformanceFrequency(&g_freq);
            g_window_start = g_last_frame = now;
            return;
        }
        const double frame_ms = double(now.QuadPart - g_last_frame.QuadPart) * 1000.0 / double(g_freq.QuadPart);
        g_last_frame = now;
        ++g_frames;
        if (frame_ms > g_worst_ms)
            g_worst_ms = frame_ms;
        g_wait_ms += xr_frame_wait_ms();

        const double window_s = double(now.QuadPart - g_window_start.QuadPart) / double(g_freq.QuadPart);
        if (window_s < 5.0)
            return;

        // The 32-bit address space: what is reserved or committed of the
        // 2 GB (4 GB if large-address-aware) the process can have.
        MEMORYSTATUSEX ms{ sizeof(ms) };
        GlobalMemoryStatusEx(&ms);
        const double va_used_mb = double(ms.ullTotalVirtual - ms.ullAvailVirtual) / (1024.0 * 1024.0);
        const double va_total_mb = double(ms.ullTotalVirtual) / (1024.0 * 1024.0);
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                sizeof(pmc));
        const double private_mb = double(pmc.PrivateUsage) / (1024.0 * 1024.0);

        double gpu_used = 0, gpu_budget = 0;
        const bool gpu = gpu_memory(&gpu_used, &gpu_budget);
        char gpu_text[96] = "n/a";
        if (gpu)
            sprintf_s(gpu_text, "%.0f / %.0f MB (%.0f%%)", gpu_used, gpu_budget,
                      gpu_budget > 0 ? gpu_used * 100.0 / gpu_budget : 0.0);

        log("perf: %.1f fps, worst frame %.1f ms, xrWaitFrame %.1f ms avg | address space %.0f / %.0f MB, "
            "private %.0f MB | GPU memory %s",
            g_frames / window_s, g_worst_ms, g_frames ? g_wait_ms / g_frames : 0.0, va_used_mb,
            va_total_mb, private_mb, gpu_text);

        // Largest free block (fragmentation), physical memory left, page
        // faults, handles and the process's CPU use (100% = one core; the
        // game thread is one core, so well over 100% means DXVK's workers
        // are busy too, and about 100% with a slow frame means the game
        // thread is the limit).
        SIZE_T largest_free = 0;
        {
            SYSTEM_INFO si{};
            GetSystemInfo(&si);
            const char* p = static_cast<const char*>(si.lpMinimumApplicationAddress);
            const char* end = static_cast<const char*>(si.lpMaximumApplicationAddress);
            MEMORY_BASIC_INFORMATION mbi{};
            while (p < end && VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi))
            {
                if (mbi.State == MEM_FREE && mbi.RegionSize > largest_free)
                    largest_free = mbi.RegionSize;
                const char* next = static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
                if (next <= p)
                    break;
                p = next;
            }
        }
        FILETIME created, exited, kernel, user;
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        auto ticks = [](const FILETIME& f) {
            return double((ULARGE_INTEGER{ { f.dwLowDateTime, f.dwHighDateTime } }).QuadPart) / 1e7;
        };
        const double cpu_s = ticks(kernel) + ticks(user);
        static double s_last_cpu_s = cpu_s;
        static DWORD s_last_faults = pmc.PageFaultCount;
        DWORD handles = 0;
        GetProcessHandleCount(GetCurrentProcess(), &handles);
        log("perf sys: process CPU %.0f%% (kernel %.0f%% of it), largest free block %.0f MB, "
            "physical free %.0f MB, page faults +%lu, handles %lu",
            (cpu_s - s_last_cpu_s) * 100.0 / window_s,
            cpu_s > s_last_cpu_s ? (ticks(kernel) - g_last_kernel_s) * 100.0 / (cpu_s - s_last_cpu_s) : 0.0,
            double(largest_free) / (1024.0 * 1024.0), double(ms.ullAvailPhys) / (1024.0 * 1024.0),
            (unsigned long)(pmc.PageFaultCount - s_last_faults), (unsigned long)handles);
        s_last_cpu_s = cpu_s;
        g_last_kernel_s = ticks(kernel);
        s_last_faults = pmc.PageFaultCount;

        // Mean time per frame in each stage, and the worst frame's stages.
        if (g_split_frames)
        {
            const double n = double(g_split_frames);
            log("perf split (ms/frame avg): game %.1f, hud %.1f, submit %.1f, xrWait %.1f, mirror %.1f, "
                "present %.1f | worst frame %.1f ms = game %.1f, hud %.1f, submit %.1f, xrWait %.1f, "
                "mirror %.1f, present %.1f | %u frames over 33 ms",
                g_sum[0] / n, g_sum[1] / n, g_sum[2] / n, g_sum[3] / n, g_sum[4] / n, g_sum[5] / n,
                g_worst_total, g_worst[0], g_worst[1], g_worst[2], g_worst[3], g_worst[4], g_worst[5],
                g_hitches);
        }
        sampler_report();
        for (double& s : g_sum)
            s = 0.0;
        g_split_frames = 0;
        g_hitches = 0;
        g_worst_total = 0.0;

        g_window_start = now;
        g_frames = 0;
        g_worst_ms = 0.0;
        g_wait_ms = 0.0;
    }
}
