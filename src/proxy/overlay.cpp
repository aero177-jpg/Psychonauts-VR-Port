#include "overlay.h"
#include "window.h"
#include "game_psy.h"

#include "../common/config.h"
#include "../common/log.h"
#include "../vr/hud.h"
#include "../vr/vr_pad.h"
#include "../vr/vr_session.h"

#include "imgui.h"
#include "imgui_internal.h"     // the active widget's id (ImGuiContext::ActiveId)
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <windows.h>
#include <xinput.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

namespace psyvr
{
    namespace
    {
        const UINT kToggleKey = VK_INSERT;

        IDirect3DDevice9* g_device = nullptr;   // referenced by the backend
        HWND g_hwnd = nullptr;
        WNDPROC g_game_proc = nullptr;
        HHOOK g_hook = nullptr;
        bool g_unicode = false;
        bool g_ready = false;
        bool g_failed = false;
        bool g_open = false;

        // --- the panel in the headset (overlay_vr_frame) -------------------
        //
        // While VR is up the window is laid out in a square of this many
        // pixels (io.DisplaySize), filling it, and rendered into a texture
        // of that size, which goes into both eyes as a panel in the room.
        // The same draw data then goes to the desktop mirror at window
        // pixels, as without VR, so the mouse still lines up with it.
        const unsigned kPanelSize = 768;
        IDirect3DTexture9* g_panel = nullptr;
        IDirect3DSurface9* g_panel_surface = nullptr;
        bool g_panel_failed = false;
        bool g_panel_logged = false;
        // Where the panel was put in the room (panel space -> room, metres),
        // for which recentre and at what distance.
        Mat4 g_panel_to_room = Mat4::identity();
        bool g_panel_placed = false;
        unsigned g_panel_recenter = 0;
        float g_panel_distance = 0.0f;
        float g_panel_yaw = 0.0f;
        float g_panel_pitch = 0.0f;

        // This frame's ImGui frame has been built (by overlay_vr_frame), so
        // overlay_frame only draws it to the mirror; the window's client
        // size at the time, which the mirror scales from.
        bool g_frame_built = false;
        ImVec2 g_client_size(0.0f, 0.0f);
        // Set at the open edge: the frame after gives the controllers a
        // visible, focused first item to start from.
        bool g_just_opened = false;

        // The controllers (vr_pad_menu_state) as ImGui sees them this frame.
        XINPUT_GAMEPAD g_pad{};
        // Buttons held when the overlay opened (or at the last frame): only
        // a fresh press counts, so the press that was in flight neither
        // activates the first item nor closes it again.
        WORD g_held_at_open = 0;
        WORD g_prev_buttons = 0;
        // The left stick as a D-pad (hysteresis, so a stick resting near the
        // threshold does not stutter): ImGui moves its focus with the D-pad
        // only; its left stick scrolls.
        WORD g_stick_dpad = 0;

        LRESULT call_game(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            return g_unicode ? CallWindowProcW(g_game_proc, hwnd, msg, wp, lp)
                             : CallWindowProcA(g_game_proc, hwnd, msg, wp, lp);
        }

        // In front of the game's window procedure. Closed, it only watches
        // for the toggle key. Open, the mouse and keyboard stop here: the
        // game recentres the cursor on every mouse move it sees, and would
        // act on every key typed into a field.
        //
        // Two places, because the game has two: its message loop may read
        // the queue itself (PeekMessage) and act before the window procedure
        // is ever called, so queued input is taken in a WH_GETMESSAGE hook,
        // which sees each message first and can blank it. The window
        // procedure is still replaced for what is sent, not queued, and as
        // the fallback if the hook cannot be installed.
        bool is_mouse(UINT msg)
        {
            return msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;
        }

        bool is_key(UINT msg)
        {
            return msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR ||
                   msg == WM_INPUT;
        }

        void set_open(bool open)
        {
            g_open = open;
            if (g_ready)
                ImGui::GetIO().MouseDrawCursor = g_open;
            window_block_game_mouse(g_open);
            log("overlay: %s", g_open ? "opened" : "closed");
        }

        // True if the message was the overlay's and the game must not see it.
        bool take_input(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            if (msg == WM_KEYDOWN && wp == kToggleKey)
            {
                if (!(lp & 0x40000000))
                    set_open(!g_open);
                return true;
            }
            if (!g_open || !g_ready || (!is_mouse(msg) && !is_key(msg)))
                return false;
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
            return true;
        }

        LRESULT CALLBACK overlay_get_message(int code, WPARAM wp, LPARAM lp)
        {
            MSG* msg = reinterpret_cast<MSG*>(lp);
            if (code >= 0 && wp == PM_REMOVE && msg && g_hwnd &&
                (msg->hwnd == g_hwnd || IsChild(g_hwnd, msg->hwnd)))
            {
                // Typed characters come from TranslateMessage, which the
                // game's loop will not reach for a blanked key.
                if (g_open && g_ready && msg->message == WM_KEYDOWN &&
                    msg->wParam != kToggleKey)
                    TranslateMessage(msg);
                if (take_input(msg->hwnd, msg->message, msg->wParam,
                               msg->lParam))
                    msg->message = WM_NULL;
            }
            return CallNextHookEx(g_hook, code, wp, lp);
        }

        LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            if (take_input(hwnd, msg, wp, lp))
                return 0;
            if (g_open && g_ready)
                ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
            return call_game(hwnd, msg, wp, lp);
        }

        bool init(IDirect3DSwapChain9* chain)
        {
            if (g_ready)
                return true;
            if (g_failed || !chain)
                return false;

            IDirect3DDevice9* device = nullptr;
            if (FAILED(chain->GetDevice(&device)) || !device)
                return false;

            HWND hwnd = window_handle();
            if (!hwnd)
            {
                D3DDEVICE_CREATION_PARAMETERS params{};
                if (SUCCEEDED(device->GetCreationParameters(&params)))
                    hwnd = params.hFocusWindow;
            }
            if (!hwnd || !IsWindow(hwnd))
            {
                device->Release();
                return false;       // no window yet; try again next frame
            }

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            // Window positions are not kept: another imgui.ini beside the
            // game would be shared with any other ImGui tool loaded there.
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;

            // Something more legible than the built-in bitmap font, if
            // Windows has it where it always has.
            wchar_t windir[MAX_PATH]{};
            char font[MAX_PATH * 3]{};
            if (GetWindowsDirectoryW(windir, MAX_PATH))
            {
                wchar_t wide[MAX_PATH]{};
                swprintf_s(wide, L"%s\\Fonts\\segoeui.ttf", windir);
                if (GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES)
                    WideCharToMultiByte(CP_UTF8, 0, wide, -1, font,
                                        (int)sizeof(font), nullptr, nullptr);
            }
            if (!font[0] || !io.Fonts->AddFontFromFileTTF(font, 18.0f))
                io.Fonts->AddFontDefault();
            ImGui::StyleColorsDark();

            if (!ImGui_ImplWin32_Init(hwnd) || !ImGui_ImplDX9_Init(device))
            {
                log("overlay: Dear ImGui would not start; no settings overlay");
                ImGui::DestroyContext();
                device->Release();
                g_failed = true;
                return false;
            }

            // The VR controllers navigate it (feed_gamepad). The Win32
            // backend's own XInput polling is compiled out (build.bat
            // IMGUI_IMPL_WIN32_DISABLE_GAMEPAD): it would read only a real
            // pad, and fight this feed over the same keys.
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
            io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

            g_unicode = IsWindowUnicode(hwnd) != FALSE;
            g_game_proc = reinterpret_cast<WNDPROC>(g_unicode
                ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                    reinterpret_cast<LONG_PTR>(&overlay_proc))
                : SetWindowLongPtrA(hwnd, GWLP_WNDPROC,
                                    reinterpret_cast<LONG_PTR>(&overlay_proc)));
            if (!g_game_proc)
            {
                log("overlay: could not get in front of the game's window "
                    "procedure; no settings overlay");
                ImGui_ImplDX9_Shutdown();
                ImGui_ImplWin32_Shutdown();
                ImGui::DestroyContext();
                device->Release();
                g_failed = true;
                return false;
            }

