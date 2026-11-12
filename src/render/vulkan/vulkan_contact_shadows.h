#pragma once

#include "render/gpu/contact_shadows.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <cstdint>
#include <memory>
#include <vector>

class VulkanDevice;

/*
* Contact shadows on the GPU (render/gpu/contact_shadows.h): the pass (contact_shadows.comp) after the depth prepass and
* ambient occlusion, before the main pass, and the render-size masks the main pass multiplies shadows by: the sun's
* (scene set 1, binding 31) and the local lights', a channel each (binding 32).
*/
class VulkanContactShadows {
public:
    // nearest samples the depth's and the noise's exact texels.
    VulkanContactShadows(const VulkanDevice& device, VkSampler nearest, std::uint32_t framesInFlight);

    // One pass record per frame in flight, since the CPU writes the next frame's while the GPU may read the last.
    void setFramesInFlight(std::uint32_t count);

    // The mask at the render size, remade when it changes; the GPU must be idle (the renderer waits).
    void ensureSize(RenderSize render);

    // Whether the mask exists (ensureSize was called), so the scene set can point at it.
    bool hasImages() const;

    // The sun's mask the main pass samples (set 1, binding 31), and the local lights' (binding 32, a channel each).
    SampledImage sampledShadow() const;
    SampledImage sampledLights() const;

    /*
    * Record the pass: the depth (the prepass's, left readable by compute), the prepass's solid mask and the blue noise
    * tile (AO_BLUE_NOISE_SIZE a side) in, the mask left readable by fragment shaders for the main pass.
    */
    void shade(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& depth, VulkanImage& solid, const SampledImage& blueNoise,
               const GpuContactShadowPass& pass);

private:
    const VulkanDevice& device;
    VkSampler nearest;
    std::unique_ptr<ComputePipeline> pipeline;
    std::vector<VulkanBuffer> passRecords;
    RenderSize size;
    VulkanImage mask;
    VulkanImage lightsMask;
};
