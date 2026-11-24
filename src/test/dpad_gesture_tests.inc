// Included by mathtest.cpp: src/vr/dpad_gesture.*
#include "../vr/dpad_gesture.h"

static void test_dpad_gesture()
{
    using namespace psyvr;
    const float dt = 0.011f;
    // Tap, then slide up: Up while out, released and disarmed at the centre.
    {
        DpadGesture g;
        DpadOut o = dpad_gesture_update(g, true, false, 0, 0, dt);
        CHECK(o.modifier && !o.stick_to_game && o.buttons == 0);
        o = dpad_gesture_update(g, false, false, 0, 0, dt);          // let go: still armed
        CHECK(o.modifier && !o.stick_to_game);
        o = dpad_gesture_update(g, false, false, 0.1f, 0.4f, dt);    // not far enough
        CHECK(o.buttons == 0);
        o = dpad_gesture_update(g, false, false, 0.2f, 0.9f, dt);
        CHECK(o.buttons == kDpadUp && !o.stick_to_game);
        o = dpad_gesture_update(g, false, false, 0.6f, 0.4f, dt);    // drifts right: still Up
        CHECK(o.buttons == kDpadUp);
        o = dpad_gesture_update(g, false, false, 0.1f, 0.1f, dt);    // centre
        CHECK(o.buttons == 0 && !o.modifier && o.stick_to_game);
        o = dpad_gesture_update(g, false, false, 0.0f, 1.0f, dt);    // now it walks
        CHECK(o.buttons == 0 && o.stick_to_game);
    }
    // Held: one slide after another; the stronger axis wins.
    {
        DpadGesture g;
        dpad_gesture_update(g, true, false, 0, 0, dt);
        DpadOut o = dpad_gesture_update(g, true, false, -0.8f, 0.6f, dt);
        CHECK(o.buttons == kDpadLeft);
        o = dpad_gesture_update(g, true, false, 0, 0, dt);
        CHECK(o.buttons == 0 && o.modifier);                         // held: stays armed
        o = dpad_gesture_update(g, true, false, 0.1f, -0.7f, dt);
        CHECK(o.buttons == kDpadDown);
        o = dpad_gesture_update(g, false, false, 0.1f, -0.7f, dt);   // let go while out
        CHECK(o.buttons == kDpadDown && !o.stick_to_game);
        o = dpad_gesture_update(g, false, false, 0, 0, dt);
        CHECK(!o.modifier && o.stick_to_game);
    }
    // Clicked while running: no press until the stick has been to the centre.
    {
        DpadGesture g;
        DpadOut o = dpad_gesture_update(g, true, false, 0.0f, 1.0f, dt);
        CHECK(o.buttons == 0 && !o.stick_to_game);
        o = dpad_gesture_update(g, true, false, 0.0f, 1.0f, dt);
        CHECK(o.buttons == 0);
        dpad_gesture_update(g, true, false, 0, 0, dt);
        o = dpad_gesture_update(g, true, false, 1.0f, 0.0f, dt);
        CHECK(o.buttons == kDpadRight);
    }
    // A tap with no slide times out; held, it never does.
    {
        DpadGesture g;
        dpad_gesture_update(g, true, false, 0, 0, dt);
        DpadOut o;
        for (int i = 0; i < 13; ++i)                                  // 1.3 s
            o = dpad_gesture_update(g, false, false, 0, 0, 0.1f);
        CHECK(o.modifier);
        o = dpad_gesture_update(g, false, false, 0, 0, 0.2f);
        CHECK(!o.modifier && o.stick_to_game);
        DpadGesture h;
        for (int i = 0; i < 40; ++i)
            o = dpad_gesture_update(h, true, false, 0, 0, 0.1f);
        CHECK(o.modifier);
    }
    // Timed out with the stick part way out: movement waits for the centre.
    {
        DpadGesture g;
        dpad_gesture_update(g, true, false, 0, 0, dt);
        DpadOut o = dpad_gesture_update(g, false, false, 0.4f, 0, 2.0f);
        CHECK(!o.modifier && !o.stick_to_game);
        o = dpad_gesture_update(g, false, false, 0.45f, 0, dt);
        CHECK(!o.stick_to_game);
        o = dpad_gesture_update(g, false, false, 0.0f, 0, dt);
        CHECK(o.stick_to_game);
    }
    // A recentre (both clicks) disarms without a press.
    {
        DpadGesture g;
        dpad_gesture_update(g, true, false, 0, 0, dt);
        DpadOut o = dpad_gesture_update(g, true, true, 0, 0, dt);
        CHECK(!o.modifier && o.buttons == 0 && o.stick_to_game);
        o = dpad_gesture_update(g, true, false, 0, 1.0f, dt);        // still held: no re-arm
        CHECK(!o.modifier && o.stick_to_game);
    }
}
