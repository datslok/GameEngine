#pragma once

#include "render/gpu/screen_reflections.h"
#include "render/gpu/upscaling.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/owned_handle.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <cstdint>
#include <memory>
#include <vector>

class VulkanDevice;

/*
* A pyramid of the depth image at the render size (depth_pyramid.comp): level 0 the depth itself, each level above the
* nearest or the farthest of the 2x2 texels below it (DepthReduction, render/gpu/screen_reflections.h). The reflections'
* march keeps the nearest; the farthest is for testing what a depth hides (occlusion culling, docs/research/OCCLUSION.md).
*/
class VulkanDepthPyramid {
public:
    // nearest: exact texels, clamped, for reading the depth image.
    VulkanDepthPyramid(const VulkanDevice& device, VkSampler nearest, DepthReduction reduction, const char* name);

    // The levels at the render size, remade when it changes; the GPU must be idle (the renderer waits).
    void ensureSize(RenderSize render);

    // Record every level from the depth image (left readable by compute); the pyramid ends in readState (by compute
    // unless told; the occlusion pyramids by mesh shaders too).
    void build(VkCommandBuffer commands, FrameDescriptors& descriptors, VulkanImage& depth, ImageState readState = ImageState::ComputeRead);

    VulkanImage& getImage();
    std::uint32_t getLevels() const;

private:
    const VulkanDevice& device;
    VkSampler nearest;
    DepthReduction reduction;
    const char* name;
    std::unique_ptr<ComputePipeline> pipeline;

    RenderSize size;
    VulkanImage image;
    std::vector<OwnedHandle<VkImageView>> levels; // a view of each level alone, for writing one level at a time
};
