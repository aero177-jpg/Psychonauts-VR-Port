#pragma once
// Back buffer sizing in VR, shared by CreateDevice and Reset.
//
// The device is created at the headset's eye size, twice as wide for side by
// side stereo. Underworld sizes its own targets from the real back buffer,
// so nothing else needs to grow (stereo.h).

#include "../vr/stereo.h"
#include "../vr/vr_session.h"
#include "../common/config.h"
#include "../common/log.h"

#include <d3d9.h>

namespace psyvr
{
    inline void size_for_vr(D3DPRESENT_PARAMETERS* pp)
    {
        unsigned w = 0, h = 0;
        vr_recommended_eye_size(&w, &h);
        pp->BackBufferWidth = w * 2;
        pp->BackBufferHeight = h;
        stereo_set_layout(pp->BackBufferWidth, pp->BackBufferHeight);
        log("  back buffer %ux%u: %ux%u an eye, side by side", pp->BackBufferWidth, pp->BackBufferHeight,
            w, h);
    }
}
