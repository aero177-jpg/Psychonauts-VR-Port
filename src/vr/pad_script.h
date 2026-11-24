#pragma once
// The text of a scripted gamepad run (pad_script.txt), parsed.
//
// One step per line: "<seconds> [token ...]", held for that long. Tokens are
// lx= ly= rx= ry= (-1..1, up positive), lt= rt= (0..1) and button names
// (a b x y lb rb l3 r3 up down left right start back). A line with only a
// time is a pause. '#' starts a comment. Pure text in, steps out (no file,
// no clock, no log), so test.bat can check it; vr_pad.cpp plays the steps.

#include <string>
#include <vector>

namespace psyvr
{
    enum PadAxisBit
    {
        PadSetLX = 1, PadSetLY = 2, PadSetRX = 4, PadSetRY = 8, PadSetLT = 16, PadSetRT = 32
    };

    struct PadStep
    {
        float seconds = 0;
        unsigned short buttons = 0;     // XINPUT_GAMEPAD_* bits, ORed in
        unsigned set = 0;               // PadSet* bits: the axes this step sets
        float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
        std::string text;               // the tokens as written, for the log
    };

    const size_t kPadScriptMaxSteps = 200;
    const float kPadScriptMaxSeconds = 600.0f;

    // Parses `text`. A bad line goes to `errors` ("line 3: ...") and is
    // skipped; steps past the limit are dropped with one error.
    void pad_script_parse(const std::string& text, std::vector<PadStep>& steps,
                          std::vector<std::string>& errors);

    // The total of the steps' seconds.
    float pad_script_seconds(const std::vector<PadStep>& steps);
}