            g_device = device;      // the backend took its own reference
            device->Release();
            g_hwnd = hwnd;
            g_hook = SetWindowsHookExW(WH_GETMESSAGE, &overlay_get_message,
                nullptr, GetWindowThreadProcessId(hwnd, nullptr));
            if (!g_hook)
                log("overlay: no message hook (error %lu); input is taken at "
                    "the window procedure only", GetLastError());
            g_ready = true;
            log("overlay: settings overlay ready -- Insert opens it on the "
                "desktop window");
            return true;
        }

        void release_panel()
        {
            if (g_panel_surface)
                g_panel_surface->Release();
            if (g_panel)
                g_panel->Release();
            g_panel_surface = nullptr;
            g_panel = nullptr;
        }

        bool ensure_panel()
        {
            if (g_panel_surface)
                return true;
            if (g_panel_failed || !g_device)
                return false;
            const HRESULT hr = g_device->CreateTexture(kPanelSize, kPanelSize, 1,
                                                       D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                                       D3DPOOL_DEFAULT, &g_panel, nullptr);
            if (FAILED(hr) || !g_panel || FAILED(g_panel->GetSurfaceLevel(0, &g_panel_surface)))
            {
                log("overlay: could not create the %ux%u VR panel target (0x%08lX); the "
                    "settings overlay stays on the desktop only", kPanelSize, kPanelSize, hr);
                g_panel_failed = true;
                release_panel();
                return false;
            }
            return true;
        }

        // The open and close edges, seen once a frame by whichever of
        // overlay_vr_frame / overlay_frame runs first. set_open() may run
        // from the window procedure at any time; this is where the rest of
        // the mod hears of it.
        void sync_open()
        {
            static bool was_open = false;
            if (g_open == was_open)
                return;
            was_open = g_open;
            if (g_open)
            {
                g_just_opened = true;
                g_panel_placed = false;     // in front of the head again
                g_held_at_open = 0xFFFF;    // everything counts as held until let go
            }
            else
            {
                // Closed with the window's own close button too: give the
                // mouse back.
                ImGui::GetIO().MouseDrawCursor = false;
                window_block_game_mouse(false);
            }
            vr_pad_block_game(g_open);
        }

        // Once a frame while open: the controllers' pad, with presses that
        // began before the overlay opened masked until released. The menu
        // (Start) button closes it. False if it just closed.
        bool poll_pad()
        {
            XINPUT_GAMEPAD pad{};
            vr_pad_menu_state(&pad);
            g_held_at_open &= pad.wButtons;
            pad.wButtons &= ~g_held_at_open;
            const WORD pressed = pad.wButtons & ~g_prev_buttons;
            g_prev_buttons = pad.wButtons;
            if (pressed & XINPUT_GAMEPAD_START)
            {
                set_open(false);
                sync_open();
                return false;
            }

            // The left stick as a D-pad: in at 0.5, out at 0.3.
            const float lx = pad.sThumbLX / 32767.0f, ly = pad.sThumbLY / 32767.0f;
            const struct { WORD bit; float v; } dirs[] = {
                { XINPUT_GAMEPAD_DPAD_LEFT, -lx },  { XINPUT_GAMEPAD_DPAD_RIGHT, lx },
                { XINPUT_GAMEPAD_DPAD_DOWN, -ly },  { XINPUT_GAMEPAD_DPAD_UP, ly },
            };
            for (const auto& d : dirs)
            {
                if (d.v > 0.5f)
                    g_stick_dpad |= d.bit;
                else if (d.v < 0.3f)
                    g_stick_dpad &= ~d.bit;
            }
            g_pad = pad;
            return true;
        }

        // g_pad into ImGui. ImGui 1.91 moves its focus with the D-pad (and
        // tweaks an active slider with it) and scrolls with its left stick;
        // the Quest's controllers have no D-pad, so the left stick feeds the
        // D-pad keys (poll_pad) and the right stick ImGui's left-stick keys
        // (scrolling), as well as its own.
        void feed_gamepad(ImGuiIO& io)
        {
            io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
            const WORD buttons = g_pad.wButtons | g_stick_dpad;
            const struct { ImGuiKey key; WORD bit; } map[] = {
                { ImGuiKey_GamepadFaceDown, XINPUT_GAMEPAD_A },
                { ImGuiKey_GamepadFaceRight, XINPUT_GAMEPAD_B },
                { ImGuiKey_GamepadFaceLeft, XINPUT_GAMEPAD_X },
                { ImGuiKey_GamepadFaceUp, XINPUT_GAMEPAD_Y },
                { ImGuiKey_GamepadL1, XINPUT_GAMEPAD_LEFT_SHOULDER },
                { ImGuiKey_GamepadR1, XINPUT_GAMEPAD_RIGHT_SHOULDER },
                { ImGuiKey_GamepadDpadLeft, XINPUT_GAMEPAD_DPAD_LEFT },
                { ImGuiKey_GamepadDpadRight, XINPUT_GAMEPAD_DPAD_RIGHT },
                { ImGuiKey_GamepadDpadUp, XINPUT_GAMEPAD_DPAD_UP },
                { ImGuiKey_GamepadDpadDown, XINPUT_GAMEPAD_DPAD_DOWN },
                { ImGuiKey_GamepadBack, XINPUT_GAMEPAD_BACK },
            };
            for (const auto& m : map)
                io.AddKeyEvent(m.key, (buttons & m.bit) != 0);

            // Past XInput's own dead zone, 0..1 (as the Win32 backend does).
            const auto analog = [&io](ImGuiKey key, int v) {
                const float dead = (float)XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
                float n = ((float)v - dead) / (32767.0f - dead);
                n = n < 0.0f ? 0.0f : n > 1.0f ? 1.0f : n;
                io.AddKeyAnalogEvent(key, n > 0.1f, n);
            };
            const int rx = g_pad.sThumbRX, ry = g_pad.sThumbRY;
            analog(ImGuiKey_GamepadLStickLeft, -rx);
            analog(ImGuiKey_GamepadLStickRight, rx);
            analog(ImGuiKey_GamepadLStickUp, ry);
            analog(ImGuiKey_GamepadLStickDown, -ry);
            analog(ImGuiKey_GamepadRStickLeft, -rx);
            analog(ImGuiKey_GamepadRStickRight, rx);
            analog(ImGuiKey_GamepadRStickUp, ry);
            analog(ImGuiKey_GamepadRStickDown, -ry);
        }

        // --- widgets ------------------------------------------------------
        //
        // None of these touches the Config. Each shows the current value,
        // and when the user changes it writes psyvr.ini and asks for the
        // live reload, which the game thread runs at its next frame
        // boundary (before the next overlay frame reads the value back).

        // Slider drags write at most this often; releasing always writes.
        const ULONGLONG kWriteIntervalMs = 100;
        ULONGLONG g_last_write = 0;

        bool write_due(bool force)
        {
            const ULONGLONG now = GetTickCount64();
            if (!force && now - g_last_write < kWriteIntervalMs)
                return false;
            g_last_write = now;
            return true;
        }

        void save_float(const wchar_t* section, const wchar_t* key, float v)
        {
            wchar_t text[32]{};
            swprintf_s(text, L"%.4g", v);
            config_save_setting(section, key, text);
            log("overlay: saved [%ls] %ls = %ls", section, key, text);
        }

        void help(const char* text)
        {
            if (!text || !ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                return;
            // Wrapped to about two lines (user 2026-11-25: a long one ran off
            // the window), never wider than the screen allows.
            const float full = ImGui::CalcTextSize(text).x;
            const float screen = ImGui::GetIO().DisplaySize.x;
            const float pad = ImGui::GetStyle().WindowPadding.x * 2.0f;
            float wrap = full * 0.5f + ImGui::GetFontSize() * 2.0f;
            wrap = fmaxf(wrap, ImGui::GetFontSize() * 12.0f);
            wrap = fminf(wrap, fmaxf(screen - pad - 8.0f, ImGui::GetFontSize() * 8.0f));
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(wrap);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }

        // `min_nonzero` is for the settings where 0 means off and anything
        // else has a floor (view_switch_hold, cull_fov_degrees): a value
        // between is snapped, so the file never holds one the loader would
        // warn about.
        void slider(const char* label, float current, float low, float high,
                    const char* format, const wchar_t* section,
                    const wchar_t* key, const char* tip,
                    float min_nonzero = 0.0f, bool logarithmic = false)
        {
            // While dragged, the slider keeps its own value: the loaded one
            // lags the drag by a frame.
            ImGuiStorage* store = ImGui::GetStateStorage();
            const ImGuiID id = ImGui::GetID(label);
            float v = ImGui::GetCurrentContext()->ActiveId == id ? store->GetFloat(id, current)
                                                 : current;
            ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp;
            if (logarithmic)
                flags |= ImGuiSliderFlags_Logarithmic;
            const bool changed = ImGui::SliderFloat(label, &v, low, high,
                                                    format, flags);
            const bool done = ImGui::IsItemDeactivatedAfterEdit();
            store->SetFloat(id, v);
            if (min_nonzero > 0.0f)
            {
                if (v < min_nonzero * 0.5f)
                    v = 0.0f;
                else if (v < min_nonzero)
                    v = min_nonzero;
            }
            if ((changed || done) && write_due(done))
                save_float(section, key, v);
            help(tip);
        }

        // Whole-number setting. As slider(): while dragged the slider keeps its
        // own value (re-reading the loaded one each frame snapped it back, so
        // it never moved: "stuck at 4", user 2026-11-16).
        // `zero_text`: what 0 shows as (e.g. "auto"), if it means something.
        void slider_int(const char* label, int current, int low, int high, const char* format,
                        const wchar_t* section, const wchar_t* key, const char* tip,
                        const char* zero_text = nullptr)
        {
            ImGuiStorage* store = ImGui::GetStateStorage();
            const ImGuiID id = ImGui::GetID(label);
            int v = ImGui::GetCurrentContext()->ActiveId == id ? store->GetInt(id, current) : current;
            const char* shown = v == 0 && zero_text ? zero_text : format;
            const bool changed = ImGui::SliderInt(label, &v, low, high, shown, ImGuiSliderFlags_AlwaysClamp);
            const bool done = ImGui::IsItemDeactivatedAfterEdit();
            store->SetInt(id, v);
            if ((changed || done) && v != current && write_due(done))
            {
                wchar_t text[16];
                swprintf_s(text, L"%d", v);
                config_save_setting(section, key, text);
                log("overlay: saved [%ls] %ls = %d", section, key, v);
            }
            help(tip);
        }

        void toggle(const char* label, bool current, const wchar_t* section,
                    const wchar_t* key, const char* tip)
        {
            bool v = current;
            if (ImGui::Checkbox(label, &v))
            {
                config_save_setting(section, key, v ? L"1" : L"0");
                log("overlay: saved [%ls] %ls = %d", section, key, v ? 1 : 0);
            }
            help(tip);
        }

        // A setting with a few named values 0..n-1.
        void choice(const char* label, int current, const char* const* names, int n,
                    const wchar_t* section, const wchar_t* key, const char* tip)
        {
            int v = current;
            if (ImGui::Combo(label, &v, names, n) && v != current)
            {
                wchar_t text[16]{};
                swprintf_s(text, L"%d", v);
                config_save_setting(section, key, text);
                log("overlay: saved [%ls] %ls = %d", section, key, v);
            }
            help(tip);
        }

        // --- Controller guide: what each Quest 3 (Touch) input does -----------
        // Built from [controls] and the gestures in vr_pad.cpp / dpad_gesture.h,
        // so it follows the settings as they change. Keep in step with them.
        // After the Tomb Raider: Legend mod's button layout.
        enum BindInput
        {
            BindLeftStick, BindLeftClick, BindMenu, BindX, BindY,
            BindLeftTrigger, BindLeftGrip,
            BindRightStick, BindRightClick, BindA, BindB,
            BindRightTrigger, BindRightGrip,
            BindBothSticks, BindRightMenu,
            BindCount
        };

        struct Binding
        {
            const char* input = nullptr;
            char brief[56] = {};    // the drawing's label ("" = unused)
            char full[300] = {};    // the table's text ("" = row hidden)
        };

        void append(char* dst, size_t size, const char* text)
        {
            const size_t len = strlen(dst);
            if (len + 1 < size)
                snprintf(dst + len, size - len, "%s", text);
        }

        // The pad buttons a physical control presses, from [controls]
        // ("A", "LB + L3"). Not Start (the pause text covers it), R3 (the
        // right stick click's gestures do) or anything on the D-pad modifier
        // (a modifier is not also a button).
        void pad_names(const ControlMap& m, ControlSource src, char* out, size_t size)
        {
            out[0] = 0;
            if (src == SrcNone || src == m.dpad_modifier)
                return;
            const struct { const char* name; ControlSource src; } list[] = {
                { "A", m.a }, { "B", m.b }, { "X", m.x }, { "Y", m.y },
                { "Back", m.back }, { "LB", m.lb }, { "RB", m.rb }, { "L3", m.l3 },
            };
            for (const auto& e : list)
                if (e.src == src)
                {
                    if (out[0])
                        append(out, size, " + ");
                    append(out, size, e.name);
                }
        }

        // view: 0 classic, 1 god mode, 2 first person.
        void describe(const Config& c, int view, ControlSource src, const char* input,
                      Binding& b)
        {
            const ControlMap& m = c.controls;
            char names[48];
            pad_names(m, src, names, sizeof(names));
            b.input = input;
            b.brief[0] = b.full[0] = 0;
            const auto part = [&](const char* brief, const char* full) {
                if (b.brief[0])
                    append(b.brief, sizeof(b.brief), ". ");
                append(b.brief, sizeof(b.brief), brief);
                if (b.full[0])
                    append(b.full, sizeof(b.full), " ");
                append(b.full, sizeof(b.full), full);
            };
            char t[200];
            if (src != SrcNone && src == m.dpad_modifier)
                part("D-pad: click, slide",
                     "Click, then slide the stick: the D-pad (left = the item wheel, "
                     "up / down = camera zoom). Raz stays still meanwhile, and the "
                     "pause button presses Back.");
            if (src != SrcNone && src == m.start)
            {
                if (c.overlay_vr_open_hold > 0.0f)
                {
                    snprintf(t, sizeof(t), "Pause. Hold %.1f s: this settings menu.",
                             c.overlay_vr_open_hold);
                    part("Pause. Hold: settings", t);
                }
                else
                    part("Pause", "Pause.");
            }
            if (src != SrcNone && src == m.r3)
            {
                const float hold = m.god_toggle_hold;
                if (view == 2)
                {
                    if (hold > 0.0f)
                    {
                        snprintf(t, sizeof(t), "Hold %.1f s: switch view (back to classic).", hold);
                        part("Hold: switch view", t);
                    }
                    else
                        part("-", "Nothing in this view.");
                }
                else
                {
                    const char* tap = "Tap: the camera glides back behind Raz.";
                    const char* push = view == 1
                        ? "Click and push: left / right = Raz's size, up / down = the seat's height."
                        : "Click and push: left / right = camera distance, up / down = view height.";
                    if (hold > 0.0f)
                    {
                        snprintf(t, sizeof(t), "%s Hold %.1f s: switch view (classic, god mode%s). %s",
                                 tap, hold, m.first_person_in_cycle ? ", first person" : "", push);
                        part("Tap: reset. Hold: view", t);
                    }
                    else
                    {
                        snprintf(t, sizeof(t), "%s %s", tap, push);
                        part("Tap: reset", t);
                    }
                }
            }
            if (names[0])
            {
                snprintf(t, sizeof(t), "Pad %s.", names);
                part(names, t);
            }
            if (!b.brief[0])
                part("-", "Nothing.");
        }

        void fixed(Binding& b, const char* input, const char* brief, const char* full)
        {
            b.input = input;
            snprintf(b.brief, sizeof(b.brief), "%s", brief);
            snprintf(b.full, sizeof(b.full), "%s", full);
        }

        void build_bindings(const Config& c, int view, Binding out[BindCount])
        {
            fixed(out[BindLeftStick], "Left stick", "Move", "Move.");
            describe(c, view, SrcLeftStick, "Left stick click", out[BindLeftClick]);
            describe(c, view, SrcLeftMenu, "Menu (left)", out[BindMenu]);
            describe(c, view, SrcLeftPrimary, "X", out[BindX]);
            describe(c, view, SrcLeftSecondary, "Y", out[BindY]);
            fixed(out[BindLeftTrigger], "Left trigger", "LT", "Pad LT.");
            describe(c, view, SrcLeftGrip, "Left grip", out[BindLeftGrip]);

            fixed(out[BindRightStick], "Right stick",
                  view == 1 ? "Turn / zoom" : view == 2 ? "Look" : "Camera",
                  view == 1 ? "Turn the view round Raz (left / right) and zoom (up / down)."
                  : view == 2 ? "Look round." : "Turn the camera.");
            describe(c, view, SrcRightStick, "Right stick click", out[BindRightClick]);
            describe(c, view, SrcRightPrimary, "A", out[BindA]);
            describe(c, view, SrcRightSecondary, "B", out[BindB]);
            fixed(out[BindRightTrigger], "Right trigger", "RT", "Pad RT.");
            describe(c, view, SrcRightGrip, "Right grip", out[BindRightGrip]);

            fixed(out[BindBothSticks], "Both stick clicks", "", "Recentre the view.");
            describe(c, view, SrcRightMenu, "Menu (right)", out[BindRightMenu]);
            // Touch controllers have no right menu button.
            if (out[BindRightMenu].full[0] && strcmp(out[BindRightMenu].brief, "-") != 0)
                append(out[BindRightMenu].full, sizeof(out[BindRightMenu].full),
                       " (Touch has no right menu button.)");
            else
                out[BindRightMenu].full[0] = 0;
        }

        // A Quest 3 controller pair seen from above, drawn with ImGui shapes
        // and labelled from the bindings; sizes follow the font so it scales
        // with the window.
        void draw_controllers(const Binding b[BindCount])
        {
            const float s = ImGui::GetFontSize() / 13.0f;
            const float width = ImGui::GetContentRegionAvail().x;
            const float height = 228.0f * s;
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            // Opaque: the style's colours are see-through, and the shell's
            // overlapping shapes would show their seams.
            const auto opaque = [](ImGuiCol col, float shade) {
                ImVec4 v = ImGui::GetStyleColorVec4(col);
                return ImGui::ColorConvertFloat4ToU32(
                    ImVec4(v.x * shade, v.y * shade, v.z * shade, 1.0f));
            };
            const ImU32 body = opaque(ImGuiCol_FrameBg, 1.0f);
            const ImU32 part = opaque(ImGuiCol_Button, 0.8f);
            const ImU32 edge = ImGui::GetColorU32(ImGuiCol_Border, 2.0f);
            const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
            const ImU32 lead = ImGui::GetColorU32(ImGuiCol_TextDisabled);
            const float mid = p0.x + width * 0.5f;
            const float face_r = 40.0f * s;
            const float cy = p0.y + 76.0f * s;

            for (int hand = 0; hand < 2; ++hand)
            {
                // inner: +1 toward the other controller.
                const float inner = hand == 0 ? 1.0f : -1.0f;
                const float cx = mid - inner * 58.0f * s;
                const ImVec2 c(cx, cy);
                const auto at = [&](float dx, float dy) {
                    return ImVec2(cx + inner * dx * s, cy + dy * s);
                };

                // Under the shell: trigger at the front, grip on the inner
                // side of the handle.
                dl->AddRectFilled(ImVec2(cx - 14.0f * s, cy - face_r - 9.0f * s),
                                  ImVec2(cx + 14.0f * s, cy - face_r + 8.0f * s),
                                  part, 5.0f * s);
                const ImVec2 g0 = at(16.0f, 36.0f), g1 = at(27.0f, 66.0f);
                dl->AddRectFilled(ImVec2(fminf(g0.x, g1.x), g0.y),
                                  ImVec2(fmaxf(g0.x, g1.x), g1.y),
                                  part, 4.0f * s);

                // The shell: a round face and a tapering handle.
                const ImVec2 handle[4] = { at(-24.0f, 20.0f), at(24.0f, 20.0f),
                                           at(17.0f, 112.0f), at(-17.0f, 112.0f) };
                dl->AddConvexPolyFilled(handle, 4, body);
                dl->AddCircleFilled(at(0.0f, 112.0f), 17.0f * s, body);
                dl->AddCircleFilled(c, face_r, body, 48);
                dl->AddCircle(c, face_r, edge, 48, 1.0f);

                // Face, as the Quest 3: the stick toward the outside, the
                // two buttons toward the inside, the menu / Meta button
                // below the stick.
                const ImVec2 stick = at(-19.0f, -12.0f);
                dl->AddCircleFilled(stick, 13.0f * s, part, 24);
                dl->AddCircle(stick, 13.0f * s, edge, 24, 1.0f);
                dl->AddCircleFilled(stick, 7.0f * s, body, 16);
                const ImVec2 top_button = at(12.0f, -13.0f);
                const ImVec2 low_button = at(0.0f, 8.0f);
                const ImVec2 small_button = at(-23.0f, 14.0f);
                dl->AddCircleFilled(top_button, 7.5f * s, part, 20);
                dl->AddCircleFilled(low_button, 7.5f * s, part, 20);
                dl->AddCircleFilled(small_button, 3.5f * s, part, 12);
                const char* top_name = hand == 0 ? "Y" : "B";
                const char* low_name = hand == 0 ? "X" : "A";
                const ImVec2 tn = ImGui::CalcTextSize(top_name);
                dl->AddText(ImVec2(top_button.x - tn.x * 0.5f,
                                   top_button.y - tn.y * 0.5f), ink, top_name);
                const ImVec2 ln = ImGui::CalcTextSize(low_name);
                dl->AddText(ImVec2(low_button.x - ln.x * 0.5f,
                                   low_button.y - ln.y * 0.5f), ink, low_name);

                // Labels on the outer side, one row each, with a leader to
                // the part.
                struct Label { ImVec2 anchor; const char* text; };
                const Label labels[7] = {
                    { ImVec2(cx, cy - face_r - 5.0f * s),
                      b[hand == 0 ? BindLeftTrigger : BindRightTrigger].brief },
                    { top_button, b[hand == 0 ? BindY : BindB].brief },
                    { low_button, b[hand == 0 ? BindX : BindA].brief },
                    { stick, b[hand == 0 ? BindLeftStick : BindRightStick].brief },
                    { stick, b[hand == 0 ? BindLeftClick : BindRightClick].brief },
                    { small_button, hand == 0 ? b[BindMenu].brief : "System" },
                    { ImVec2((g0.x + g1.x) * 0.5f, (g0.y + g1.y) * 0.5f),
                      b[hand == 0 ? BindLeftGrip : BindRightGrip].brief },
                };
                static const char* const kPrefix[7] = { "Trigger: ", "",
                    "", "Stick: ", "Click: ", "", "Grip: " };
                const float column = cx - inner * (face_r + 14.0f * s);
                for (int i = 0; i < 7; ++i)
                {
                    char text[96];
                    snprintf(text, sizeof(text), "%s%s", kPrefix[i],
                             labels[i].text[0] ? labels[i].text : "-");
                    // Two lines when it does not fit beside the controller:
                    // split at the last space that fits.
                    const float room = inner > 0.0f
                        ? column - 3.0f * s - p0.x
                        : p0.x + width - column - 3.0f * s;
                    const char* split = nullptr;
                    if (ImGui::CalcTextSize(text).x > room)
                        for (const char* q = text; *q; ++q)
                            if (*q == ' ' &&
                                ImGui::CalcTextSize(text, q).x <= room)
                                split = q;
                    const float line = ImGui::GetTextLineHeight();
                    const float y = p0.y + (6.0f + i * 31.0f) * s;
                    const float lines = split ? 2.0f : 1.0f;
                    dl->AddLine(labels[i].anchor,
                                ImVec2(column + inner * 3.0f * s,
                                       y + line * lines * 0.5f), lead, 1.0f);
                    const char* parts[2][2] = { { text, split ? split : nullptr },
                                                { split ? split + 1 : nullptr,
                                                  nullptr } };
                    for (int k = 0; k < (split ? 2 : 1); ++k)
                    {
                        const char* from = parts[k][0];
                        const char* to = parts[k][1];
                        const float w = ImGui::CalcTextSize(from, to).x;
                        const float x = inner > 0.0f
                            ? column - 3.0f * s - w : column + 3.0f * s;
                        dl->AddText(ImVec2(x, y + k * line), ink, from, to);
                    }
                }
            }
            ImGui::Dummy(ImVec2(width, height));
        }

        void controls_section(const Config& c)
        {
            static int view = -1;
            if (view < 0)
                view = c.god_mode ? 1 : 0;
            static const char* const kViews[] = { "Classic", "God mode", "First person" };
            ImGui::Combo("Show for", &view, kViews, 3);
            help("The view the guide is for: the right stick and its click do "
                 "different things in each.");
            Binding b[BindCount];
            build_bindings(c, view, b);
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("Meta Quest 3 (Touch). The game sees an Xbox pad, so its own "
                               "button prompts apply; this is which control presses what, "
                               "and what the mod adds. It follows [controls] in psyvr.ini.");
            ImGui::PopStyleColor();
            draw_controllers(b);
            if (ImGui::BeginTable("##bindings", 2,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Input", ImGuiTableColumnFlags_WidthFixed,
                                        ImGui::CalcTextSize("Right stick click  ").x);
                ImGui::TableSetupColumn("Action");
                ImGui::TableHeadersRow();
                for (int i = 0; i < BindCount; ++i)
                {
                    if (!b[i].input || !b[i].full[0])
                        continue;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(b[i].input);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextWrapped("%s", b[i].full);
                }
                ImGui::EndTable();
            }
        }

        void vr_section(const Config& c)
        {
            {
                static const char* const kModes[] = { "off", "auto", "on" };
                static const wchar_t* const kModeNames[] = { L"off", L"auto", L"on" };
                int sel = c.horizon_lock >= 0 && c.horizon_lock <= 2 ? c.horizon_lock : 1;
                if (ImGui::Combo("Horizon lock", &sel, kModes, 3))
                {
                    config_save_setting(L"vr", L"horizon_lock", kModeNames[sel]);
                    log("overlay: saved [vr] horizon_lock = %ls", kModeNames[sel]);
                }
                help("The view keeps the game camera's position and turn but not "
                     "its tilt up/down or roll: look down with your head instead. "
                     "Auto = locked except in the levels listed below; on = locked "
                     "everywhere; off = the game's camera as it is (it pitches and swoops).");
            }
            ImGui::TextWrapped("Level %s; auto leaves horizon lock off in: %ls ([vr] "
                               "horizon_lock_off_levels in psyvr.ini).",
                               game_level_name()[0] ? game_level_name() : "(none)",
                               c.horizon_lock_off_levels);
            slider("World scale", c.world_scale, 10.0f, 5000.0f, "%.0f",
                   L"vr", L"world_scale",
                   "Game units per metre. 100 = life size (Raz is 125 units, "
                   "1.25 m tall). Higher makes the world smaller, like a "
                   "diorama; lower makes it bigger.", 0.0f, true);
            slider("Cull field of view", c.cull_fov_degrees, 0.0f, 179.0f,
                   "%.0f deg", L"vr", L"cull_fov_degrees",
                   "Vertical field of view the game draws for, in degrees (its "
                   "own is about 52). Wider keeps things above and below drawn "
                   "when you look there. Applies at the next level load. 0 = the "
                   "game's own.", 10.0f);
        }

        void theatre_section(const Config& c)
        {
            static const char* const kModes[] = { "off", "cutscenes", "always" };
            choice("Theatre", c.theatre_mode, kModes, 3, L"theatre", L"mode",
                   "The game as in the flat game, on a screen floating in a dark room, "
                   "still in 3D. The screen, its picture and the HUD stay where they are "
                   "when you move your head. Cutscenes = only cutscenes and scripted "
                   "camera shots; always = gameplay too (horizon lock and god mode then "
                   "show the game's own camera). The screen appears in front of you "
                   "when it opens; recentre to bring it back.");
            static const char* const kStyles[] = { "screen", "window" };
            choice("Theatre style", c.theatre_style, kStyles, 2, L"theatre", L"style",
                   "Screen = a 3D screen: the game's shot in stereo, fixed; moving your head "
                   "does not change the picture. Window = a window into the game: the same "
                   "shot seen from where you sat when it appeared, and leaning or turning "
                   "looks into the scene and round the window's edges.");
            ImGui::Text("Showing now: %s", game_theatre_showing() ? "yes" : "no");
            slider("Screen distance", c.theatre_distance, 0.3f, 50.0f, "%.2f m", L"theatre",
                   L"distance", "Metres from your head to the screen.", 0.0f, true);
            slider("Screen width", c.theatre_width_degrees, 10.0f, 150.0f, "%.0f deg", L"theatre",
                   L"width_degrees", "How wide the screen looks from where it was placed.");
            {
                // Presets for [theatre] aspect; the slider below is the same
                // setting, for any other shape.
                static const char* const kShapes[] = { "16:9 (wide)", "3:2", "4:3 (the game's backdrops)", "custom" };
                static const float kShapeValue[] = { 1.78f, 1.5f, 1.333f };
                static const wchar_t* const kShapeText[] = { L"1.78", L"1.5", L"1.333" };
                int sel = 3;
                for (int i = 0; i < 3; ++i)
                    if (fabsf(c.theatre_aspect - kShapeValue[i]) < 0.01f)
                        sel = i;
                const int was = sel;
                if (ImGui::Combo("Screen shape", &sel, kShapes, 4) && sel != was && sel < 3)
                {
                    config_save_setting(L"theatre", L"aspect", kShapeText[sel]);
                    log("overlay: saved [theatre] aspect = %ls", kShapeText[sel]);
                }
                help("The theatre screen's shape. 16:9 fits the game's 3D view; the "
                     "game's loading screens and other backdrops are 4:3, so they leave "
                     "bars on a 16:9 screen. Custom: set it with the slider below.");
            }
            slider("Screen shape (custom)", c.theatre_aspect, 0.5f, 4.0f, "%.2f", L"theatre", L"aspect",
                   "Width:height: 1.78 = 16:9, 1.5 = 3:2, 1.33 = 4:3.");
            slider("Depth", c.theatre_depth, 0.0f, 3.0f, "%.2f", L"theatre", L"depth",
                   "How strong the 3D is: the eyes' separation in the game times this. "
                   "1 = life size; 0 = flat on the screen; above 1 near things get "
                   "deeper while far things stay far.");
        }

        void camera_section(const Config& c)
        {
            slider("Camera distance", c.chase_radius_scale, 0.1f, 3.0f, "x %.2f",
                   L"camera", L"chase_radius_scale",
                   "How far the camera follows behind Raz, times the game's "
                   "own (about 7 m). Below 1 = closer. Live; also zoom while playing "
                   "with D-pad up/down, or click the right VR stick in and push it "
                   "right (closer) / left (farther); pushed up/down it raises or "
                   "lowers the view (not in menus).");
            slider("Start menu zoom", c.start_menu_zoom, 0.0f, 5.0f, "%.2f m",
                   L"camera", L"start_menu_zoom",
                   "In the start menu, how many metres nearer its 3D scene "
                   "(Raz and the brain) you are. 0 = where the game's camera is. "
                   "Also live in the start menu: right stick up = nearer, down = farther.");
            slider("Start menu near limit", c.start_menu_near_limit, 0.0f, 10.0f, "%.1f m",
                   L"camera", L"start_menu_near_limit",
                   "In the start menu, the nearest the floating layers (doodles, "
                   "credit bubbles) come; one that would be nearer is pushed back "
                   "to this. 0 = off.");
            toggle("First person stays", c.first_person_stays, L"camera", L"first_person_stays",
                   "The first-person look stays on while Raz walks and acts. Off = the "
                   "game's own: it leaves as soon as he moves.");
            toggle("Camera shake", c.camera_shake, L"camera", L"camera_shake",
                   "The game's camera shakes (landings, explosions). Off by "
                   "default: shaking the whole world in a headset is "
                   "uncomfortable. Applies at the next level load.");
        }

        void god_section(const Config& c)
        {
            // No on/off here: the right stick hold switches views (a
            // checkbox labelled like the section header also shared its ImGui
            // id, so opening the section flipped it).
            ImGui::TextWrapped("God mode: the world shown small, Raz a few centimetres tall, seen "
                               "from a seat above and behind him on a leash. Cutscenes and scripted "
                               "cameras play at normal size. Hold the right stick click to switch "
                               "views: classic, god mode, first person. In god mode the right stick "
                               "turns the view round Raz (left/right) and zooms (up/down); clicked in "
                               "and pushed, it makes Raz bigger or smaller (left/right) and raises or "
                               "lowers the seat (up/down), for the session. A recentre puts those back.");
            slider("View switch hold", c.controls.god_toggle_hold, 0.0f, 3.0f, "%.1f s", L"controls",
                   L"god_toggle_hold", "How long the right stick click is held to switch the "
                   "view: classic, god mode, first person, and round again. A quicker tap "
                   "resets the view: the camera glides behind Raz at its own height (the "
                   "zoom and Raz's size stay). 0 = no switching.");
            toggle("First person in the switch", c.controls.first_person_in_cycle, L"controls",
                   L"first_person_in_cycle", "The first-person look is one of the views the "
                   "hold switches through. Off = classic and god mode only.");
            slider("View glide", c.controls.view_transition_seconds, 0.0f, 2.0f, "%.2f s", L"controls",
                   L"view_transition_seconds", "Seconds the view glides from one view to the "
                   "next, instead of a cut. 0 = a cut.");
            slider("Raz height", c.god_raz_height_cm, 1.0f, 125.0f, "%.0f cm", L"god",
                   L"raz_height_cm", "How tall Raz looks. Smaller = a bigger world "
                   "to look down on.", 0.0f, true);
            slider("Seat distance", c.god_distance, 0.05f, 5.0f, "%.2f m", L"god",
                   L"distance", "How far back from the centre the seat is, in metres "
                   "at the shown scale.", 0.0f, true);
            slider("Seat height", c.god_height, -1.0f, 5.0f, "%.2f m", L"god",
                   L"height", "How far up the seat is, in metres at the shown scale.");
            slider("Leash", c.god_leash, 0.0f, 3.0f, "%.2f m", L"god", L"leash",
                   "How far Raz walks from the centre before the seat follows him, "
                   "in metres at the shown scale. 0 = always on him.");
            slider("Leash height", c.god_leash_height, 0.0f, 2.0f, "%.2f m", L"god",
                   L"leash_height", "The same, up and down.");
            slider("Turn speed", c.god_turn_speed, 0.0f, 360.0f, "%.0f deg/s", L"god",
                   L"turn_speed", "How fast the right stick turns the view round Raz, "
                   "at full tilt.");
            toggle("Steer the game camera", c.god_steer_camera, L"god", L"steer_camera",
                   "The game's own chase camera is moved onto the seat's line, so the "
                   "game draws what you can see from there. Off = it stays behind Raz.");
            toggle("Aim it with the head", c.god_aim_with_head, L"god", L"aim_with_head",
                   "That camera also looks where your head looks, so the game keeps drawing "
                   "what you turn to. Off = it looks at Raz (things at the edges can vanish).");
        }

        void hud_section(const Config& c)
        {
            ImGui::TextWrapped(hud_showing_menu()
                                   ? "Showing now: the MENU screen (menus, loading, pause). The Menu settings below apply."
                                   : "Showing now: the gameplay HUD. Distance, Width and HUD shape apply; the Menu settings are for menus and loading screens.");
            toggle("HUD on a screen", c.hud_enabled, L"hud", L"enabled",
                   "The game's 2D (HUD, menus) on a screen floating in front "
                   "of you. Off = drawn into each eye as the game laid it "
                   "out.");
            slider("Distance", c.hud_distance, 0.1f, 100.0f, "%.2f m", L"hud",
                   L"distance", "How far in front of you the in-game HUD "
                   "(gameplay) floats, in metres. Menus and loading screens "
                   "use the Menu settings.", 0.0f, true);
            slider("Width", c.hud_width_degrees, 2.0f, 179.0f, "%.0f deg",
                   L"hud", L"width_degrees",
                   "How wide the in-game HUD (gameplay) looks, in degrees (its "
                   "visible part). Menus and loading screens use the Menu "
                   "settings.");
            slider("HUD shape", c.hud_aspect, 0.0f, 4.0f, "%.2f", L"hud", L"aspect",
                   "The HUD screen shows only the middle of the game's screen "
                   "at this width:height shape. Psychonauts lays its 2D out at "
                   "4:3 (1.33) in the middle; 0 = the whole screen.", 1.0f);
            slider("Menu distance", c.hud_menu_distance, 0.1f, 100.0f,
                   "%.2f m", L"hud", L"menu_distance",
                   "How far away the screen that shows menus and loading "
                   "screens is, in metres.", 0.0f, true);
            slider("Menu width", c.hud_menu_width_degrees, 2.0f, 179.0f,
                   "%.0f deg", L"hud", L"menu_width_degrees",
                   "How wide the menu screen looks, in degrees (its visible "
                   "part, after the Menu shape crop).");
            slider("Menu shape", c.hud_menu_aspect, 0.0f, 4.0f, "%.2f",
                   L"hud", L"menu_aspect",
                   "The menu screen shows only the middle part of the game's "
                   "screen at this width:height shape. 0 = the whole screen, "
                   "1.33 = the game's 4:3 layout.", 1.0f);
            toggle("Menus world-locked", c.hud_world_locked, L"hud",
                   L"world_locked",
                   "Menus and loading screens stay where they were placed in "
                   "the room: in front of you whenever one appears, and at a "
                   "recentre. Off = menus and loading screens move with your "
                   "head.");
            toggle("HUD body-locked", c.hud_body_locked, L"hud", L"body_locked",
                   "The in-game HUD does not turn with your head: it stays "
                   "straight ahead (the way you faced at the last recentre) "
                   "and moves only with your head's position. Off = fixed to "
                   "your head, so the aiming reticle always marks where you "
                   "look.");
            toggle("Lock-on reticle in 3D", c.hud_lock_reticle, L"hud", L"lock_reticle",
                   "While you are locked on (left trigger), the game's lock-on "
                   "reticle sits on the target in 3D, in both eyes. Off = it "
                   "stays on the HUD screen.");
            slider("Reticle size", c.hud_lock_reticle_scale, 0.1f, 10.0f, "%.1fx",
                   L"hud", L"lock_reticle_scale",
                   "The 3D lock-on reticle's size, against how big it would "
                   "look on the HUD screen.");
            slider("Reticle height", c.hud_lock_reticle_height, -2.0f, 5.0f, "%.2f m",
                   L"hud", L"lock_reticle_height",
                   "How high above the target's feet the 3D lock-on reticle "
                   "sits, in metres.");
            static const char* const kTintNames[] = { "On the HUD screen", "Over the whole view", "Off" };
            choice("Fades to black", c.hud_screen_fades, kTintNames, 3, L"hud", L"screen_fades",
                   "The game's fades to black between areas. Over the whole "
                   "view = the world fades in both eyes; on the HUD screen = "
                   "a floating black panel, as before.");
            choice("Screen flashes", c.hud_screen_flashes, kTintNames, 3, L"hud", L"screen_flashes",
                   "The game's full-screen colour flashes, such as the ground "
                   "pound's orange.");
        }

        void overlay_section(const Config& c)
        {
            slider("Panel distance", c.overlay_vr_distance, 0.3f, 5.0f, "%.2f m",
                   L"overlay", L"vr_distance",
                   "How far in front of you this window floats in the headset, "
                   "in metres.");
            slider("Panel width", c.overlay_vr_width_degrees, 10.0f, 120.0f,
                   "%.0f deg", L"overlay", L"vr_width_degrees",
                   "How wide this window looks in the headset, in degrees.");
            slider("Panel to the left", c.overlay_vr_yaw_degrees, -90.0f, 90.0f,
                   "%.0f deg", L"overlay", L"vr_yaw_degrees",
                   "How far to the left of straight ahead this window floats, "
                   "in degrees, round your head (negative = to the right). It "
                   "keeps facing you.");
            slider("Panel up", c.overlay_vr_pitch_degrees, -60.0f, 60.0f,
                   "%.0f deg", L"overlay", L"vr_pitch_degrees",
                   "How far above straight ahead this window floats, in "
                   "degrees (negative = below).");
            slider("Hold to open", c.overlay_vr_open_hold, 0.0f, 3.0f,
                   "%.1f s", L"overlay", L"vr_open_hold",
                   "Hold the left controller's menu button this long to open "
                   "this window. 0 = off (then only the Insert key opens it).");
        }

        // `vr`: laid out for the headset panel (the window fills the
        // panel's square and stays put), else for the desktop window.
        void build_ui(bool vr)
        {
            const Config& c = config();
            ImGuiWindowFlags flags = 0;
            if (vr)
            {
                ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
                flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoCollapse;
            }
            else
            {
                ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(480.0f, 640.0f),
                                         ImGuiCond_FirstUseEver);
            }
            // Focused on opening, so the controllers start on its first item
            // (the Recentre button) without a click.
            if (g_just_opened)
                ImGui::SetNextWindowFocus();
            if (!ImGui::Begin("Psychonauts VR settings", &g_open, flags))
            {
                ImGui::End();
                return;
            }
            ImGui::Text("Game frame rate: %.0f fps",
                        (double)ImGui::GetIO().Framerate);
            ImGui::TextWrapped("Changes apply at once and save to psyvr.ini. "
                               "Insert closes this window; the game takes no "
                               "mouse, keys or pad while it is open.");
            ImGui::TextWrapped("Ctrl+click a slider to type an exact value.");
            if (vr)
                ImGui::TextWrapped("VR controllers (the game keeps your Xbox "
                                   "pad): left stick moves, A selects, B goes "
                                   "back, right stick scrolls. On a slider: A, "
                                   "then left/right (hold a grip for slower/"
                                   "faster), then A. The left menu button "
                                   "closes this.");
            if (ImGui::Button("Recentre"))
                vr_recenter();
            if (g_just_opened)
                ImGui::SetItemDefaultFocus();
            help("Recentre the view on where you are looking now (the same "
                 "as the recentre key).");
            ImGui::Spacing();
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
            if (ImGui::CollapsingHeader("VR"))
                vr_section(c);
            if (ImGui::CollapsingHeader("Controls (button guide)"))
                controls_section(c);
            if (ImGui::CollapsingHeader("Theatre"))
                theatre_section(c);
            if (ImGui::CollapsingHeader("Camera"))
                camera_section(c);
            if (ImGui::CollapsingHeader("God mode"))
                god_section(c);
            if (ImGui::CollapsingHeader("HUD"))
                hud_section(c);
            if (ImGui::CollapsingHeader("This window in VR"))
                overlay_section(c);
            if (ImGui::CollapsingHeader("Performance"))
            {
                ImGui::TextWrapped("What a frame costs. Resolution and MSAA apply after a restart.");
                slider("Resolution scale", c.resolution_scale, 0.5f, 1.5f, "%.2f", L"vr",
                       L"resolution_scale",
                       "Each eye's render size as a fraction of what the headset asks "
                       "for (Virtual Desktop's own setting picks that). 0.75 draws about "
                       "half the pixels. Restart to apply.");
                static const char* const kMsaa[] = { "off", "2x", "4x", "8x" };
                static const int kMsaaValue[] = { 0, 2, 4, 8 };
                int sel = c.msaa >= 8 ? 3 : c.msaa >= 4 ? 2 : c.msaa >= 2 ? 1 : 0;
                if (ImGui::Combo("MSAA", &sel, kMsaa, 4))
                {
                    wchar_t text[4]{};
                    swprintf_s(text, L"%d", kMsaaValue[sel]);
                    config_save_setting(L"graphics", L"msaa", text);
                    log("overlay: saved [graphics] msaa = %ls", text);
                }
                help("Anti-aliasing samples (the game asks for 4x). Each of the game's "
                     "full-screen passes is paid per sample, so this costs a lot in a "
                     "headset. Restart to apply.");
                toggle("Fast fades", c.fast_fades, L"graphics", L"fast_fades",
                       "Things fading in or out (far away props, mostly) are drawn only "
                       "where they are. The game lays each one over the whole frame "
                       "twice, which at headset resolution costs a lot. The picture is "
                       "the same; off = the game's own way, for comparison.");
                toggle("Skip full-screen effects", c.debug_skip_fullscreen, L"debug",
                       L"skip_fullscreen_passes",
                       "Leave out the game's full-screen effect passes: the glow, things "
                       "fading in and out far away, some shading on characters, and the "
                       "telekinesis hand. Faster, but those are missing.");
                toggle("Detail culling", c.detail_culling, L"vr", L"detail_culling",
                       "The game's fading out of small things far away (tree tops, "
                       "rocks, props past 80 m) once they look tiny on screen. Off = "
                       "everything is drawn at any distance; the game is light enough. "
                       "On = the game's own behaviour.");
                toggle("Texture pack compatibility", c.texture_pack_compat, L"graphics",
                       L"texture_pack_compat",
                       "Texture packs replace the game's textures with larger ones. The mod "
                       "recognises a few effects by the size of their texture (the ground "
                       "pound's orange flash); on = it also accepts the larger versions. "
                       "Turn off if something looks wrong with a texture pack.");
            }
            ImGui::PopItemWidth();
            ImGui::End();
        }
    }

    bool overlay_open()
    {
        return g_ready && g_open;
    }

    bool overlay_request_open(bool open)
    {
        if (!g_ready)
            return false;
        if (open != g_open)
            set_open(open);                 // the same path as Insert
        return true;
    }

    void overlay_before_reset()
    {
        // The panel target is in D3DPOOL_DEFAULT: made again on next use.
        release_panel();
        g_panel_failed = false;
        if (g_ready)
            ImGui_ImplDX9_InvalidateDeviceObjects();
    }

    void overlay_device_closing(IDirect3DDevice9* real_device)
    {
        if (!g_ready || real_device != g_device)
            return;
        // The window outlives the device, so hand its procedure back and
        // start again on the next device.
        if (g_hwnd && IsWindow(g_hwnd) && g_game_proc)
        {
            if (g_unicode)
                SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                                  reinterpret_cast<LONG_PTR>(g_game_proc));
            else
                SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC,
                                  reinterpret_cast<LONG_PTR>(g_game_proc));
        }
        if (g_hook)
            UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
        window_block_game_mouse(false);
        if (g_open)
            vr_pad_block_game(false);
        release_panel();
        g_panel_failed = false;
        g_frame_built = false;
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_device = nullptr;
        g_hwnd = nullptr;
        g_game_proc = nullptr;
        g_ready = false;
        g_open = false;
    }

    namespace
    {
        // One ImGui frame: the controllers in, the window laid out, drawn
        // into draw data. False if the menu button closed it instead.
        bool build_frame(bool vr)
        {
            if (!poll_pad())
                return false;
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGuiIO& io = ImGui::GetIO();
            g_client_size = io.DisplaySize;     // the Win32 backend's: client pixels
            if (vr)
                io.DisplaySize = ImVec2((float)kPanelSize, (float)kPanelSize);
            feed_gamepad(io);
            ImGui::NewFrame();
            // The focus ring on the first item from the start: by default
            // ImGui shows it only after a directional press.
            if (g_just_opened)
                ImGui::SetNavCursorVisible(true);
            build_ui(vr);
            g_just_opened = false;
            ImGui::Render();
            g_frame_built = true;
            return true;
        }

        // The draw data onto `target`, which the device state is restored
        // around (the backend restores what it changes, but not the render
        // target, which the game may have left on anything). `clear` first
        // clears it to transparent (the panel); the game's depth buffer is
        // left out (ImGui does not depth test, and it may not fit).
        void render_to(IDirect3DSurface9* target, ImDrawData* data, bool clear)
        {
            IDirect3DSurface9* old_target = nullptr;
            IDirect3DSurface9* old_depth = nullptr;
            D3DVIEWPORT9 old_viewport{};
            g_device->GetRenderTarget(0, &old_target);
            g_device->GetDepthStencilSurface(&old_depth);
            g_device->GetViewport(&old_viewport);
            if (SUCCEEDED(g_device->SetRenderTarget(0, target)))
            {
                g_device->SetDepthStencilSurface(nullptr);
                if (clear)
                    g_device->Clear(0, nullptr, D3DCLEAR_TARGET,
                                    D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
                if (SUCCEEDED(g_device->BeginScene()))
                {
                    ImGui_ImplDX9_RenderDrawData(data);
                    g_device->EndScene();
                }
            }
            if (old_target)
            {
                g_device->SetRenderTarget(0, old_target);
                g_device->SetViewport(&old_viewport);
                old_target->Release();
            }
            g_device->SetDepthStencilSurface(old_depth);
            if (old_depth)
                old_depth->Release();
        }
    }

    void overlay_vr_frame(IDirect3DSwapChain9* chain, IDirect3DSurface9* back_buffer)
    {
        // Nothing at all while closed (or before the overlay is up).
        if (!g_open || !init(chain))
            return;
        sync_open();
        if (!g_open || !back_buffer || !ensure_panel() || !build_frame(true))
            return;
        ImDrawData* data = ImGui::GetDrawData();
        if (!data || data->DisplaySize.x <= 0.0f || data->DisplaySize.y <= 0.0f)
            return;
        render_to(g_panel_surface, data, true);

        // World-locked like the game's menus (hud.cpp): put level in front
        // of the head when it opens, at a recentre, or when its distance is
        // changed (the slider then shows its effect where you look), and
        // left there so it can be read by leaning and looking around. With
        // no head pose it rides in front of the head.
        const float distance = config().overlay_vr_distance;
        const float yaw = config().overlay_vr_yaw_degrees;
        const float pitch = config().overlay_vr_pitch_degrees;
        Mat4 head_to_room = Mat4::identity();
        Mat4 panel_to_room = translation(0.0f, 0.0f, distance);
        if (vr_head_in_room_metres(&head_to_room))
        {
            if (!g_panel_placed || g_panel_recenter != vr_recenter_count() ||
                g_panel_distance != distance || g_panel_yaw != yaw || g_panel_pitch != pitch)
            {
                g_panel_to_room = hud_place_round_head(head_to_room, distance,
                                                       yaw * 0.01745329f, pitch * 0.01745329f);
                g_panel_placed = true;
                g_panel_recenter = vr_recenter_count();
                g_panel_distance = distance;
                g_panel_yaw = yaw;
                g_panel_pitch = pitch;
            }
            panel_to_room = g_panel_to_room;
        }
        else
        {
            head_to_room = Mat4::identity();
            g_panel_placed = false;
        }
        const float half_w = distance * tanf(config().overlay_vr_width_degrees * 0.5f * 0.01745329f);
        hud_draw_panel(g_device, back_buffer, g_panel, panel_to_room, head_to_room,
                       half_w, half_w, 0.0f, 1.0f, false);
        if (!g_panel_logged)
        {
            g_panel_logged = true;
            log("overlay: shown in the headset as a %ux%u panel, %.2f m away, %.0f deg wide",
                kPanelSize, kPanelSize, distance, config().overlay_vr_width_degrees);
        }
    }

    void overlay_frame(IDirect3DSwapChain9* chain)
    {
        // Initialised even while closed: the toggle key is seen by the
        // window procedure this installs.
        if (!init(chain))
            return;
        sync_open();
        if (!g_open)
        {
            g_frame_built = false;
            return;
        }

        IDirect3DSurface9* back = nullptr;
        D3DSURFACE_DESC desc{};
        if (FAILED(chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back)) ||
            !back)
            return;
        if (FAILED(back->GetDesc(&desc)) || !desc.Width || !desc.Height)
        {
            back->Release();
            return;
        }

        // Already built for the headset this frame (overlay_vr_frame): the
        // same draw data, not a second frame.
        const bool built = g_frame_built;
        g_frame_built = false;
        if (!built && !build_frame(false))
        {
            back->Release();
            return;
        }

        // ImGui laid this out in the window's client pixels (for the
        // headset, in the panel's square at the client's top left). The back
        // buffer is the two-eye image, usually far larger and of another
        // shape, and is squeezed into the window when presented -- so the
        // draw data is stretched to the back buffer here, and comes out at
        // its intended size and shape on the monitor, under the mouse.
        ImDrawData* data = ImGui::GetDrawData();
        const ImVec2 client = g_client_size.x > 0.0f && g_client_size.y > 0.0f
                            ? g_client_size : (data ? data->DisplaySize : ImVec2(0.0f, 0.0f));
        if (data && client.x > 0.0f && client.y > 0.0f)
        {
            const ImVec2 scale((float)desc.Width / client.x,
                               (float)desc.Height / client.y);
            for (int n = 0; n < data->CmdListsCount; ++n)
            {
                ImDrawList* list = data->CmdLists[n];
                for (int i = 0; i < list->VtxBuffer.Size; ++i)
                {
                    list->VtxBuffer[i].pos.x *= scale.x;
                    list->VtxBuffer[i].pos.y *= scale.y;
                }
            }
            data->ScaleClipRects(scale);
            data->DisplayPos.x *= scale.x;
            data->DisplayPos.y *= scale.y;
            data->DisplaySize = ImVec2((float)desc.Width, (float)desc.Height);
            render_to(back, data, false);
        }
        back->Release();
    }
}
