// The mouse turning the view as a mouse does (mouse_look.h).
//
// A console title turns its view with a stick, which is a rate: so many
// degrees a second at full tilt, through a response curve that flattens the
// middle of the stick, an acceleration of 1200 degrees a second squared and a
// slowing near a target. Driving that stick from the mouse (kernel/input.cpp
// still does, for an executable without the addresses below) cannot be made
// a mouse: measured in Modern Warfare 3's multiplayer at sensitivity 5, a
// thousand counts in a second turned the view 28 degrees where the PC game
// turns 110, two hundred counts in two seconds turned it 0.3 where the PC
// game turns 22, and three thousand in half a second were held to the
// stick's 260 degrees a second.
//
// The title works its view out once a frame in CL_GamepadMove, which gives
// the angles it has and the stick to AimAssist_UpdateGamePadInput and stores
// the angles that come back. The hook adds the mouse's turn to those, as the
// PC game's CL_MouseMove adds it to the same two angles: 0.022 degrees a
// count times the sensitivity, times the zoom factor the title keeps for its
// own stick (1 from the hip, the view's tangent over the hip view's down a
// sight), which is what the PC game scales its mouse by. The title's
// Sensitivity option scales it too, as it scales the stick. Pitch is not
// held here: the player movement code holds the view short of straight up
// and down and takes the excess out of the angles' offset, as for the stick.
//
// On Android the same hook turns the view for a finger on the look area and
// for the gyroscope (android/input.cpp), in every executable the four games
// have there. Through the stick a finger was no better than the mouse: the
// title's dead zone ate a slow swipe, its acceleration of 1200 degrees a
// second squared lagged a quick one and its turn rate capped it, so the same
// swipe turned the view by a different angle each time. Their turn is scaled
// by the sight's zoom alone: how far a swipe turns is the app's setting, not
// the Sensitivity option the title has for its stick.

#include <ppc_recomp_shared.h>
#include "title.h"
#include "mouse_look.h"
#include "log.h"

#include <bit>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <utility>

#if defined(T_AimAssist_UpdateInput) && (defined(_WIN32) || defined(MW2_ANDROID))

namespace
{
    using Clock = std::chrono::steady_clock;

    // AimInput and AimOutput, as AimAssist_UpdateGamePadInput reads and
    // writes them. Call of Duty 4's input is a word shorter before the
    // local client, and Black Ops III keeps it as a byte (title.h).
#ifndef T_AIM_INPUT_LOCAL_CLIENT
#define T_AIM_INPUT_LOCAL_CLIENT 44u
#endif
    constexpr uint32_t kInputLocalClient = T_AIM_INPUT_LOCAL_CLIENT;

    uint32_t LocalClient(uint8_t* base, uint32_t input)
    {
#ifdef T_AIM_INPUT_LOCAL_CLIENT_BYTE
        return PPC_LOAD_U8(input + T_AIM_INPUT_LOCAL_CLIENT_BYTE);
#else
        return PPC_LOAD_U32(input + kInputLocalClient);
#endif
    }
    constexpr uint32_t kOutputPitch = 0, kOutputYaw = 4;
    constexpr uint32_t kGamepadSensitivity = 4;

    struct
    {
        std::mutex lock;
        float right = 0, down = 0;      // the mouse's degrees not turned yet
        float exactRight = 0, exactDown = 0;    // a finger's and the gyroscope's
        long long since = 0;            // when the oldest of those was made
        Clock::time_point taken{};      // when the hook last looked
    } g;

    float Real(uint8_t* base, uint32_t address)
    {
        return std::bit_cast<float>(uint32_t(PPC_LOAD_U32(address)));
    }

    long long Nanoseconds(Clock::time_point when)
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(when.time_since_epoch()).count();
    }
}

// The mouse needs the title's Sensitivity option, which the executables of
// Modern Warfare 2 and 3 have been found to keep.
#if defined(_WIN32) && defined(T_Gamepad_Settings)
bool mouse_look::There() { return true; }
#else
bool mouse_look::There() { return false; }
#endif

void mouse_look::Turn(float right, float down)
{
    std::lock_guard lock(g.lock);
    if (Clock::now() - g.taken > std::chrono::milliseconds(250)) g.right = g.down = 0;
    g.right += right;
    g.down += down;
}

