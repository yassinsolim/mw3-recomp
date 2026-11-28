#pragma once
// How the presenter's frame sits on the panel, and what the log says of it:
// the rule alone, with no device in it (tests/surface_transform_test.cpp);
// presenter.cpp CreateSwapchain applies it.
//
// A phone's panel is wired portrait, so a landscape window reports
// currentTransform as ROTATE_90. Passing that back says "my frame is already
// turned to match" -- and this renderer's is not, it draws the console's
// landscape frame square on. The result is a picture lying on its side, which
// is what the first run on a phone showed.
//
// Identity instead, where the surface allows it: the compositor then turns the
// frame, which it is going to be doing for the system bars anyway. It costs a
// pass the pre-rotated path would not, and the alternative is rotating every
// frame in the blit -- worth doing one day, not worth an upside-down menu today.
//
// The values are Vulkan's VkSurfaceTransformFlagBitsKHR.
#include <cstdint>

namespace vk::surface_transform
{
    constexpr uint32_t kIdentity = 0x1;
    constexpr uint32_t kRotate90 = 0x2;
    constexpr uint32_t kRotate180 = 0x4;
    constexpr uint32_t kRotate270 = 0x8;

    struct Choice
    {
        // What the swapchain is made with.
        uint32_t transform;
        // Identity where the surface is turned: presents say "suboptimal",
        // which is expected and not a reason to remake the swapchain.
        bool compositorTurns;
        // Not identity: the surface would not take it, and the frame, drawn
        // square on, may show on its side.
        bool mayBeRotated;
    };

    constexpr Choice Choose(uint32_t supported, uint32_t current)
    {
        const uint32_t transform = (supported & kIdentity) ? kIdentity : current;
        return { transform, transform != current, transform != kIdentity };
    }

    enum class Note { None, CompositorTurns, MayBeRotated };

    // What the log says of a swapchain made with [choice]. That the compositor
    // turns the frame is said once in a run ([told]): the swapchain is remade
    // on every rotation and resize. A surface that will not take identity is
    // said every time. A remade swapchain the compositor turns says nothing
    // else: the warning used to follow every one after the first, while the
    // frame was turned as it should be.
    inline Note Tell(const Choice& choice, bool& told)
    {
        if (choice.compositorTurns)
        {
            if (told) return Note::None;
            told = true;
            return Note::CompositorTurns;
        }
        return choice.mayBeRotated ? Note::MayBeRotated : Note::None;
    }
}
