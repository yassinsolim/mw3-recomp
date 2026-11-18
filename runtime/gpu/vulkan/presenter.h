#pragma once
#include <cstdint>

// The window and Vulkan device. It runs on its own thread -- the guest owns the
// main thread and never gives it back -- and degrades to headless if there is
// no device.
namespace vk
{
    bool Start();
    // The window is made hidden, and shown from here: a player's start that
    // finds the game's shaders not compiled for this device hands over to the
    // launcher before anything shows (runtime/main.cpp).
    void ShowWindow();
    // Stops the window thread but keeps the device, so what the renderer built on
    // it can be torn down first.
    void StopPresenting();
    void Stop();

    // A frame the renderer has finished: a VkImage in TRANSFER_SRC layout, and
    // `serial` its number. The window shows every one, once, in order, a
    // blank each; the renderer draws into the image again only after
    // WaitUntilTaken(serial) has returned.
    void ShowImage(void* image, uint32_t width, uint32_t height, uint64_t serial);
    // Returns once the window has copied frame `serial` out (or let it go),
    // at once without a window.
    void WaitUntilTaken(uint64_t serial);
    // Paced by the display (frame_rate.h): returns once the window has taken
    // frame `serial`, the one before the frame being finished, and no sooner
    // than a display period after it last returned.
    void WaitForDisplay(uint64_t serial);
    // The guest's blank period in nanoseconds, following the display the
    // window is on; 0 when there is none to follow.
    uint64_t GuestBlankPeriod();
    // The mouse, which on Windows is the first player's right stick
    // (kernel/input.cpp). While the window has the keyboard it holds the
    // mouse: the cursor is hidden and cannot leave, and what is counted is the
    // mouse's own motion rather than the cursor's, which stops at the screen's
    // edge and carries the desktop's acceleration. Leaving the window lets it
    // go, and so does F2 until F2 again or a click in the window; MW2_MOUSE=0
    // never takes it. False when the window does not hold the mouse; otherwise
    // the counts since the last call, x to the right and y down.
    bool TakeMouseMotion(float& dx, float& dy);

    bool Running();
    uint64_t PresentedFrames();
}
