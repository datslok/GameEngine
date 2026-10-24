#pragma once

#include "render/gpu/transparency.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/vulkan_colour_pyramid.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <cstdint>
#include <memory>

class VulkanDevice;

/*
* The picture before the glass (render/gpu/transparency.h), kept whenever glass is drawn: level 0 (opaque_picture.comp)
* the scene image's colour with each pixel's distance along the view in alpha, the levels above ever blurrier
* (VulkanColourPyramid). Refracting glass reads it where its view ray lands, at the level its blur needs; the reactive
* mask compares the picture after the glass with its level 0.
*/
class VulkanOpaquePicture {
public:
    // nearest: exact texels (the scene, the depth); linear: bilinear, one level (building the levels); levels:
    // trilinear and clamped, every level (the glass reading it).
    VulkanOpaquePicture(const VulkanDevice& device, VkSampler nearest, VkSampler linear, VkSampler levels);

    // At the render size, remade when it changes; the GPU must be idle (the renderer waits).
    void ensureSize(RenderSize render);

    /*
    * Record keeping it: the scene image (the main pass's or the reflections' result) and its depth in, both left
    * readable by compute for the glass pass's own barriers to take from; the picture left readable by fragment and
    * compute shaders.
    */
    void keep(VkCommandBuffer commands, FrameDescriptors& descriptors, VulkanImage& scene, VulkanImage& depth, const GpuOpaquePicturePass& pass);

    // The whole picture, every level, as the glass reads it (set 2, binding 1).
    SampledImage sampled() const;

    VulkanImage& getImage();
    std::uint32_t getLevels() const;

private:
    const VulkanDevice& device;
    VkSampler nearest;
    VkSampler levelsSampler;
    std::unique_ptr<ComputePipeline> pipeline;
    std::unique_ptr<VulkanColourPyramid> pyramid;
    RenderSize size;
};
