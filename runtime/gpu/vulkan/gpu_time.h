#pragma once
// How long the graphics chip is at work, for the report (MW2_REPORT=1): a
// timestamp before and after every submission to the queue, read back once
// the GPU has written them -- nothing ever waits for one. It says whether a
// slow run is the GPU's: the renderer's thread waiting on a fence is the GPU
// being full, or the two taking turns, and only the GPU's own clock tells
// them apart. Off, a submission is the plain vkQueueSubmit.
#include <cstdint>
#include <vulkan/vulkan.h>

namespace vk::gpu_time
{
    // On the device and queue family every submission goes to, when the report
    // is on or something else `wanted` the time (the scene's size follows it,
    // renderer_scene.cpp), and the family keeps time.
    void Start(VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t family, bool wanted);
    // With the device idle: what is left is counted first.
    void Stop();
    // Who a submission is from, so the report can say where the GPU's time
    // went: the renderer's frames, or the window's copies to the screen.
    enum class Source : uint8_t { kFrame, kPresent, kCount };
    // vkQueueSubmit of one batch, between the two timestamps. Under
    // vk::pipeline::QueueMutex like any submission: the order the pairs are
    // taken in is the order the queue runs them.
    VkResult Submit(VkQueue queue, const VkSubmitInfo& submit, VkFence fence, Source source);
    // Adds to the report the time the GPU took over the submissions it has
    // finished since the last call. One thread at a time.
    void Collect();
    // All the time collected so far, in nanoseconds; 0 when it is not kept.
    uint64_t Busy();
}
