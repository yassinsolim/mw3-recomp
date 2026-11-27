#pragma once

// The view turned by an angle rather than by the stick (mouse_look.cpp): the
// mouse on Windows, and a finger and the phone's gyroscope on Android.
namespace mouse_look
{
    // Whether this title's view can be turned by the mouse as an angle.
    // Where it cannot, the mouse has to drive the right stick
    // (kernel/input.cpp).
    bool There();
    // Turns the first player's view by this many degrees, to the right and
    // downwards, the next time the title works its view out -- before its
    // own Sensitivity option and the sight's zoom, which both scale it.
    // What is not taken within a quarter of a second is dropped: the title
    // is not turning a view just now. From the title's main thread.
    void Turn(float right, float down);

    // Whether the title has worked its view out within the last quarter of
    // a second, so that a turn handed to TurnExactly will be taken. Never,
    // for an executable whose view has not been found.
    bool Taking();
    // The same turn for a finger on the glass and for the gyroscope, whose
    // sensitivity is the app's own setting: the sight's zoom scales it and
    // the title's Sensitivity option does not. `since` is when the movement
    // was made, on the steady clock in nanoseconds, for MW2_TRACE_LOOK.
    // From any thread.
    void TurnExactly(float right, float down, long long since);
}
