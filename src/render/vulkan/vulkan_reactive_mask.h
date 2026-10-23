#pragma once

#include "render/gpu/transparency.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <cstdint>
#include <memory>

class VulkanDevice;

/*
* The reactive mask on the GPU (render/gpu/transparency.h): a copy of the scene image taken just before the glass and
* blended surfaces are drawn, the mask pass (reactive_mask.comp) comparing it with the picture after them, and the
* render-size R8 mask the anti-aliasing reads (the engine's resolve, FSR, DLSS). In frames without transparent surfaces
* (or with the mask off) the mask is cleared to 0 instead, once, so the readers can always read it.
*/
class VulkanReactiveMask {
public:
    // nearest samples the two pictures' exact texels.
    VulkanReactiveMask(const VulkanDevice& device, VkSampler nearest);

    // The copy and the mask at the render size, remade when it changes; the GPU must be idle (the renderer waits).
    void ensureSize(RenderSize render);

    // Record the copy of the picture before the transparent surfaces: the scene image (the main pass's or the
    // reflections' result) in, left as a copy source for the glass pass's own barrier to take from.
    void keepOpaque(VkCommandBuffer commands, VulkanImage& scene);

    // Record the mask pass after them: the scene image (left readable by the glass pass) in, the mask left readable by
    // compute shaders.
    void measure(VkCommandBuffer commands, FrameDescriptors& descriptors, VulkanImage& scene, const GpuReactivePass& pass);

    // Nothing reactive this frame: the mask cleared to 0 and left readable by compute shaders, unless it already is.
    void clear(VkCommandBuffer commands);

    // The mask, readable by compute shaders after measure or clear.
    VulkanImage& getMask();

private:
    const VulkanDevice& device;
    VkSampler nearest;
    std::unique_ptr<ComputePipeline> pipeline;

    RenderSize size;
    VulkanImage opaque;
    VulkanImage mask;

    // Whether the mask holds the clear's zeros, so frames without glass need not clear it again.
    bool cleared = false;
};
