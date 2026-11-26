#pragma once
// The switches that can be given another value under a running title, which
// is how the Android app's in-game menu changes a setting without leaving
// the level (android/jni.cpp, nativeApplyOption). Each is read from the
// environment once, as every switch is (env.h), and holds what it was last
// given here from then on; the hook that reads it asks again every time it
// runs. The frame-rate limit is frame_rate::SetLimit (frame_rate.h), and
// whether it is the player's or follows the device adaptive::PlayerSetLimit
// (adaptive.h).
namespace live
{
    // MW2_SHADOWS: 0 or 1, or -1 for the runtime's own choice, which under
    // Android is what the frames are measured to have room for
    // (gpu/d3d_hooks.cpp, adaptive.h).
    void SetSunShadows(int asked);
    // MW2_FOV in degrees (field_of_view.cpp). False in a title whose view
    // has not been located, which keeps the console's.
    bool SetFieldOfView(double degrees);
    // MW2_LOD_BIAS (model_detail.cpp). False in a title without the hook.
    bool SetDetailBias(double bias);
}
