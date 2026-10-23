#pragma once

#include "render/gpu/mirrored_light.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <cstdint>
#include <memory>
#include <vector>

class VulkanDevice;

// The sets the mirrored light pass reads before its own: every texture, the frame's scene and the ray tracing set.
struct MirroredLightSets {
    VkDescriptorSet textures = VK_NULL_HANDLE;
    VkDescriptorSet scene = VK_NULL_HANDLE;
    VkDescriptorSet rayScene = VK_NULL_HANDLE;
};

/*
* Mirrored lights on the GPU (render/gpu/mirrored_light.h), only on GPUs with ray queries: the pass (mirrored_light.comp)
* between the depth prepass and the main pass, and the two render-size images it leaves for the main pass (scene set 1,
* bindings 22 and 23): the light arriving by way of the mirrors, and the way it arrives.
*/
class VulkanMirroredLights {
public:
    // The pass reads the scene's sets (these layouts: textures, the frame's scene, the ray tracing set) before its own;
    // nearest samples the depth's exact texels.
    VulkanMirroredLights(const VulkanDevice& device, VkSampler nearest, VkDescriptorSetLayout textures, VkDescriptorSetLayout scene,
                         VkDescriptorSetLayout rayScene, std::uint32_t framesInFlight);

    // One pass record per frame in flight, since the CPU writes the next frame's while the GPU may read the last.
    void setFramesInFlight(std::uint32_t count);

    // The images at the render size, remade when it changes; the GPU must be idle (the renderer waits).
    void ensureSize(RenderSize render);

    // Whether the images exist (ensureSize was called), so the scene set can point at them.
    bool hasImages() const;

    // The images as the main pass samples them (set 1, bindings 22 and 23), once the pass has run.
    SampledImage sampledLight() const;
    SampledImage sampledDirection() const;

    /*
    * Record the pass: the depth (the prepass's, left readable by compute) in, the images written and left readable by
    * fragment shaders for the main pass.
    */
    void light(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& depth, const GpuMirroredLightPass& pass,
               const MirroredLightSets& sets);

private:
    const VulkanDevice& device;
    VkSampler nearest;
    std::unique_ptr<ComputePipeline> pipeline;
    std::vector<VulkanBuffer> passRecords;

    RenderSize size;
    VulkanImage arriving;
    VulkanImage directions;
};
