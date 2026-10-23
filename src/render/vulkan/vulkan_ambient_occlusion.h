#pragma once

#include "render/gpu/ambient_occlusion.h"
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
* Ambient occlusion on the GPU (render/gpu/ambient_occlusion.h): the pass (ambient_occlusion.comp) between the depth
* prepass and the main pass, the blur (ambient_occlusion_blur.comp) after it while there is no anti-aliasing, and the
* render-size image the main pass reads (scene set 1, binding 25): the blur's while it runs, else the pass's own.
*/
class VulkanAmbientOcclusion {
public:
    // nearest samples the depth's and the occlusion's exact texels.
    VulkanAmbientOcclusion(const VulkanDevice& device, VkSampler nearest, std::uint32_t framesInFlight);

    // One pass record per frame in flight, since the CPU writes the next frame's while the GPU may read the last.
    void setFramesInFlight(std::uint32_t count);

    // The images at the render size, remade when it changes; the GPU must be idle (the renderer waits).
    void ensureSize(RenderSize render);

    // Whether the images exist (ensureSize was called), so the scene set can point at them.
    bool hasImages() const;

    // The image the main pass samples (set 1, binding 25): the blurred one when blurred is true, else the pass's own.
    SampledImage sampledOcclusion(bool blurred) const;

    /*
    * Record the pass, and the blur when blur is true: the depth (the prepass's, left readable by compute) in, the
    * occlusion left readable by fragment shaders for the main pass.
    */
    void occlude(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& depth,
                 const GpuAmbientOcclusionPass& pass, bool blur);

private:
    const VulkanDevice& device;
    VkSampler nearest;
    std::unique_ptr<ComputePipeline> occlusionPipeline;
    std::unique_ptr<ComputePipeline> blurPipeline;
    std::vector<VulkanBuffer> passRecords;

    RenderSize size;
    VulkanImage noisy;
    VulkanImage blurred;
};
