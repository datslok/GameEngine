#pragma once

#include "render/gpu/taa.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <array>
#include <cstdint>
#include <memory>

class VulkanDevice;

/*
* Temporal anti-aliasing's images and its resolve pass (render/gpu/taa.h): the velocity image the velocity pass draws
* moving objects' motion into, and two history images that take turns (one read as last frame's result while the
* other is written as this frame's), all the frame's size. The resolve (taa.comp) blends the scene image into the
* history and copies the result back over the scene image, so everything after it (eye adaptation, bloom, tone
* mapping, captures) reads the smoothed picture where it always did.
*/
class VulkanTaa {
public:
    // nearest: exact texels, clamped; filtered: bilinear and clamped (the history, read between texels).
    VulkanTaa(const VulkanDevice& device, VkSampler nearest, VkSampler filtered);

    // The images at the frame's size, remade (and the history forgotten) when it changes.
    void ensureSize(std::uint32_t width, std::uint32_t height);

    VulkanImage& getVelocity();

    // Whether last frame's result may be blended in: false at first, after a resize, and once forgotten.
    bool hasHistory() const;
    void forgetHistory();

    /*
    * Record the resolve: the scene (readable by fragment and compute shaders), its depth (readable by compute) and the
    * velocity image (drawn this frame) in, this frame's history out, copied over the scene image, which is left
    * readable again. The result becomes the history the next resolve reads.
    */
    void resolve(VkCommandBuffer commands, FrameDescriptors& descriptors, VulkanImage& scene, VulkanImage& depth, const GpuTaaPass& pass);

private:
    const VulkanDevice& device;
    VkSampler nearest;
    VkSampler filtered;
    std::unique_ptr<ComputePipeline> pipeline;

    VulkanImage velocity;
    std::array<VulkanImage, 2> history;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    // Which history this frame writes; the other holds last frame's.
    std::uint32_t written = 0;
    bool historyValid = false;
};
