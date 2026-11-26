#pragma once

#include "diagnostics.h"

// MW2_TRACE_AIM=1 (weapon_timing.cpp): how long the local player's weapon
// takes to come up to the sight, frame by frame, beside the times its own
// asset gives. Diagnostics only.
namespace weapon_timing
{
#if MW2_DIAGNOSTICS
    // Once a present, on the title's thread.
    void Frame();
#else
    inline void Frame() {}
#endif
}
