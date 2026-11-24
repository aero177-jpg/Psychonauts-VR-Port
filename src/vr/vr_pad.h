#pragma once
// The VR controllers as the game's own gamepad.
//
// The mod replaces the game's XInput imports (game_xinput_*_slot in
// game_psy.cpp) so pad 0 reports the controllers, merged with a real pad if
// one is plugged in, and the game's rumble goes to the controllers' haptics.
// Psychonauts picks keyboard or pad once at start-up, so the imports are
// replaced while the mod loads and pad 0 reads as connected from then on.
// The mapping is in psyvr.ini [controls].

struct _XINPUT_GAMEPAD;     // <xinput.h>, kept out of this header

namespace psyvr
{
    // From DllMain: replaces the game's XInput imports, before the game
    // first looks for a pad.
    void vr_pad_install();

    // From the frame boundary: re-installs if the imports were changed back,
    // and with no hook feeds the controllers to the settings overlay.
    void vr_pad_frame();

    // The game reads the VR controllers as its pad 0 (vr_pad_install
    // succeeded and [controls] enabled).
    bool vr_pad_drives_game();

    // From the frame boundary: the keyboard recentre key ([vr] recenter_key),
    // edge-triggered, only while the game window is in front.
    void vr_keyboard_frame();

    // The settings overlay (overlay.cpp) while it is open: the game gets a
    // neutral pad (no sticks, triggers or buttons). On unblocking, the pad
    // stays neutral until every button and trigger is let go, so the press
    // that closed the overlay never reaches the game. Rumble still passes
    // through.
    void vr_pad_block_game(bool block);
    // The pad as it would have gone to the game at its last poll (the
    // controllers, a real pad 0 and a pad script merged), for the overlay to
    // navigate with. False (and a neutral pad) if the game has not polled in
    // the last quarter second.
    bool vr_pad_menu_state(_XINPUT_GAMEPAD* out);

    // The right stick and its click, as in the Underworld mod. Worked out
    // on the merged pad (controllers, real pad 0, pad script) at each poll:
    //  * a tap of the click (let go before [controls] god_toggle_hold) resets
    //    the view (vr_pad_take_view_reset: the camera behind Raz, its height
    //    the game's own; game_psy.cpp). The game no longer gets R3 from it
    //    (its first-person look is in the view switch);
    //  * held that long it switches the view (vr_pad_take_god_toggle; the
    //    switch itself is game_psy.cpp's), with a buzz on the right hand;
    //  * clicked in, the stick is the mod's (camera distance and height in
    //    the classic view, Raz's size and the seat's height in god mode) and
    //    the game sees it centred; pushed past half way the press is neither
    //    a tap nor a hold.
    // A recentre (both stick clicks) spoils the tap and the hold.
    struct VrRightStick
    {
        float x = 0.0f, y = 0.0f;   // -1..1, up positive, no dead zone
        bool clicked = false;       // the click is down
        bool settling = false;      // let go under 200 ms ago: the stick may still carry the click's nudge
    };
    // The stick at the game's last poll; false (and neutral) if the game
    // has not polled in the last quarter second, or the overlay has the pad.
    bool vr_pad_right_stick(VrRightStick* out);
    // True once per view switch asked for by a hold; the frame boundary
    // takes it.
    bool vr_pad_take_god_toggle();
    // True once per view reset asked for by a tap; the frame boundary takes it.
    bool vr_pad_take_view_reset();
    // A short R3 for the game, as a tap gives (the view switch enters and
    // leaves the first-person look with it).
    void vr_pad_press_r3();
}
