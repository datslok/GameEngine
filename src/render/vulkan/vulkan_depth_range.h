#pragma once

#include "render/gpu/depth_range.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"

#include "volk/volk.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class VulkanDevice;

/*
* Measures the nearest and farthest surface on screen from the scene's depth image (render/gpu/depth_range.h), for
* fitting the sun's cascades: depth_range.comp reduces the image into a small buffer in GPU memory, which is copied into
* read-back memory the CPU reads once the frame has finished. The CPU plans each frame's shadows before the GPU draws its
* depth, so a measurement is used a frame or two later (as many as there are in flight); each frame in flight has its
* own read-back copy.
*/
class VulkanDepthRange {
public:
    // sampler: exact texels, clamped (the depths are read one by one).
    VulkanDepthRange(const VulkanDevice& device, VkSampler sampler, std::uint32_t framesInFlight);

    // Between frames only: any measurement not yet collected is dropped.
    void setFramesInFlight(std::uint32_t count);

    // Record the measurement of the depth image (left readable by compute shaders) into this frame's commands.
    void measure(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& depth);

    // Once the frame's commands have finished (its fence passed): what it measured. Each measurement is returned once;
    // none when the frame measured nothing.
    std::optional<ScreenDepthRange> collect(std::uint32_t frameIndex);

private:
    struct Result {
        VulkanBuffer readback;
        bool pending = false;
    };

    const VulkanDevice& device;
    VkSampler sampler;
    std::unique_ptr<ComputePipeline> pipeline;

    // Where the pass keeps its range: in GPU memory, since every workgroup ends with two atomics on it.
    VulkanBuffer range;
    std::vector<Result> results;
};
