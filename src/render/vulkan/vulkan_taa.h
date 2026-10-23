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
* moving objects' motion into, at the render size like the scene image, and two history images at the output size that
* take turns (one read as last frame's result while the other is written as this frame's). The resolve (taa.comp)
* blends the scene image, upscaling it, into the history, and that result is the finished picture everything after it
* reads (eye adaptation, bloom, tone mapping, captures).
*/
class VulkanTaa {
public:
    // nearest: exact texels, clamped; filtered: bilinear and clamped (the history, read between texels).
    VulkanTaa(const VulkanDevice& device, VkSampler nearest, VkSampler filtered);

    // The history at the output's size, remade (and forgotten) when it changes, and the velocity image at the render
    // size, remade when that changes (the history is kept: it is still the output's picture).
    void ensureSize(std::uint32_t outputWidth, std::uint32_t outputHeight, RenderSize render);

    VulkanImage& getVelocity();

    // Whether last frame's result may be blended in: false at first, after a resize, and once forgotten.
    bool hasHistory() const;
    void forgetHistory();

    /*
    * Record the resolve: the scene (readable by fragment and compute shaders), its depth (readable by compute), the
    * velocity image (drawn this frame) and the reactive mask (readable by compute) in, this frame's history out, left
    * readable by fragment and compute shaders. It
    * becomes the history the next resolve reads.
    */
    void resolve(VkCommandBuffer commands, FrameDescriptors& descriptors, VulkanImage& scene, VulkanImage& depth, VulkanImage& reactive,
                 const GpuTaaPass& pass);

    /*
    * Record the camera motion pass (motion_fill.comp): every velocity pixel the velocity pass left at "no motion" gets
    * the camera's motion from its depth, so the image holds a motion everywhere, as FSR needs. The depth (readable by
    * compute) and the velocity image (drawn this frame) in, the velocity image left as a storage image.
    */
    void fillCameraMotion(VkCommandBuffer commands, FrameDescriptors& descriptors, VulkanImage& depth, const GpuMotionFillPass& pass);

    // The last resolve's result: the finished picture at the output's size.
    VulkanImage& getResult();

private:
    const VulkanDevice& device;
    VkSampler nearest;
    VkSampler filtered;
    std::unique_ptr<ComputePipeline> pipeline;
    std::unique_ptr<ComputePipeline> fillPipeline;

    VulkanImage velocity;
    std::array<VulkanImage, 2> history;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    RenderSize velocitySize;

    // Which history this frame writes; the other holds last frame's.
    std::uint32_t written = 0;
    bool historyValid = false;
};