bool mouse_look::Taking()
{
    std::lock_guard lock(g.lock);
    return Clock::now() - g.taken <= std::chrono::milliseconds(250);
}

void mouse_look::TurnExactly(float right, float down, long long since)
{
    std::lock_guard lock(g.lock);
    if (Clock::now() - g.taken > std::chrono::milliseconds(250)) g.exactRight = g.exactDown = 0;
    if (g.exactRight == 0.0f && g.exactDown == 0.0f) g.since = since;
    g.exactRight += right;
    g.exactDown += down;
}

GUEST_HOOK(T_AimAssist_UpdateInput)
{
    const uint32_t input = ctx.r3.u32, output = ctx.r4.u32;
    GUEST_ORIG(T_AimAssist_UpdateInput)(ctx, base);
    // The mouse and the touch controls are the first player's.
    const uint32_t localClient = LocalClient(base, input);
    if (localClient != 0) return;

    float right, down, exactRight, exactDown;
    long long since;
    const auto now = Clock::now();
    {
        std::lock_guard lock(g.lock);
        g.taken = now;
        right = std::exchange(g.right, 0.0f);
        down = std::exchange(g.down, 0.0f);
        exactRight = std::exchange(g.exactRight, 0.0f);
        exactDown = std::exchange(g.exactDown, 0.0f);
        since = g.since;
    }

    // MW2_TRACE_LOOK: the view's angles whenever they change, with what was
    // added here and how long it had waited -- how far a swipe turned the
    // view and how soon, whichever way it was turned.
    static const bool trace = diag::Flag("MW2_TRACE_LOOK");
    const bool turned = right != 0.0f || down != 0.0f || exactRight != 0.0f || exactDown != 0.0f;
    if (!turned && !trace) return;

    float scale = Real(base, T_DATA_AimAssistState + T_AIMASSIST_FOV_SCALE);
    if (!(scale > 0.0f && scale <= 4.0f)) scale = 1.0f;
    float yaw = Real(base, output + kOutputYaw), pitch = Real(base, output + kOutputPitch);
    float addedRight = 0.0f, addedDown = 0.0f;
    if (turned)
    {
        float sensitivity = 1.0f;
#ifdef T_Gamepad_Settings
        if (right != 0.0f || down != 0.0f)
        {
            // The player's controller settings, as the title's own turn rates
            // find them; the function has returned, so its volatile registers
            // are free.
            ctx.r3.u64 = localClient;
            GUEST_FUNC(T_CL_ControllerOf)(ctx, base);
            GUEST_FUNC(T_Gamepad_Settings)(ctx, base);
            const float option = ctx.r3.u32 ? Real(base, ctx.r3.u32 + kGamepadSensitivity) : 1.0f;
            if (option > 0.0f && option <= 16.0f) sensitivity = option;
        }
#endif
        addedRight = (right * sensitivity + exactRight) * scale;
        addedDown = (down * sensitivity + exactDown) * scale;
        // Yaw counts up to the left, pitch downwards.
        yaw -= addedRight;
        pitch += addedDown;
        PPC_STORE_U32(output + kOutputYaw, std::bit_cast<uint32_t>(yaw));
        PPC_STORE_U32(output + kOutputPitch, std::bit_cast<uint32_t>(pitch));
    }
    if (trace)
    {
        static float lastYaw = 0.0f, lastPitch = 0.0f;
        if (yaw != lastYaw || pitch != lastPitch)
        {
            const bool exact = exactRight != 0.0f || exactDown != 0.0f;
            LOGI("look: yaw %.4f pitch %.4f added %.4f %.4f zoom %.4f waited %.2f ms at %.2f ms", yaw, pitch,
                 addedRight, addedDown, scale, exact ? double(Nanoseconds(now) - since) / 1e6 : 0.0,
                 double(Nanoseconds(now)) / 1e6);
            lastYaw = yaw;
            lastPitch = pitch;
        }
    }
}

#else

bool mouse_look::There() { return false; }
void mouse_look::Turn(float, float) {}
bool mouse_look::Taking() { return false; }
void mouse_look::TurnExactly(float, float, long long) {}

#endif
