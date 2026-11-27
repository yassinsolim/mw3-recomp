// The pad the title reads, fed by the touch controls and by a physical pad.
//
// The overlay sends an absolute state on every touch event -- a button mask,
// two triggers and two sticks -- so a dropped event cannot leave a button
// held down. The look area is different: it sends movement, and so does the
// gyroscope.
//
// Look: the movement is an angle. So many degrees a pixel is the player's
// setting (the app works it out from the screen's density, so a centimetre
// of glass turns the view the same on every phone), and the degrees go to
// the title's view as they are (mouse_look.h): the same swipe turns the view
// by the same angle at any frame rate, however fast it was made, and a slow
// one is not lost. Until 27 November 2026 the finger's speed was a
// deflection of the right stick, smoothed over several frames: the title's
// dead zone, response curve, acceleration and turn rate all stood between
// the finger and the view, and a swipe of one length turned it anything
// from nothing to its whole angle by how fast it was made.
//
// Where the title is not working a view out (a menu, an executable whose
// view has not been found) the degrees still become a stick, as the mouse's
// do on Windows (kernel/input.cpp): the rate that turns that far in the
// frame, beyond the title's dead zone, with no smoothing.
#ifdef MW2_ANDROID

#include "android.h"
#include "../log.h"
#include "../mouse_look.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

namespace
{
    using Clock = std::chrono::steady_clock;

    struct Source
    {
        std::mutex lock;
        android::input::Pad pad;
        bool connected = false;
    };

    Source g_touch;                     // the on-screen controls
    Source g_hardware[4];               // physical pads, as the app numbers them
    std::atomic<bool> g_hardwareEnabled{ true };

    // ---- the look area and the gyroscope ----------------------------------
    std::mutex g_lookLock;
    float g_degreesX = 0.03f, g_degreesY = 0.03f;   // degrees a pixel
    float g_sightsFactor = 1.0f;                    // of that, while aiming
    int g_gyroMode = 0;                             // 0 off, 1 always, 2 while aiming
    // The left trigger at the last poll: the sights are up, or coming up.
    std::atomic<bool> g_aiming{ false };

    // What the stick has still to turn, for a title not taking angles: the
    // title's own numbers, which are Modern Warfare 3's and its neighbours'
    // (kernel/input.cpp has where they are from).
    constexpr float kDeadZone = 0.2f, kDeadZoneTop = 0.01f;
    constexpr float kTurnAcross[2] = { 260.0f, 90.0f };     // from the hip, down the sights
    constexpr float kTurnUpDown[2] = { 90.0f, 55.0f };
    constexpr float kCarrySeconds = 0.05f;
    struct
    {
        float right = 0, down = 0;      // degrees not turned yet
        Clock::time_point read{};
        bool reading = false;           // `read` is a time
        bool turning = false;           // the last read moved the stick,
        int16_t x = 0, y = 0;           // to here
    } g_stick;

    // A turn in degrees, to the title's view where it is taking one and to
    // the stick where it is not.
    void Turn(float right, float down, long long when)
    {
        static const bool trace = diag::Flag("MW2_TRACE_LOOK");
        if (trace) LOGI("look: turn %.4f %.4f made at %.2f ms", right, down, double(when) / 1e6);
        if (mouse_look::Taking())
        {
            mouse_look::TurnExactly(right, down, when);
            return;
        }
        std::lock_guard lock(g_lookLock);
        g_stick.right += right;
        g_stick.down += down;
    }

    // The stick that turns the view by what is waiting, over the frame this
    // read is for. Called from the title's poll, which is where the title's
    // own clock is.
    void UpdateLook(android::input::Pad& out)
    {
        std::lock_guard lock(g_lookLock);
        const auto now = Clock::now();
        const float since = g_stick.reading ? std::chrono::duration<float>(now - g_stick.read).count() : 0.0f;
        auto merge = [&out]
        {
            // Added to the right stick rather than replacing it, so a player
            // using both is not fighting them.
            out.rightX = int16_t(std::clamp<int32_t>(int32_t(out.rightX) + g_stick.x, -32768, 32767));
            out.rightY = int16_t(std::clamp<int32_t>(int32_t(out.rightY) + g_stick.y, -32768, 32767));
        };
        // The title asks for every user's state one after another, and may
        // ask twice a frame: the later reads answer as the first did, and
        // what came between them waits for the next frame.
        if (g_stick.reading && since < 0.002f)
        {
            if (g_stick.turning) merge();
            return;
        }
        // The first read, or the first after a pause: the frame is not as
        // long as the pause was.
        const float seconds = g_stick.reading && since < 0.25f ? since : 1.0f / 60.0f;
        g_stick.reading = true;
        g_stick.read = now;
        g_stick.turning = false;
        if (g_stick.right == 0.0f && g_stick.down == 0.0f) return;

        // The stick these degrees ask for, as a part of what a full one
        // turns in the frame: past 1 when the finger is quicker than that,
        // and the rest waits, a twentieth of a second of full stick at most.
        const int sights = g_aiming.load(std::memory_order_relaxed) ? 1 : 0;
        const float across = kTurnAcross[sights] * seconds, upDown = kTurnUpDown[sights] * seconds;
        float x = g_stick.right / across, y = g_stick.down / upDown;
        const float asked = std::hypot(x, y);
        const float given = std::min(asked, 1.0f);
        x /= asked;
        y /= asked;
        const float carried = std::min(asked - given, kCarrySeconds / seconds);
        g_stick.right = x * carried * across;
        g_stick.down = y * carried * upDown;

        // Onto the stick beyond the dead zone. The guest's Y axis points up.
        const float stick = (kDeadZone + (1.0f - kDeadZone - kDeadZoneTop) * given) * 32767.0f;
        g_stick.x = int16_t(std::lround(x * stick));
        g_stick.y = int16_t(std::lround(-y * stick));
        g_stick.turning = true;
        merge();
    }

