#pragma once

#include "render/frames_in_flight.h"
#include "render/gpu/sky_visibility_bake.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/vulkan_device.h"
#include "render/vulkan/vulkan_memory.h"
#include "scene/frame_description.h"

#include <array>
#include <cstdint>
#include <vector>

class ImmediateCommands;

/*
* The sky visibility grid on the GPU (render/gpu/sky_visibility.h): the grid baked on the CPU (SkyVisibilityGrid) and an
* image holding every cascade's points one after another along z (48 x 24 x 48 a cascade, room for the most cascades,
* each point a GpuSkyCell in RGBA32_UINT, read by exact texels), which the main pass and the fog read through the scene
* set. Points change only when baked, so the image is copied up only when the grid's version moves, through this frame
* in flight's own staging buffer. Made once, cleared to "not baked", so the scene set always has an image to point at.
*/
class VulkanSkyVisibility {
public:
    VulkanSkyVisibility(const VulkanDevice& device, ImmediateCommands& uploads, VkSampler nearest);

    /*
    * Follows the camera (centre) and bakes on the CPU against the frame's static level as planSkyBake says: all that
    * waits, refinement included, when everything is asked (a capture, whose picture must not depend on what came
    * before); all that waits at the quick rays when the grid starts afresh or more than half of it waits; otherwise
    * within the settings' budget, waiting points first, then refinement to the full rays. Called before the frame holds
    * a swapchain image, so a long bake keeps nothing of the screen's waiting. Only for frames that read it (a static
    * level, the setting on, no probes).
    */
    void bake(const FrameDescription& frame, const Vec3& centre, const SkyVisibilitySettings& settings, JobSystem& jobs, bool everything);

    // Records the copy of a grid changed since the last into commands, before anything this frame reads it.
    void record(VkCommandBuffer commands, std::uint32_t frameIndex);

    std::span<const ProbeCascade> getCascades() const;

    // The image as the scene set samples it.
    SampledImage sampled() const;

private:
    const VulkanDevice& device;
    VkSampler nearest;
    VulkanImage cells;
    std::array<VulkanBuffer, maxFramesInFlight> staging;
    SkyVisibilityGrid grid;
    std::vector<GpuSkyCell> packed;
    std::uint64_t uploadedVersion = 0;

    // What a point cost to bake at full rays last time (seconds on every core), for the budget; none known at first.
    double secondsPerCell = 0.0;
};
