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

#include <ppc_recomp_shared.h>
#include "title.h"
#include "mouse_look.h"

#include <bit>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <utility>

#if defined(_WIN32) && defined(T_AimAssist_UpdateInput)

namespace
{
    using Clock = std::chrono::steady_clock;

    // AimInput and AimOutput, as AimAssist_UpdateGamePadInput reads and
    // writes them.
    constexpr uint32_t kInputLocalClient = 44;
    constexpr uint32_t kOutputPitch = 0, kOutputYaw = 4;
    constexpr uint32_t kGamepadSensitivity = 4;

    struct
    {
        std::mutex lock;
        float right = 0, down = 0;      // degrees not turned yet
        Clock::time_point taken{};      // when the hook last looked
    } g;

    float Real(uint8_t* base, uint32_t address)
    {
        return std::bit_cast<float>(uint32_t(PPC_LOAD_U32(address)));
    }
}

bool mouse_look::There() { return true; }

void mouse_look::Turn(float right, float down)
{
    std::lock_guard lock(g.lock);
    if (Clock::now() - g.taken > std::chrono::milliseconds(250)) g.right = g.down = 0;
    g.right += right;
    g.down += down;
}

GUEST_HOOK(T_AimAssist_UpdateInput)
{
    const uint32_t input = ctx.r3.u32, output = ctx.r4.u32;
    GUEST_ORIG(T_AimAssist_UpdateInput)(ctx, base);
    // The mouse is the first player's.
    const uint32_t localClient = PPC_LOAD_U32(input + kInputLocalClient);
    if (localClient != 0) return;

    float right, down;
    {
        std::lock_guard lock(g.lock);
        g.taken = Clock::now();
        right = std::exchange(g.right, 0.0f);
        down = std::exchange(g.down, 0.0f);
    }
    if (right == 0.0f && down == 0.0f) return;

    float scale = Real(base, T_DATA_AimAssistState + T_AIMASSIST_FOV_SCALE);
    if (!(scale > 0.0f && scale <= 4.0f)) scale = 1.0f;
    // The player's controller settings, as the title's own turn rates find
    // them; the function has returned, so its volatile registers are free.
    ctx.r3.u64 = localClient;
    GUEST_FUNC(T_CL_ControllerOf)(ctx, base);
    GUEST_FUNC(T_Gamepad_Settings)(ctx, base);
    const float sensitivity = ctx.r3.u32 ? Real(base, ctx.r3.u32 + kGamepadSensitivity) : 1.0f;
    if (sensitivity > 0.0f && sensitivity <= 16.0f) scale *= sensitivity;

    // Yaw counts up to the left, pitch downwards.
    PPC_STORE_U32(output + kOutputYaw, std::bit_cast<uint32_t>(Real(base, output + kOutputYaw) - right * scale));
    PPC_STORE_U32(output + kOutputPitch, std::bit_cast<uint32_t>(Real(base, output + kOutputPitch) + down * scale));
}

#else

bool mouse_look::There() { return false; }
void mouse_look::Turn(float, float) {}

#endif
