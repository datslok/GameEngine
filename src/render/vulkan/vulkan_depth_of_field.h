#pragma once

#include "render/gpu/depth_of_field.h"
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
* Depth of field on the GPU (render/gpu/depth_of_field.h): its six passes after anti-aliasing (the focus, prepare with
* the tiles, their neighbourhoods, the gather, the median, the composite), from the finished picture and the depth into
* a blurred copy at the output size, which motion blur, eye adaptation, bloom and tone mapping then read in its place.
* The finished picture itself is left alone, since next frame's reflections and gathered light read it as last frame's.
* The eased focus stays in GPU memory from frame to frame.
*/
class VulkanDepthOfField {
public:
    // nearest samples exact texels, linear the half-resolution layers between their texels.
    VulkanDepthOfField(const VulkanDevice& device, VkSampler nearest, VkSampler linear, std::uint32_t framesInFlight);

    // One pass record per frame in flight, since the CPU writes the next frame's while the GPU may read the last.
    void setFramesInFlight(std::uint32_t count);

    // The images at the output size (and the blur grid's and its tiles'), remade when it changes; the GPU must be idle
    // (the renderer waits).
    void ensureSize(RenderSize output);
    bool fits(RenderSize output) const;

    /*
    * Record the passes: the finished picture (output size) and the depth (render size; left readable by compute) in, the
    * blurred picture out, left readable by fragment and compute shaders alike (as the anti-aliasing leaves its result),
    * and returned.
    */
    VulkanImage& blur(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& picture, VulkanImage& depth,
                      const GpuDepthOfFieldPass& pass);

private:
    const VulkanDevice& device;
    VkSampler nearest;
    VkSampler linear;
    std::unique_ptr<ComputePipeline> focusPipeline;
    std::unique_ptr<ComputePipeline> preparePipeline;
    std::unique_ptr<ComputePipeline> neighboursPipeline;
    std::unique_ptr<ComputePipeline> gatherPipeline;
    std::unique_ptr<ComputePipeline> medianPipeline;
    std::unique_ptr<ComputePipeline> compositePipeline;
    std::vector<VulkanBuffer> passRecords;
    VulkanBuffer focus; // the eased focus's inverse distance, kept from frame to frame
    RenderSize size;
    VulkanImage prepared;     // each blur pixel's light and blur radius
    VulkanImage tiles;        // each tile's longest near radius
    VulkanImage neighbours;   // each tile's neighbourhood's longest
    VulkanImage farLayer;     // the gather's far layer
    VulkanImage nearLayer;    // the gather's near layer, its coverage in alpha
    VulkanImage farFiltered;  // the far layer's median
    VulkanImage nearFiltered; // the near layer's median
    VulkanImage result;       // the blurred picture
};
