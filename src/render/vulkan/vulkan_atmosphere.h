#pragma once

#include "render/gpu/atmosphere.h"
#include "render/gpu/ray_tracing.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/owned_handle.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "render/vulkan/vulkan_pipeline.h"

#include "volk/volk.h"

#include <memory>
#include <optional>

class VulkanDevice;

/*
* The sky and atmosphere on the GPU (render/gpu/atmosphere.h): its three tables (transmittance, multiple scattering and
* the sky view, RGBA16F storage images, sampled by later passes), the flat ambient light it gives (a small GPU buffer the
* scene set points at), and the passes that fill them: the first two tables when the atmosphere changes, the sky view
* and the ambient every frame. Also draws the sky over the background. The atmosphere's numbers for the frame come in a
* buffer the renderer writes (GpuAtmosphere, the frame's scene buffers). The tables and the ambient buffer are shared by
* the frames in flight: barriers order each frame's writes after the last frame's reads.
*/
class VulkanAtmosphere {
public:
    // sampler: linear, clamped, for every read of the tables.
    VulkanAtmosphere(const VulkanDevice& device, DescriptorLayouts& layouts, VkSampler sampler, VkFormat colourFormat, VkFormat depthFormat);

    /*
    * Record this frame's tables for the atmosphere, its numbers (packAtmosphere) in params: the transmittance and
    * multiple scattering tables when they are new or the atmosphere's air changed, then the sky view and the flat ambient,
    * leaving the tables readable by compute and fragment shaders and the ambient by both. Outside any rendering block.
    */
    void update(VkCommandBuffer commands, FrameDescriptors& descriptors, const Atmosphere& atmosphere, const VkDescriptorBufferInfo& params);

    // Draw the sky over the background into the open rendering block (the scene image, and its depth for the equal test
    // at the far depth), the camera's rays in camera.
    void drawSky(VkCommandBuffer commands, FrameDescriptors& descriptors, const VkDescriptorBufferInfo& params, const GpuRayDebugView& camera);

    // What the scene set points at once update has run: the sky view table and the flat ambient buffer.
    SampledImage sampledSkyView() const;
    SampledImage sampledTransmittance() const;
    VkDescriptorBufferInfo describeSkyLight() const;

    // For tests.
    VulkanImage& getTransmittanceTable();
    VulkanImage& getMultipleScatteringTable();
    VulkanImage& getSkyViewTable();
    const VulkanBuffer& getSkyLight() const;

private:
    const VulkanDevice& device;
    VkSampler sampler;

    std::unique_ptr<ComputePipeline> transmittancePipeline;
    std::unique_ptr<ComputePipeline> multipleScatteringPipeline;
    std::unique_ptr<ComputePipeline> skyViewPipeline;
    std::unique_ptr<ComputePipeline> ambientPipeline;

    OwnedHandle<VkDescriptorSetLayout> skySetLayout;
    OwnedHandle<VkPipelineLayout> skyLayout;
    std::unique_ptr<VulkanPipeline> skyPipeline;

    VulkanImage transmittance;
    VulkanImage multipleScattering;
    VulkanImage skyView;
    VulkanBuffer skyLight;

    // The atmosphere the first two tables were made for (none yet).
    std::optional<Atmosphere> tablesMadeFor;

    void createSkyPipeline(DescriptorLayouts& layouts, VkFormat colourFormat, VkFormat depthFormat);
    void makeTables(VkCommandBuffer commands, FrameDescriptors& descriptors, const VkDescriptorBufferInfo& params);
};