    void Merge(android::input::Pad& into, const android::input::Pad& from)
    {
        into.buttons |= from.buttons;
        into.leftTrigger = std::max(into.leftTrigger, from.leftTrigger);
        into.rightTrigger = std::max(into.rightTrigger, from.rightTrigger);
        auto stronger = [](int16_t a, int16_t b) { return std::abs(int(a)) >= std::abs(int(b)) ? a : b; };
        into.leftX = stronger(into.leftX, from.leftX);
        into.leftY = stronger(into.leftY, from.leftY);
        into.rightX = stronger(into.rightX, from.rightX);
        into.rightY = stronger(into.rightY, from.rightY);
    }

    // Set by jni.cpp so a motor can reach the app.
    void (*g_rumble)(uint32_t user, uint16_t low, uint16_t high) = nullptr;
}

namespace android::input::detail
{
    void SetRumbleSink(void (*sink)(uint32_t, uint16_t, uint16_t)) { g_rumble = sink; }
}

void android::input::SetTouchPad(const Pad& pad)
{
    std::lock_guard lock(g_touch.lock);
    g_touch.pad = pad;
    g_touch.connected = true;
}

void android::input::AddLook(float dx, float dy, long long when)
{
    float right, down;
    {
        std::lock_guard lock(g_lookLock);
        const float factor = g_aiming.load(std::memory_order_relaxed) ? g_sightsFactor : 1.0f;
        right = dx * g_degreesX * factor;
        down = dy * g_degreesY * factor;
    }
    Turn(right, down, when);
}

void android::input::AddGyro(float right, float down, long long when)
{
    {
        std::lock_guard lock(g_lookLock);
        if (g_gyroMode == 0 || (g_gyroMode == 2 && !g_aiming.load(std::memory_order_relaxed))) return;
    }
    Turn(right, down, when);
}

void android::input::ClearLook()
{
    // The view stops with the finger: what the stick had still to turn is
    // not turned after it has gone.
    std::lock_guard lock(g_lookLock);
    g_stick.right = g_stick.down = 0;
}

void android::input::SetLookSettings(float degreesX, float degreesY, float sightsFactor, int gyroMode)
{
    std::lock_guard lock(g_lookLock);
    g_degreesX = std::clamp(degreesX, 0.0f, 10.0f);
    g_degreesY = std::clamp(degreesY, 0.0f, 10.0f);
    g_sightsFactor = std::clamp(sightsFactor, 0.05f, 4.0f);
    g_gyroMode = std::clamp(gyroMode, 0, 2);
}

void android::input::SetHardwarePad(uint32_t user, const Pad& pad)
{
    if (user >= 4) return;
    std::lock_guard lock(g_hardware[user].lock);
    g_hardware[user].pad = pad;
    g_hardware[user].connected = true;
}

void android::input::SetHardwarePadConnected(uint32_t user, bool connected)
{
    if (user >= 4) return;
    std::lock_guard lock(g_hardware[user].lock);
    g_hardware[user].connected = connected;
    if (!connected) g_hardware[user].pad = Pad{};
    LOGI("input: physical pad %u %s", user, connected ? "connected" : "gone");
}

void android::input::SetHardwarePadEnabled(bool enabled)
{
    g_hardwareEnabled.store(enabled, std::memory_order_relaxed);
    LOGI("input: physical pads %s", enabled ? "on" : "off");
}

bool android::input::HardwarePadEnabled() { return g_hardwareEnabled.load(std::memory_order_relaxed); }

bool android::input::Connected(uint32_t user)
{
    if (user == 0) return true;   // the touch controls are always there
    if (user >= 4 || !HardwarePadEnabled()) return false;
    std::lock_guard lock(g_hardware[user].lock);
    return g_hardware[user].connected;
}

bool android::input::Poll(uint32_t user, Pad& out)
{
    out = Pad{};
    if (user >= 4) return false;

    bool any = false;
    if (user == 0)
    {
        std::lock_guard lock(g_touch.lock);
        if (g_touch.connected) { out = g_touch.pad; any = true; }
    }
    if (HardwarePadEnabled())
    {
        std::lock_guard lock(g_hardware[user].lock);
        if (g_hardware[user].connected)
        {
            if (any) Merge(out, g_hardware[user].pad);
            else     out = g_hardware[user].pad;
            any = true;
        }
    }
    // User 0 always answers: the touch controls stand in for a pad, and a
    // title told "no controller" on user 0 stops at its "reconnect the
    // controller" screen.
    if (user == 0)
    {
        // Past gpad_button_deadzone (0.13) the left trigger is pulled, and
        // the sights are up or coming up.
        g_aiming.store(out.leftTrigger > 33, std::memory_order_relaxed);
        UpdateLook(out);
        any = true;
    }
    return any;
}

void android::input::Rumble(uint32_t user, uint16_t low, uint16_t high)
{
    if (g_rumble) g_rumble(user, low, high);
}

#endif  // MW2_ANDROID
