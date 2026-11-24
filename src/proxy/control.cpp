#include "control.h"
#include "overlay.h"
#include "game_psy.h"
#include "trace.h"
#include "drawlog.h"
#include "../vr/vr_submit.h"
#include "../vr/vr_session.h"
#include "../vr/hud.h"
#include "../vr/stereo.h"

#include "../common/config.h"
#include "../common/log.h"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace psyvr
{
    namespace
    {
        enum Kind { KindNone = 0, KindFrame = 1, KindGame = 2 };

        // The one command in flight (one client, one command at a time).
        // Written by the pipe thread, taken by the executing thread, both
        // under g_lock; g_kind is also read unlocked as a cheap "anything
        // queued?" test every frame. An abandoned command (timeout) bumps
        // g_id, so a late answer is dropped.
        CRITICAL_SECTION g_lock;
        volatile LONG g_kind = KindNone;
        unsigned g_id = 0;
        std::string g_cmd;
        std::string g_reply;
        bool g_done = false;
        HANDLE g_event = nullptr;
        volatile LONG g_stop = 0;
        bool g_started = false;
        const DWORD kTimeoutMs = 10000;

        unsigned g_frame = 0;               // frames seen by control_frame_boundary

        // --- text helpers ----------------------------------------------------

        std::vector<std::string> split(const std::string& s)
        {
            std::vector<std::string> out;
            size_t i = 0;
            while (i < s.size())
            {
                while (i < s.size() && s[i] == ' ')
                    ++i;
                const size_t from = i;
                while (i < s.size() && s[i] != ' ')
                    ++i;
                if (i > from)
                    out.push_back(s.substr(from, i - from));
            }
            return out;
        }

        bool parse_float(const std::string& s, float* out)
        {
            char* end = nullptr;
            const float v = strtof(s.c_str(), &end);
            if (end == s.c_str() || *end || !std::isfinite(v))
                return false;
            *out = v;
            return true;
        }

        std::string fmt(const char* f, ...)
        {
            char buf[512];
            va_list args;
            va_start(args, f);
            vsnprintf(buf, sizeof(buf), f, args);
            va_end(args);
            return buf;
        }

        // [A-Za-z0-9_] only (and, for shot names, '-'), length lo..hi.
        bool plain_word(const std::string& s, size_t hi, bool dash)
        {
            if (s.empty() || s.size() > hi)
                return false;
            for (char c : s)
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_' || (dash && c == '-')))
                    return false;
            return true;
        }

        std::wstring widen(const std::string& s)
        {
            return std::wstring(s.begin(), s.end());    // ASCII only by now
        }

        // --- the answer ------------------------------------------------------

        // The executing thread's answer for command `id`.
        void finish(unsigned id, const std::string& reply)
        {
            EnterCriticalSection(&g_lock);
            if (id == g_id && !g_done)
            {
                g_reply = reply;
                g_done = true;
                SetEvent(g_event);
            }
            LeaveCriticalSection(&g_lock);
        }

        // Takes the queued command of this kind, if any.
        bool take(Kind kind, unsigned* id, std::string* cmd)
        {
            if (g_kind != kind)
                return false;
            bool got = false;
            EnterCriticalSection(&g_lock);
            if (g_kind == kind)
            {
                *id = g_id;
                *cmd = g_cmd;
                g_kind = KindNone;
                got = true;
            }
            LeaveCriticalSection(&g_lock);
            return got;
        }

        Kind kind_of(const std::string& verb)
        {
            if (verb == "ping" || verb == "trace" || verb == "shot" || verb == "drawlog" || verb == "set" ||
                verb == "overlay" || verb == "head")
                return KindFrame;
            if (verb == "status")
                return KindGame;
            return KindNone;
        }

        void log_result(const std::string& cmd, const std::string& reply)
        {
            const size_t nl = reply.find('\n');
            log("control: %s -> %s", cmd.c_str(), reply.substr(0, nl).c_str());
        }

        // --- shot ------------------------------------------------------------

        // Render thread only. 1 = armed (the trace starts at this boundary),
        // 2 = the traced frame is being drawn; it is captured at its Present.
        struct Shot
        {
            int state = 0;
            unsigned id = 0;
            std::string name;
            bool eyes_only = false;     // `shot <name> eyes`: no trace, no draw log
        };
        Shot g_shot;
        unsigned g_drawlog_id = 0;      // a `drawlog` command waiting for its frame to end

        bool write_png(const wchar_t* path, const unsigned char* bits, UINT pitch,
                       UINT w, UINT h)
        {
            IWICImagingFactory* factory = nullptr;
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
                return false;
            IWICStream* stream = nullptr;
            IWICBitmapEncoder* encoder = nullptr;
            IWICBitmapFrameEncode* frame = nullptr;
            IPropertyBag2* props = nullptr;
            bool ok = false;
            // The PNG encoder takes 24-bit BGR, not 32-bit BGRX (it answers
            // SetPixelFormat with another format): drop the X byte.
            std::vector<BYTE> rgb((size_t)w * 3 * h);
            for (UINT y = 0; y < h; ++y)
            {
                const unsigned char* s = bits + (size_t)y * pitch;
                BYTE* d = &rgb[(size_t)y * w * 3];
                for (UINT x = 0; x < w; ++x, s += 4, d += 3)
                {
                    d[0] = s[0];
                    d[1] = s[1];
                    d[2] = s[2];
                }
            }
            WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
            if (SUCCEEDED(factory->CreateStream(&stream)) &&
                SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
                SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
                SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
                SUCCEEDED(encoder->CreateNewFrame(&frame, &props)) &&
                SUCCEEDED(frame->Initialize(props)) &&
                SUCCEEDED(frame->SetSize(w, h)) &&
                SUCCEEDED(frame->SetPixelFormat(&format)) &&
                IsEqualGUID(format, GUID_WICPixelFormat24bppBGR) &&
                SUCCEEDED(frame->WritePixels(h, w * 3, (UINT)rgb.size(), rgb.data())) &&
                SUCCEEDED(frame->Commit()) &&
                SUCCEEDED(encoder->Commit()))
                ok = true;
            if (props)
                props->Release();
            if (frame)
                frame->Release();
            if (encoder)
                encoder->Release();
            if (stream)
                stream->Release();
            factory->Release();
            return ok;
        }

        // The eyes' images (left and right halves of the side-by-side back
        // buffer) as PNGs. Returns "" on success, else the error text.
        std::string capture_eyes(IDirect3DDevice9* device, IDirect3DSurface9* back,
                                 const std::string& name, std::string* left,
                                 std::string* right)
        {
            D3DSURFACE_DESC desc{};
            if (FAILED(back->GetDesc(&desc)))
                return "error back buffer desc";
            if (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8)
                return fmt("error back buffer format %d", (int)desc.Format);

            // A multisampled back buffer cannot be read directly: resolve it
            // into a plain render target first. Both are made per shot and
            // released at once, so a device Reset never meets one.
            IDirect3DSurface9* source = back;
            IDirect3DSurface9* resolved = nullptr;
            if (desc.MultiSampleType != D3DMULTISAMPLE_NONE)
            {
                if (FAILED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format,
                                                      D3DMULTISAMPLE_NONE, 0, FALSE,
                                                      &resolved, nullptr)) ||
                    FAILED(device->StretchRect(back, nullptr, resolved, nullptr, D3DTEXF_NONE)))
                {
                    if (resolved)
                        resolved->Release();
                    return "error msaa resolve";
                }
                source = resolved;
            }
            IDirect3DSurface9* system = nullptr;
            std::string error;
            if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                           D3DPOOL_SYSTEMMEM, &system, nullptr)))
                error = "error system surface";
            else if (FAILED(device->GetRenderTargetData(source, system)))
                error = "error GetRenderTargetData";
            else
            {
                D3DLOCKED_RECT lr{};
                if (FAILED(system->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
                    error = "error LockRect";
                else
                {
                    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    wchar_t dir[MAX_PATH]{};
                    swprintf_s(dir, L"%spsyvr_shots", exe_dir());
                    CreateDirectoryW(dir, nullptr);
                    const UINT half = desc.Width / 2;
                    const unsigned char* bits = static_cast<const unsigned char*>(lr.pBits);
                    wchar_t lp[MAX_PATH]{}, rp[MAX_PATH]{};
                    swprintf_s(lp, L"%s\\%s_L.png", dir, widen(name).c_str());
                    swprintf_s(rp, L"%s\\%s_R.png", dir, widen(name).c_str());
                    if (!write_png(lp, bits, lr.Pitch, half, desc.Height) ||
                        !write_png(rp, bits + half * 4, lr.Pitch, half, desc.Height))
                        error = "error writing png";
                    else
                    {
                        char a[MAX_PATH * 2], b[MAX_PATH * 2];
                        snprintf(a, sizeof(a), "%S", lp);
                        snprintf(b, sizeof(b), "%S", rp);
                        *left = a;
                        *right = b;
                    }
                    if (SUCCEEDED(co))
                        CoUninitialize();
                    system->UnlockRect();
                }
            }
            if (system)
                system->Release();
            if (resolved)
                resolved->Release();
            return error;
        }

        // --- the commands ----------------------------------------------------

        // Frame commands, on the render thread. Returns false when the answer
        // comes later (shot).
        bool run_frame_command(unsigned id, const std::string& cmd, std::string* reply)
        {
            const std::vector<std::string> t = split(cmd);
            const std::string& verb = t[0];
            if (verb == "ping")
            {
                *reply = fmt("ok frame %u", g_frame);
            }
            else if (verb == "trace")
            {
                int n = 0;
                if (t.size() != 2 || t[1].find_first_not_of("0123456789") != std::string::npos ||
                    t[1].size() > 3 || (n = atoi(t[1].c_str())) < 1 || n > 120)
                    *reply = "error usage: trace <1..120>";
                else
                {
                    trace_request((unsigned)n);
                    *reply = fmt("ok trace %d", n);
                }
            }
            else if (verb == "set")
            {
                // set <section> <key> <value...>
                const size_t v0 = cmd.find(' ', cmd.find(' ', 4) + 1);
                if (t.size() < 4 || v0 == std::string::npos)
                    *reply = "error usage: set <section> <key> <value>";
                else
                {
                    const std::string value = cmd.substr(v0 + 1);
                    if (!plain_word(t[1], 40, false) || !plain_word(t[2], 40, false))
                        *reply = "error bad section or key";
                    else if (value.size() > 100)
                        *reply = "error value too long";
                    else
                    {
                        config_save_setting(widen(t[1]).c_str(), widen(t[2]).c_str(),
                                            widen(value).c_str());
                        *reply = "ok";
                    }
                }
            }
            else if (verb == "overlay")
            {
                // overlay open|close: the settings panel, as the controllers' hold opens it.
                if (t.size() != 2 || (t[1] != "open" && t[1] != "close"))
                    *reply = "error usage: overlay open|close";
                else
                    *reply = overlay_request_open(t[1] == "open") ? "ok" : "error overlay not ready";
            }
            else if (verb == "head")
            {
                // head off | head <x> <y> <z> [<yaw> <pitch> <roll>]: metres in
                // the recentred room, degrees (vr_session.h: HeadScript). The
                // frame drawn after this boundary is the first with it.
                HeadScript s;
                bool ok = false;
                if (t.size() == 2 && t[1] == "off")
                    ok = true;
                else if (t.size() == 4 || t.size() == 7)
                {
                    float v[6] = {};
                    ok = true;
                    for (size_t i = 1; i < t.size() && ok; ++i)
                        ok = parse_float(t[i], &v[i - 1]);
                    for (int i = 0; i < 3 && ok; ++i)
                        ok = fabsf(v[i]) <= 5.0f && fabsf(v[i + 3]) <= 180.0f;
                    if (ok)
                    {
                        s.on = true;
                        s.offset_m[0] = v[0]; s.offset_m[1] = v[1]; s.offset_m[2] = v[2];
                        s.yaw_deg = v[3]; s.pitch_deg = v[4]; s.roll_deg = v[5];
                    }
                }
                if (!ok)
                    *reply = "error usage: head off | head <x> <y> <z> [<yaw> <pitch> <roll>] "
                             "(metres up to 5, degrees up to 180)";
                else
                {
                    vr_set_head_script(s);
                    *reply = s.on ? fmt("ok head %.3f %.3f %.3f %.1f %.1f %.1f frame %u", s.offset_m[0],
                                        s.offset_m[1], s.offset_m[2], s.yaw_deg, s.pitch_deg,
                                        s.roll_deg, g_frame)
                                  : fmt("ok head off frame %u", g_frame);
                }
            }
            else if (verb == "shot")
            {
                const bool eyes_only = t.size() == 3 && t[2] == "eyes";
                if ((t.size() != 2 && !eyes_only) || !plain_word(t[1], 40, true))
                    *reply = "error usage: shot <name> [eyes] (letters, digits, _ and -)";
                else if (!vr_submit_ready())
                    *reply = "error no vr frame to capture";
                else if (g_shot.state != 0 || trace_active() || drawlog_busy() || g_drawlog_id)
                    *reply = "error busy";
                else
                {
                    // The traced frame is the one drawn after this boundary;
                    // it is captured at the end of it (control_shot_capture).
                    g_shot.state = 1;
                    g_shot.id = id;
                    g_shot.name = t[1];
                    g_shot.eyes_only = eyes_only;
                    if (!eyes_only)
                    {
                        log("shot %s: trace begin", g_shot.name.c_str());
                        trace_request(1);
                        drawlog_request(g_shot.name.c_str());   // the same frame
                    }
                    return false;
                }
            }
            else if (verb == "drawlog")
            {
                if (t.size() != 2 || !plain_word(t[1], 40, true))
                    *reply = "error usage: drawlog <name> (letters, digits, _ and -)";
                else if (g_shot.state != 0 || drawlog_busy() || g_drawlog_id)
                    *reply = "error busy";
                else
                {
                    // The frame drawn after this boundary; answered when it
                    // ends (control_drawlog_done).
                    drawlog_request(t[1].c_str());
                    g_drawlog_id = id;
                    return false;
                }
            }
            return true;
        }

        // Game-thread commands (run from control_game_update). Returns false
        // when the answer comes later. Commands could be added that read or
        // poke Psychonauts' memory (position, camera, peek, teleport ...).
        bool run_game_command(unsigned id, const std::string& cmd, std::string* reply)
        {
            (void)id;
            const std::vector<std::string> t = split(cmd);
            const std::string& verb = t[0];
            if (verb == "status")
            {
                *reply = "ok";
                *reply += fmt("\ngame_supported %d", game_is_supported_build() ? 1 : 0);
                *reply += fmt("\nlevel %s controls_paused %d", game_level_name(),
                              game_controls_paused() ? 1 : 0);
                *reply += fmt("\nhud_menu %d scene %d", hud_showing_menu() ? 1 : 0,
                              stereo_last_frame_had_scene() ? 1 : 0);
            }
            else
                *reply = "error unknown game command";
            return true;
        }

        // --- the pipe --------------------------------------------------------

        // Queues one command line and waits for its answer.
        std::string submit(const std::string& line)
        {
            const std::vector<std::string> t = split(line);
            if (t.empty())
                return "error unknown command";
            const Kind kind = kind_of(t[0]);
            if (kind == KindNone)
            {
                const std::string reply = "error unknown command";
                log_result(line, reply);
                return reply;
            }
            EnterCriticalSection(&g_lock);
            ++g_id;
            g_cmd = line;
            g_reply.clear();
            g_done = false;
            ResetEvent(g_event);
            g_kind = kind;                  // last: now the executor may take it
            LeaveCriticalSection(&g_lock);

            WaitForSingleObject(g_event, kTimeoutMs);

            std::string reply;
            EnterCriticalSection(&g_lock);
            if (g_done)
                reply = g_reply;
            else
            {
                ++g_id;                     // drop a late answer
                g_kind = KindNone;          // or cancel it, if still queued
                reply = "error timeout";
            }
            LeaveCriticalSection(&g_lock);
            log_result(line, reply);
            return reply;
        }

        bool write_all(HANDLE pipe, const std::string& s)
        {
            DWORD done = 0;
            return WriteFile(pipe, s.data(), (DWORD)s.size(), &done, nullptr) && done == s.size();
        }

        DWORD WINAPI pipe_thread(LPVOID)
        {
            while (!g_stop)
            {
                HANDLE pipe = CreateNamedPipeW(L"\\\\.\\pipe\\psyvr", PIPE_ACCESS_DUPLEX,
                                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
                                               8192, 4096, 0, nullptr);
                if (pipe == INVALID_HANDLE_VALUE)
                {
                    log("control: CreateNamedPipe failed (%lu); control channel off", GetLastError());
                    return 0;
                }
                if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
                {
                    CloseHandle(pipe);
                    continue;
                }
                std::string buffer;
                char chunk[512];
                bool alive = true;
                while (alive && !g_stop)
                {
                    DWORD got = 0;
                    if (!ReadFile(pipe, chunk, sizeof(chunk), &got, nullptr) || got == 0)
                        break;
                    buffer.append(chunk, got);
                    size_t nl;
                    while (alive && (nl = buffer.find('\n')) != std::string::npos)
                    {
                        std::string line = buffer.substr(0, nl);
                        buffer.erase(0, nl + 1);
                        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                            line.pop_back();
                        if (line.empty())
                            continue;
                        const std::string reply = line.size() > 400 ? "error command too long"
                                                                    : submit(line);
                        alive = write_all(pipe, reply + "\n.\n");
                    }
                    if (buffer.size() > 4096)
                        break;
                }
                FlushFileBuffers(pipe);
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
            }
            return 0;
        }
    }

    void control_start()
    {
        if (g_started || !config().debug_control_pipe)
            return;
        g_started = true;
        InitializeCriticalSection(&g_lock);
        g_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE h = CreateThread(nullptr, 0, pipe_thread, nullptr, 0, nullptr);
        if (h)
            CloseHandle(h);
        log("control: pipe thread started (\\\\.\\pipe\\psyvr)");
    }

    void control_shutdown()
    {
        g_stop = 1;
        if (g_started && g_event)
            SetEvent(g_event);          // a waiting command ends; the thread is not joined
    }

    void control_frame_boundary()
    {
        if (!g_started)
            return;
        ++g_frame;
        // The shot armed at the last boundary: its traced frame was drawn
        // since, and ends at this frame's Present.
        if (g_shot.state == 1)
            g_shot.state = 2;
        unsigned id = 0;
        std::string cmd;
        if (!take(KindFrame, &id, &cmd))
            return;
        std::string reply;
        if (run_frame_command(id, cmd, &reply))
            finish(id, reply);
    }

    void control_game_update()
    {
        if (!g_started)
            return;
        unsigned id = 0;
        std::string cmd;
        if (!take(KindGame, &id, &cmd))
            return;
        std::string reply;
        if (run_game_command(id, cmd, &reply))
            finish(id, reply);
    }

    void control_drawlog_done()
    {
        if (!g_drawlog_id || drawlog_busy())
            return;
        const DrawLogResult& r = drawlog_last_result();
        const unsigned id = g_drawlog_id;
        g_drawlog_id = 0;
        if (r.ok)
            finish(id, fmt("ok frame %u draws %u\n", g_frame, r.draws) + r.path);
        else
            finish(id, "error writing the draw log");
    }

    bool control_shot_pending()
    {
        return g_shot.state == 2;
    }

    void control_shot_capture(IDirect3DDevice9* device, IDirect3DSurface9* back)
    {
        if (g_shot.state != 2)
            return;
        const Shot shot = g_shot;
        g_shot = Shot();
        std::string left, right;
        const std::string error = capture_eyes(device, back, shot.name, &left, &right);
        log("control: shot %s frame %u", shot.name.c_str(), g_frame);
        if (shot.eyes_only)
        {
            finish(shot.id, error.empty() ? fmt("ok frame %u draws 0\n", g_frame) + left + "\n" + right
                                          : error);
            return;
        }
        log("shot %s: trace end", shot.name.c_str());
        if (error.empty())
        {
            // The draw log of the same frame ended at this boundary
            // (drawlog_frame_boundary).
            const DrawLogResult& dl = drawlog_last_result();
            std::string reply = fmt("ok frame %u draws %u\n", g_frame, dl.draws) + left + "\n" + right;
            if (dl.ok)
                reply += std::string("\n") + dl.path;
            finish(shot.id, reply);
        }
        else
            finish(shot.id, error);
    }
}
