#pragma once

#include "render/gpu/upscaling.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/owned_handle.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <cstdint>
#include <memory>
#include <vector>

class VulkanDevice;

/*
* A picture with ever blurrier levels at the render size (colour_pyramid.comp): each level the average of the 2x2
* texels below it, so a rough reflection or frosted glass reads the level as wide as its cone. Screen-space reflections
* build one from last frame's finished picture; refraction from this frame's picture before the glass, whose level 0 a
* pass of its own writes first (VulkanOpaquePicture).
*/
class VulkanColourPyramid {
public:
    // linear: bilinear and clamped, one level, for reading the picture and each level below.
    VulkanColourPyramid(const VulkanDevice& device, VkSampler linear, const char* name);

    // The levels at the render size (as many as the screen-space reflections' depth pyramid), remade when it changes;
    // the GPU must be idle (the renderer waits).
    void ensureSize(RenderSize render);

    /*
    * Record every level from picture (any size; left readable by compute), or with no picture every level above 0,
    * which the caller has just written as a storage image. The whole pyramid ends in finished.
    */
    void build(VkCommandBuffer commands, FrameDescriptors& descriptors, VulkanImage* picture, ImageState finished);

    VulkanImage& getImage();
    VkImageView getLevelView(std::uint32_t level) const;
    std::uint32_t getLevels() const;

private:
    const VulkanDevice& device;
    VkSampler linear;
    const char* name;
    std::unique_ptr<ComputePipeline> pipeline;

    RenderSize size;
    VulkanImage image;
    std::vector<OwnedHandle<VkImageView>> levels;
};
