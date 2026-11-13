#pragma once

#include "render/gpu/motion_blur.h"
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
* Motion blur on the GPU (render/gpu/motion_blur.h): its three passes after anti-aliasing (the tiles, their
* neighbourhoods, the gather), from the finished picture, the velocity image and the depth into a blurred copy at the
* output size, which eye adaptation, bloom and tone mapping then read in its place. The finished picture itself is left
* alone, since next frame's reflections and gathered light read it as last frame's.
*/
class VulkanMotionBlur {
public:
    // nearest samples every image's exact texels.
    VulkanMotionBlur(const VulkanDevice& device, VkSampler nearest, std::uint32_t framesInFlight);

    // One pass record per frame in flight, since the CPU writes the next frame's while the GPU may read the last.
    void setFramesInFlight(std::uint32_t count);

    // The images at the output size (and its tiles'), remade when it changes; the GPU must be idle (the renderer waits).
    void ensureSize(RenderSize output);
    bool fits(RenderSize output) const;

    /*
    * Record the passes: the finished picture (output size), the velocity image and the depth (render size; the depth
    * left readable by compute) in, the blurred picture out, left readable by fragment and compute shaders alike (as the
    * anti-aliasing leaves its result), and returned.
    */
    VulkanImage& blur(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& picture, VulkanImage& velocity,
                      VulkanImage& depth, const GpuMotionBlurPass& pass);

private:
    const VulkanDevice& device;
    VkSampler nearest;
    std::unique_ptr<ComputePipeline> tilesPipeline;
    std::unique_ptr<ComputePipeline> neighboursPipeline;
    std::unique_ptr<ComputePipeline> gatherPipeline;
    std::vector<VulkanBuffer> passRecords;
    RenderSize size;
    VulkanImage prepared;   // each output pixel's half-blur and view depth
    VulkanImage tiles;      // each tile's longest half-blur
    VulkanImage neighbours; // each tile's neighbourhood's longest
    VulkanImage blurred;    // the result
};
