#include "vr_pad.h"
#include "dpad_gesture.h"
#include "pad_script.h"
#include "vr_session.h"
#include "xr_runtime.h"

#include "../common/config.h"
#include "../common/log.h"
#include "../proxy/game_psy.h"
#include "../proxy/overlay.h"
#include "../proxy/window.h"

#include <windows.h>
#include <xinput.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace psyvr
{
    namespace
    {
        typedef DWORD(WINAPI* PFN_GetState)(DWORD, XINPUT_STATE*);
        typedef DWORD(WINAPI* PFN_SetState)(DWORD, XINPUT_VIBRATION*);

        // The game's import slots for XInputGetState / XInputSetState
        // (game_psy.cpp); nullptr if the exe is not the one expected.
        PFN_GetState* game_get_slot() { return reinterpret_cast<PFN_GetState*>(game_xinput_get_state_slot()); }
        PFN_SetState* game_set_slot() { return reinterpret_cast<PFN_SetState*>(game_xinput_set_state_slot()); }

        PFN_GetState g_real_get = nullptr;
        PFN_SetState g_real_set = nullptr;
        bool g_installed = false;

        XINPUT_GAMEPAD g_last{};
        DWORD g_packet = 1;
        bool g_logged_first = false;

        // Both stick clicks together recentre; they are held back from the
        // game until released, so it does not see L3 + R3 as well.
        bool g_recenter_held = false;

        // [controls] dpad_modifier: the left stick as the D-pad. A press is
        // held at least kDpadMinMs so the game, polling once a frame, sees it.
        DpadGesture g_dpad;
        ULONGLONG g_dpad_last_poll = 0;
        WORD g_dpad_bits = 0;
        ULONGLONG g_dpad_bits_until = 0;
        const ULONGLONG kDpadMinMs = 60;

        bool source_down(ControlSource s)
        {
            switch (s)
            {
            case SrcRightPrimary:   return xr_input_button(1, XrButtonPrimary);
            case SrcRightSecondary: return xr_input_button(1, XrButtonSecondary);
            case SrcLeftPrimary:    return xr_input_button(0, XrButtonPrimary);
            case SrcLeftSecondary:  return xr_input_button(0, XrButtonSecondary);
            case SrcLeftMenu:       return xr_input_button(0, XrButtonMenu);
            case SrcRightMenu:      return xr_input_button(1, XrButtonMenu);
            case SrcLeftStick:      return xr_input_button(0, XrButtonStick);
            case SrcRightStick:     return xr_input_button(1, XrButtonStick);
            case SrcLeftGrip:       return xr_input_squeeze(0) > 0.6f;
            case SrcRightGrip:      return xr_input_squeeze(1) > 0.6f;
            case SrcLeftTrigger:    return xr_input_trigger(0) > 0.6f;
            case SrcRightTrigger:   return xr_input_trigger(1) > 0.6f;
            default:                return false;
            }
        }

        SHORT axis(float v)
        {
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            return (SHORT)lroundf(v * 32767.0f);
        }

        BYTE trigger(float v)
        {
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            return (BYTE)lroundf(v * 255.0f);
        }


        // The controllers as an XINPUT_GAMEPAD (the mapping lives in
        // psyvr.ini [controls]); sticks and triggers pass straight through.
        // Touch has no D-pad and no Back: the modifier's click turns the next
        // left stick slide into a D-pad press, and while it is armed the
        // start button presses Back.
        XINPUT_GAMEPAD from_controllers()
        {
            XINPUT_GAMEPAD pad{};
            if (!xr_input_active())
                return pad;
            const ControlMap& m = config().controls;

            float lx = 0, ly = 0, rx = 0, ry = 0;
            xr_input_stick(0, &lx, &ly);
            xr_input_stick(1, &rx, &ry);

            const ULONGLONG now = GetTickCount64();
            float dt = g_dpad_last_poll ? (now - g_dpad_last_poll) / 1000.0f : 0.0f;
            if (dt > 0.25f) dt = 0.25f;
            g_dpad_last_poll = now;
            const bool both_clicks = xr_input_button(0, XrButtonStick) && xr_input_button(1, XrButtonStick);
            const bool modifier_down = m.dpad_modifier != SrcNone && source_down(m.dpad_modifier);
            const DpadOut dpad = dpad_gesture_update(g_dpad, modifier_down, both_clicks || g_recenter_held,
                                                     lx, ly, dt);
            const bool modifier = dpad.modifier;
            if (dpad.buttons)
            {
                if (dpad.buttons != g_dpad_bits)
                    log("pad: left stick D-pad 0x%X", dpad.buttons);
                g_dpad_bits = dpad.buttons;
                g_dpad_bits_until = now + kDpadMinMs;
            }
            else if (now >= g_dpad_bits_until)
                g_dpad_bits = 0;
            pad.wButtons |= g_dpad_bits;
            if (dpad.stick_to_game)
            {
                pad.sThumbLX = axis(lx);
                pad.sThumbLY = axis(ly);
            }
            pad.sThumbRX = axis(rx);
            pad.sThumbRY = axis(ry);
            pad.bLeftTrigger = trigger(xr_input_trigger(0));
            pad.bRightTrigger = trigger(xr_input_trigger(1));

            const struct { ControlSource src; WORD bit; } buttons[] = {
                { m.a, XINPUT_GAMEPAD_A },          { m.b, XINPUT_GAMEPAD_B },
                { m.x, XINPUT_GAMEPAD_X },          { m.y, XINPUT_GAMEPAD_Y },
                { m.start, XINPUT_GAMEPAD_START },  { m.back, XINPUT_GAMEPAD_BACK },
                { m.lb, XINPUT_GAMEPAD_LEFT_SHOULDER }, { m.rb, XINPUT_GAMEPAD_RIGHT_SHOULDER },
                { m.l3, XINPUT_GAMEPAD_LEFT_THUMB },    { m.r3, XINPUT_GAMEPAD_RIGHT_THUMB },
            };
            for (const auto& b : buttons)
            {
                // The modifier is a modifier, not also a button.
                if (b.src == SrcNone || (modifier && b.src == m.dpad_modifier))
                    continue;
                // Armed, the start button is Back, straight through (no
                // tap-or-hold wait: start_hold never sees it).
                if (modifier && b.bit == XINPUT_GAMEPAD_START)
                {
                    if (source_down(b.src))
                        pad.wButtons |= XINPUT_GAMEPAD_BACK;
                    continue;
                }
                if (source_down(b.src))
                    pad.wButtons |= b.bit;
            }

            // Recentre: both stick clicks.
            const bool both = xr_input_button(0, XrButtonStick) && xr_input_button(1, XrButtonStick);
            if (both && !g_recenter_held)
            {
                g_recenter_held = true;
                vr_recenter();
            }
            if (g_recenter_held)
            {
                pad.wButtons &= ~(XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB);
                if (!xr_input_button(0, XrButtonStick) && !xr_input_button(1, XrButtonStick))
                    g_recenter_held = false;
            }
            return pad;
        }

        SHORT merge_axis(SHORT a, SHORT b)
        {
            return (abs(a) >= abs(b)) ? a : b;
        }

        // The scripted pad (pad_script.txt, for automated tests). The game's
        // thread polls and plays it; the lock is only for safety should
        // XInput ever be read from another thread.
        SRWLOCK g_script_lock = SRWLOCK_INIT;
        std::vector<PadStep> g_script;
        size_t g_script_step = 0;           // the step playing
        bool g_script_playing = false;
        bool g_script_started = false;      // g_script_step has been logged
        ULONGLONG g_script_step_start = 0;
        unsigned g_script_polls = 0;

        // Reads and deletes the script file; the new script replaces any
        // playing one and starts at the next read of the pad.
        void script_load(const wchar_t* path)
        {
            std::string text;
            if (FILE* f = _wfopen(path, L"rb"))
            {
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < (1u << 20))
                    text.append(buf, n);
                fclose(f);
            }
            DeleteFileW(path);
            std::vector<PadStep> steps;
            std::vector<std::string> errors;
            pad_script_parse(text, steps, errors);
            for (const std::string& e : errors)
                log("pad script: %s", e.c_str());
            log("pad script: %u steps", (unsigned)steps.size());
            AcquireSRWLockExclusive(&g_script_lock);
            g_script = steps;
            g_script_step = 0;
            g_script_started = false;
            g_script_playing = !steps.empty();
            ReleaseSRWLockExclusive(&g_script_lock);
        }

        // The stop flag, and the script file, looked for every 30 frames.
        void script_poll()
        {
            if ((++g_script_polls % 30) != 0)
                return;
            wchar_t path[MAX_PATH]{};
            swprintf_s(path, L"%spad_script_stop.flag", exe_dir());
            if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            {
                DeleteFileW(path);
                AcquireSRWLockExclusive(&g_script_lock);
                const bool was = g_script_playing;
                g_script_playing = false;
                ReleaseSRWLockExclusive(&g_script_lock);
                if (was)
                    log("pad script: stopped");
            }
            swprintf_s(path, L"%spad_script.txt", exe_dir());
            if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
                script_load(path);
        }

        // The step playing now, advancing by the wall clock; false when no
        // script is playing. Logs each step as it starts, and the end.
        bool script_current(PadStep* out)
        {
            AcquireSRWLockExclusive(&g_script_lock);
            bool playing = g_script_playing;
            if (playing)
            {
                const ULONGLONG now = GetTickCount64();
                if (g_script_started && now - g_script_step_start >= (ULONGLONG)(g_script[g_script_step].seconds * 1000.0f))
                {
                    ++g_script_step;
                    g_script_started = false;
                }
                if (g_script_step >= g_script.size())
                {
                    g_script_playing = playing = false;
                    log("pad script: done");
                }
                else
                {
                    const PadStep& s = g_script[g_script_step];
                    if (!g_script_started)
                    {
                        g_script_started = true;
                        g_script_step_start = now;
                        log("pad script: step %u/%u, %g s: %s", (unsigned)g_script_step + 1,
                            (unsigned)g_script.size(), s.seconds, s.text.c_str());
                    }
                    *out = s;
                }
            }
            ReleaseSRWLockExclusive(&g_script_lock);
            return playing;
        }

        // Each frame: the polls, and a step on (with its log line) even if
        // the game is not reading the pad.
        void script_frame()
        {
            script_poll();
            PadStep unused;
            script_current(&unused);
        }

        // The settings overlay (vr_pad_block_game): whether the game is
        // cut off, and whether it stays so until the pad is let go.
        bool g_block = false;
        bool g_block_until_release = false;
        // What the game would have got at its last poll, for the overlay.
        XINPUT_GAMEPAD g_menu_pad{};
        ULONGLONG g_menu_pad_ms = 0;
        const ULONGLONG kMenuPadStaleMs = 250;

        // Keeps `pad` for the overlay; true if the game must not see it.
        bool overlay_takes(const XINPUT_GAMEPAD& pad)
        {
            g_menu_pad = pad;
            g_menu_pad_ms = GetTickCount64();
            if (g_block)
                return true;
            if (!g_block_until_release)
                return false;
            if (pad.wButtons || pad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ||
                pad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
                return true;
            g_block_until_release = false;
            log("pad: settings overlay closed and the pad let go; the game has it again");
            return false;
        }

        // Start, held: [overlay] vr_open_hold. With the overlay closed, Start
        // is kept from the game while it is down. Let go before the time is
        // up, the game gets a short press of it (a tap); still down at the
        // time, the overlay opens and the game never sees that press. Done on
        // the merged pad (controllers, real pad 0, script), where the game's
        // own poll takes it, so every source behaves alike. While the overlay
        // is open (or the pad is held neutral after it) Start is the
        // overlay's: a press closes it (overlay.cpp poll_pad), so a hold does.
        enum StartState { StartIdle, StartPending, StartConsumed };
        StartState g_start_state = StartIdle;
        ULONGLONG g_start_down_ms = 0;
        ULONGLONG g_start_pulse_until = 0;
        bool g_start_opened = false;    // the overlay was opened by the Start still held
        const ULONGLONG kStartPulseMs = 100;    // the game polls every frame

        void start_hold(XINPUT_GAMEPAD& pad)
        {
            const bool down = (pad.wButtons & XINPUT_GAMEPAD_START) != 0;
            const float hold = config().overlay_vr_open_hold;
            if (g_block || g_block_until_release)
            {
                g_start_pulse_until = 0;
                // The press that opened the overlay is still down: keep it
                // from the overlay too, or it reads as a new press and closes
                // it at once (seen 2026-10-18: open, closed 35 ms later).
                if (g_start_opened && down)
                {
                    pad.wButtons &= ~XINPUT_GAMEPAD_START;
                    return;
                }
                g_start_opened = false;
                g_start_state = down ? StartConsumed : StartIdle;
                return;
            }
            if (hold <= 0.0f)
            {
                g_start_state = StartIdle;
                g_start_pulse_until = 0;
                return;
            }
            const ULONGLONG now = GetTickCount64();
            pad.wButtons &= ~XINPUT_GAMEPAD_START;
            switch (g_start_state)
            {
            case StartIdle:
                if (down)
                {
                    g_start_state = StartPending;
                    g_start_down_ms = now;
                }
                break;
            case StartPending:
                if (!down)
                {
                    g_start_state = StartIdle;
                    g_start_pulse_until = now + kStartPulseMs;
                    log("pad: start tapped: passed to the game");
                }
                else if (now - g_start_down_ms >= (ULONGLONG)(hold * 1000.0f))
                {
                    g_start_state = StartConsumed;
                    log("pad: start held %.1f s: settings overlay opened",
                        (now - g_start_down_ms) / 1000.0);
                    g_start_opened = true;
                    overlay_request_open(true);
                }
                break;
            case StartConsumed:
                if (!down)
                    g_start_state = StartIdle;
                break;
            }
            if (now < g_start_pulse_until)
                pad.wButtons |= XINPUT_GAMEPAD_START;
        }

        // The right stick click (vr_pad.h VrRightStick): the Underworld mod's
        // tap / hold / click-and-push, on the merged pad.
        ULONGLONG g_rc_down_ms = 0;         // when the click went down; 0 = up
        ULONGLONG g_rc_up_ms = 0;           // when it was let go
        bool g_rc_spoiled = false;          // a recentre joined: neither tap nor hold
        bool g_rc_fired = false;            // the hold toggled god mode
        bool g_rc_pushed = false;           // clicked and pushed: adjusting the view
        ULONGLONG g_r3_pulse_until = 0;     // a tap's R3 reaches the game until then
        VrRightStick g_rs;
        volatile LONG g_god_toggle_pending = 0;
        volatile LONG g_view_reset_pending = 0;
        const ULONGLONG kR3PulseMs = 120;   // the game polls every frame
        const ULONGLONG kSettleMs = 200;
        const float kPushed = 0.5f;

        void right_click(XINPUT_GAMEPAD& pad)
        {
            const ULONGLONG now = GetTickCount64();
            // A recentre keeps R3 from the game (from_controllers), but the
            // right click is still down for this.
            const bool down = (pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0 ||
                              (g_recenter_held && xr_input_button(1, XrButtonStick));
            pad.wButtons &= ~XINPUT_GAMEPAD_RIGHT_THUMB;
            g_rs.x = pad.sThumbRX / 32767.0f;
            g_rs.y = pad.sThumbRY / 32767.0f;
            if (g_block || g_block_until_release)
            {
                // The overlay has the pad; a click held into it is no gesture.
                g_rc_down_ms = 0;
                g_r3_pulse_until = 0;
                g_rs.clicked = g_rs.settling = false;
                return;
            }
            const float hold = config().controls.god_toggle_hold;
            const ULONGLONG hold_ms = (ULONGLONG)((hold > 0.0f ? hold : 0.4f) * 1000.0f);
            if (down)
            {
                if (!g_rc_down_ms)
                {
                    g_rc_down_ms = now;
                    g_rc_spoiled = g_rc_fired = g_rc_pushed = false;
                }
                if (g_recenter_held || xr_input_button(0, XrButtonStick) ||
                    (pad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB))
                    g_rc_spoiled = true;
                if (!g_rc_pushed && (fabsf(g_rs.x) > kPushed || fabsf(g_rs.y) > kPushed))
                {
                    g_rc_pushed = true;
                    log("pad: right stick clicked and pushed: the view is adjusted");
                }
                if (!g_rc_fired && !g_rc_spoiled && !g_rc_pushed && hold > 0.0f && now - g_rc_down_ms >= hold_ms)
                {
                    g_rc_fired = true;
                    InterlockedExchange(&g_god_toggle_pending, 1);
                    log("pad: right stick held %.1f s: view switch", (now - g_rc_down_ms) / 1000.0);
                    if (vr_ready())
                        xr_input_haptic(1, 0.08f, 0.0f, 0.6f);
                }
                // Clicked in, the stick is the mod's.
                pad.sThumbRX = 0;
                pad.sThumbRY = 0;
            }
            else if (g_rc_down_ms)
            {
                if (!g_rc_spoiled && !g_rc_fired && !g_rc_pushed && !g_recenter_held && now - g_rc_down_ms < hold_ms)
                {
                    InterlockedExchange(&g_view_reset_pending, 1);
                    log("pad: right stick tapped: view reset");
                }
                g_rc_down_ms = 0;
                g_rc_up_ms = now;
            }
            g_rs.clicked = down;
            g_rs.settling = !down && g_rc_up_ms && now - g_rc_up_ms < kSettleMs;
            if (now < g_r3_pulse_until)
                pad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
        }

        // A script step on top (pad_script.h): buttons added, the axes it sets replaced.
        void apply_script(XINPUT_GAMEPAD& pad)
        {
            PadStep step;
            if (!script_current(&step))
                return;
            pad.wButtons |= step.buttons;
            if (step.set & PadSetLX) pad.sThumbLX = axis(step.lx);
            if (step.set & PadSetLY) pad.sThumbLY = axis(step.ly);
            if (step.set & PadSetRX) pad.sThumbRX = axis(step.rx);
            if (step.set & PadSetRY) pad.sThumbRY = axis(step.ry);
            if (step.set & PadSetLT) pad.bLeftTrigger = trigger(step.lt);
            if (step.set & PadSetRT) pad.bRightTrigger = trigger(step.rt);
        }

        DWORD WINAPI pad_get_state(DWORD index, XINPUT_STATE* state)
        {
            if (!state)
                return ERROR_BAD_ARGUMENTS;
            XINPUT_STATE real{};
            const DWORD real_result = g_real_get ? g_real_get(index, &real) : ERROR_DEVICE_NOT_CONNECTED;
            // Pad 0 reads as connected from the game's first poll, before VR
            // is up: the game picks keyboard or pad once, at start-up.
            if (index != 0 || !config().controls.enabled || !config().vr_enabled)
            {
                if (real_result == ERROR_SUCCESS)
                {
                    *state = real;
                    if (index == 0 && overlay_takes(real.Gamepad))
                        state->Gamepad = XINPUT_GAMEPAD{};
                }
                return real_result;
            }

            // Pad 0: the controllers (neutral until VR is up), with a real
            // pad 0 merged in (larger stick deflection wins).
            XINPUT_GAMEPAD pad = from_controllers();
            if (real_result == ERROR_SUCCESS)
            {
                const XINPUT_GAMEPAD& r = real.Gamepad;
                pad.wButtons |= r.wButtons;
                pad.bLeftTrigger = max(pad.bLeftTrigger, r.bLeftTrigger);
                pad.bRightTrigger = max(pad.bRightTrigger, r.bRightTrigger);
                pad.sThumbLX = merge_axis(pad.sThumbLX, r.sThumbLX);
                pad.sThumbLY = merge_axis(pad.sThumbLY, r.sThumbLY);
                pad.sThumbRX = merge_axis(pad.sThumbRX, r.sThumbRX);
                pad.sThumbRY = merge_axis(pad.sThumbRY, r.sThumbRY);
            }
            apply_script(pad);
            start_hold(pad);
            right_click(pad);
            // The settings overlay navigates with it instead.
            if (overlay_takes(pad))
                pad = XINPUT_GAMEPAD{};
            if (memcmp(&pad, &g_last, sizeof(pad)) != 0)
            {
                g_last = pad;
                ++g_packet;
            }
            state->dwPacketNumber = g_packet;
            state->Gamepad = pad;
            if (!g_logged_first && pad.wButtons)
            {
                g_logged_first = true;
                log("pad: first controller input reached the game (buttons 0x%04X)", pad.wButtons);
            }
            return ERROR_SUCCESS;
        }

        DWORD WINAPI pad_set_state(DWORD index, XINPUT_VIBRATION* vibration)
        {
            const DWORD result = g_real_set ? g_real_set(index, vibration) : ERROR_DEVICE_NOT_CONNECTED;
            if (index != 0 || !vibration || !config().controls.enabled || !config().vr_enabled)
                return result;
            if (!vr_ready())
                return ERROR_SUCCESS;
            // The pad's left motor is the heavy one; each goes to its hand.
            // The game sets a level and leaves it, so a short pulse per call,
            // renewed while the level stays up, is enough.
            const float scale = config().controls.rumble;
            const float left = vibration->wLeftMotorSpeed / 65535.0f * scale;
            const float right = vibration->wRightMotorSpeed / 65535.0f * scale;
            if (left > 0.02f)
                xr_input_haptic(0, 0.1f, 0.0f, left > 1.0f ? 1.0f : left);
            if (right > 0.02f)
                xr_input_haptic(1, 0.1f, 0.0f, right > 1.0f ? 1.0f : right);
            return ERROR_SUCCESS;
        }
    }

    void vr_pad_install()
    {
        if (!config().controls.enabled || !config().vr_enabled)
            return;
        PFN_GetState* const get_slot = game_get_slot();
        PFN_SetState* const set_slot = game_set_slot();
        if (!get_slot || !set_slot)
            return;
        __try
        {
            PFN_GetState get = *get_slot;
            PFN_SetState set = *set_slot;
            if (!get || !set)
                return;                     // XInput not loaded yet
            if (get == &pad_get_state && set == &pad_set_state)
                return;                     // installed
            if (get != &pad_get_state)
                g_real_get = get;
            if (set != &pad_set_state)
                g_real_set = set;
            *get_slot = &pad_get_state;
            *set_slot = &pad_set_state;
            g_installed = true;
            log("pad: the game's XInput now reads the VR controllers as pad 0 (real pads still work)");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    bool vr_pad_drives_game()
    {
        return g_installed && config().controls.enabled;
    }

    void vr_pad_frame()
    {
        if (!config().controls.enabled || !vr_ready())
            return;
        script_frame();
        vr_pad_install();       // normally done at load; again if the game reloaded XInput
        if (!g_installed)
        {
            // No hook into the game's XInput (not the expected exe). The VR
            // controllers drive only the settings overlay: holding the left
            // menu button opens it ([overlay] vr_open_hold), then they
            // navigate it. The game never sees them.
            XINPUT_GAMEPAD pad = from_controllers();
            apply_script(pad);      // pad_script.txt, for testing without controllers
            start_hold(pad);
            overlay_takes(pad);
        }
    }

    void vr_pad_block_game(bool block)
    {
        if (block == g_block)
            return;
        g_block = block;
        if (block)
        {
            g_block_until_release = false;
            log("pad: settings overlay open; the game gets a neutral pad");
        }
        else
            g_block_until_release = true;   // logged in overlay_takes
    }

    bool vr_pad_menu_state(XINPUT_GAMEPAD* out)
    {
        if (!out)
            return false;
        const bool fresh = g_menu_pad_ms && GetTickCount64() - g_menu_pad_ms <= kMenuPadStaleMs;
        *out = fresh ? g_menu_pad : XINPUT_GAMEPAD{};
        return fresh;
    }

    bool vr_pad_right_stick(VrRightStick* out)
    {
        if (!out)
            return false;
        const bool fresh = !g_block && g_menu_pad_ms && GetTickCount64() - g_menu_pad_ms <= kMenuPadStaleMs;
        *out = fresh ? g_rs : VrRightStick{};
        return fresh;
    }

    bool vr_pad_take_god_toggle()
    {
        return InterlockedExchange(&g_god_toggle_pending, 0) != 0;
    }

    bool vr_pad_take_view_reset()
    {
        return InterlockedExchange(&g_view_reset_pending, 0) != 0;
    }

    void vr_pad_press_r3()
    {
        g_r3_pulse_until = GetTickCount64() + kR3PulseMs;
    }

    void vr_keyboard_frame()
    {
        static bool was_down = false;
        if (!vr_ready())
        {
            was_down = false;
            return;
        }
        const HWND window = window_handle();
        const bool front = window && GetForegroundWindow() == window;

        const int vk = config().recenter_vk;
        const bool down = vk && front && (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !was_down)
            vr_recenter();
        was_down = down;
    }
}
