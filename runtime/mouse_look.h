#pragma once

// The mouse turning the view as a mouse does (mouse_look.cpp): the disc's
// executables on Windows.
namespace mouse_look
{
    // Whether this title's view can be turned by an angle. Where it cannot,
    // the mouse has to drive the right stick (kernel/input.cpp).
    bool There();
    // Turns the first player's view by this many degrees, to the right and
    // downwards, the next time the title works its view out -- before its
    // own Sensitivity option and the sight's zoom, which both scale it.
    // What is not taken within a quarter of a second is dropped: the title
    // is not turning a view just now. From the title's main thread.
    void Turn(float right, float down);
}
