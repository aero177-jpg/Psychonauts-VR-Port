#include "pad_script.h"

#include <windows.h>
#include <xinput.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace psyvr
{
    namespace
    {
        bool same(const std::string& a, const char* b)
        {
            return _stricmp(a.c_str(), b) == 0;
        }

        // The whole of `s` as a finite number.
        bool number(const std::string& s, float* out)
        {
            if (s.empty())
                return false;
            char* end = nullptr;
            const double v = strtod(s.c_str(), &end);
            if (*end || !std::isfinite(v))
                return false;
            *out = (float)v;
            return true;
        }

        unsigned short button_bit(const std::string& name)
        {
            static const struct { const char* name; WORD bit; } kButtons[] = {
                { "a", XINPUT_GAMEPAD_A },          { "b", XINPUT_GAMEPAD_B },
                { "x", XINPUT_GAMEPAD_X },          { "y", XINPUT_GAMEPAD_Y },
                { "lb", XINPUT_GAMEPAD_LEFT_SHOULDER }, { "rb", XINPUT_GAMEPAD_RIGHT_SHOULDER },
                { "l3", XINPUT_GAMEPAD_LEFT_THUMB },    { "r3", XINPUT_GAMEPAD_RIGHT_THUMB },
                { "up", XINPUT_GAMEPAD_DPAD_UP },       { "down", XINPUT_GAMEPAD_DPAD_DOWN },
                { "left", XINPUT_GAMEPAD_DPAD_LEFT },   { "right", XINPUT_GAMEPAD_DPAD_RIGHT },
                { "start", XINPUT_GAMEPAD_START },      { "back", XINPUT_GAMEPAD_BACK },
            };
            for (const auto& b : kButtons)
                if (same(name, b.name))
                    return b.bit;
            return 0;
        }

        // One line (comment already cut off). Returns false and says why.
        bool parse_line(const std::string& line, PadStep* step, std::string* why)
        {
            std::vector<std::string> words;
            size_t i = 0;
            while (i < line.size())
            {
                while (i < line.size() && isspace((unsigned char)line[i])) ++i;
                size_t j = i;
                while (j < line.size() && !isspace((unsigned char)line[j])) ++j;
                if (j > i)
                    words.push_back(line.substr(i, j - i));
                i = j;
            }
            if (words.empty())
                return false;

            float secs = 0;
            if (!number(words[0], &secs) || secs <= 0.0f || secs > kPadScriptMaxSeconds)
            {
                *why = "the time must be above 0 and at most 600 seconds, got '" + words[0] + "'";
                return false;
            }
            step->seconds = secs;

            static const struct { const char* name; unsigned bit; float PadStep::* field; float lo; } kAxes[] = {
                { "lx", PadSetLX, &PadStep::lx, -1.0f }, { "ly", PadSetLY, &PadStep::ly, -1.0f },
                { "rx", PadSetRX, &PadStep::rx, -1.0f }, { "ry", PadSetRY, &PadStep::ry, -1.0f },
                { "lt", PadSetLT, &PadStep::lt, 0.0f },  { "rt", PadSetRT, &PadStep::rt, 0.0f },
            };
            for (size_t k = 1; k < words.size(); ++k)
            {
                const std::string& w = words[k];
                const size_t eq = w.find('=');
                if (eq == std::string::npos)
                {
                    const unsigned short bit = button_bit(w);
                    if (!bit)
                    {
                        *why = "unknown token '" + w + "'";
                        return false;
                    }
                    step->buttons |= bit;
                    continue;
                }
                const std::string key = w.substr(0, eq);
                bool found = false;
                for (const auto& a : kAxes)
                {
                    if (!same(key, a.name))
                        continue;
                    found = true;
                    float v = 0;
                    if (!number(w.substr(eq + 1), &v) || v < a.lo || v > 1.0f)
                    {
                        *why = "'" + w + "' needs a number " + (a.lo < 0 ? "-1 to 1" : "0 to 1");
                        return false;
                    }
                    step->*a.field = v;
                    step->set |= a.bit;
                }
                if (!found)
                {
                    *why = "unknown axis '" + key + "'";
                    return false;
                }
            }
            // The tokens after the time, as written.
            const size_t at = line.find(words[0]) + words[0].size();
            const size_t first = line.find_first_not_of(" \t\r", at);
            step->text = first == std::string::npos ? "" : line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
            return true;
        }
    }

    void pad_script_parse(const std::string& text, std::vector<PadStep>& steps,
                          std::vector<std::string>& errors)
    {
        bool full_said = false;
        int number_of_line = 0;
        size_t pos = 0;
        while (pos <= text.size())
        {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos)
                end = text.size();
            std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            ++number_of_line;
            const size_t hash = line.find('#');
            if (hash != std::string::npos)
                line.erase(hash);
            if (line.find_first_not_of(" \t\r") == std::string::npos)
                continue;

            PadStep step;
            std::string why;
            if (!parse_line(line, &step, &why))
            {
                errors.push_back("line " + std::to_string(number_of_line) + ": " + why);
                continue;
            }
            if (steps.size() >= kPadScriptMaxSteps)
            {
                if (!full_said)
                    errors.push_back("line " + std::to_string(number_of_line) + ": more than 200 steps, the rest is dropped");
                full_said = true;
                continue;
            }
            steps.push_back(step);
        }
    }

    float pad_script_seconds(const std::vector<PadStep>& steps)
    {
        float total = 0;
        for (const PadStep& s : steps)
            total += s.seconds;
        return total;
    }
}
