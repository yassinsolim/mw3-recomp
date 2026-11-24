#pragma once
// The frame's way to the window with sharpening (MW2_SHARPEN,
// docs/rendering.md): instead of the linear copy, the title's 1280x720 frame
// is drawn over its place in the swapchain image through FidelityFX Super
// Resolution 1.0's RCAS, five taps a window pixel apart, in one pass. It
// gives back some of what two linear enlargements took -- the scene's to the
// frame, the frame's to the window -- and costs a third to half a
// millisecond of GPU a frame on an Adreno 650 or 740.
//
// The two shaders are GLSL beside this file (shaders/sharpen.vert and
// .frag), compiled by tools/gen_sharpen_shaders.py into
// shaders/sharpen_shaders.inc.
#ifdef MW2_HAVE_VULKAN
#include <cstdint>
#include <vulkan/vulkan.h>

namespace vk::sharpen
{
    // What the renderer's finished frames are (renderer.cpp makes them,
    // sampled): the presenter hands over the image alone.
    constexpr VkFormat kFrameFormat = VK_FORMAT_R8G8B8A8_UNORM;

    // The sharpening in percent of the filter's most, 0 for none: in place
    // of MW2_SHARPEN's, from any thread, and the next frame shown has it.
    void Set(uint32_t percent);

    struct Draw
    {
        VkDevice device;
        VkCommandBuffer commands;
        uint32_t slot;                // the presenter's frame in flight
        VkImage target;               // TRANSFER_DST, and left so
        VkFormat targetFormat;
        VkExtent2D extent;
        VkImage frame;                // TRANSFER_SRC, and left so
        VkOffset2D from, to;          // the frame's place on the target
    };
    // The whole target: the frame sharpened into its place and black around
    // it. False with no sharpening asked for, or where it cannot be made on
    // this device: nothing was recorded, and the caller copies the frame.
    bool Record(const Draw& draw);

    // The views of the renderer's frames, before it destroys the images; of
    // the swapchain's, before it is replaced.
    void ForgetFrames();
    void ForgetTargets();
    void Destroy();
}
#endif
