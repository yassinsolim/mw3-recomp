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
    // at once without a window. `meanwhile` is called every quarter of a
    // millisecond of the wait until it returns false.
    void WaitUntilTaken(uint64_t serial, bool (*meanwhile)() = nullptr);
    // Paced by the display (frame_rate.h): returns once the window has taken
    // frame `serial`, the one before the frame being finished, and no sooner
    // than a display period after it last returned.
    void WaitForDisplay(uint64_t serial, bool (*meanwhile)() = nullptr);
    // The guest's blank period in nanoseconds, following the display the
    // window is on; 0 when there is none to follow.
    uint64_t GuestBlankPeriod();
    // The mouse, which on Windows is the first player's (kernel/input.cpp).
    // While the window has the keyboard and the player is in the world it
    // holds the mouse: the cursor is hidden and cannot leave, and what is
    // counted is the mouse's own motion rather than the cursor's, which stops
    // at the screen's edge and carries the desktop's acceleration. With a
    // menu up that the mouse can point in, the cursor is the desktop's again
    // and what is read is where it is on the frame. Leaving the window lets
    // the mouse go, and so does F2 until F2 again or a click in the window;
    // MW2_MOUSE=0 never takes it.
    struct Mouse
    {
        bool held = false;          // the window holds the mouse, and
        float dx = 0, dy = 0;       // these are its counts since the last call, x to the right and y down
        bool pointing = false;      // the cursor is a menu's pointer, and
        float x = 0, y = 0;         // is here on the frame: 0 its left or top edge, 1 its right or bottom
        int wheel = 0;              // whole notches since the last call, away from the hand positive
    };
    // `menu` says whether the title has a menu up for the cursor to point in,
    // which lets the mouse go or takes it by the window's next time round.
    // Neither held nor pointing when the window has not got the keyboard or
    // F2 has given the cursor back.
    void TakeMouse(bool menu, Mouse& out);

    bool Running();
    uint64_t PresentedFrames();
    // What the window thread is doing (a few words) and for how many
    // milliseconds it has been, and the serial of the newest frame the
    // renderer has handed it (0: none yet): for the log's line about a window
    // that has had no frame for a while (gpu::WatchFrames).
    const char* PresenterStage(uint64_t& milliseconds, uint64_t& newestQueued);
}
