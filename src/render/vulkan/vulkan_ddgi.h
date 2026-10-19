#pragma once

#include "render/gpu/ddgi.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/owned_handle.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "render/vulkan/vulkan_pipeline.h"

#include "volk/volk.h"

#include <cstdint>
#include <memory>
#include <optional>

class VulkanDevice;

// The descriptor sets a probe update reads the scene through, all already written for this frame.
struct ProbeSceneSets {
    VkDescriptorSet textures = VK_NULL_HANDLE;   // set 0, every texture
    VkDescriptorSet frame = VK_NULL_HANDLE;      // set 1, the frame's scene (instances, materials, lights)
    VkDescriptorSet rayScene = VK_NULL_HANDLE;   // set 2, the frame's top-level acceleration structure
};

/*
* Global illumination's light probes on the GPU (render/gpu/ddgi.h): the two atlases (irradiance, RGBA16F, and distances,
* RG16F, written by compute as storage images, the renderer's first), the buffer of the frame's probe rays, and the
* passes that fill them: ddgi_trace.comp traces every probe's rays, then ddgi_update_irradiance.comp and
* ddgi_update_distance.comp blend them into the atlases, one workgroup per probe. Between updates the atlases are
* sampled (SurfaceRead): by the main pass, the debug balls and the next frame's trace (last frame's probes, for the light
* that bounced more than once). They are shared by the frames in flight: barriers order each frame's update after the
* last frame's reads. Also draws the probes' debug balls. Needs ray queries.
*/
class VulkanDdgi {
public:
    // Layouts of the scene's sets 0 and 1 and the ray tracing set, shared with the passes that bind them; sampler: linear,
    // clamped, for every read of the atlases.
    VulkanDdgi(const VulkanDevice& device, DescriptorLayouts& layouts, VkDescriptorSetLayout texturesLayout, VkDescriptorSetLayout frameLayout,
               VkDescriptorSetLayout rayLayout, VkSampler sampler, VkFormat colourFormat, VkFormat depthFormat);

    /*
    * Ready the atlases for this frame's volume, before the frame's scene set (which points at them) is written: a volume
    * that differs from last frame's (or the first) gets new atlases, cleared, which its first update fills outright.
    * Leaves them readable. Outside any rendering block.
    */
    void prepare(VkCommandBuffer commands, const ProbeVolume& volume);

    /*
    * Record this frame's probe update for the volume prepare() readied: trace, then blend, leaving the atlases readable
    * for the frame's later passes. Outside any rendering block.
    */
    void update(VkCommandBuffer commands, FrameDescriptors& descriptors, const ProbeSceneSets& sets, const ProbeVolume& volume,
                const GlobalIlluminationSettings& settings, const Vec3& sky, float frameSeconds);

    // Draw the last updated volume's probes as balls into the open rendering block (colour and depth bound, the
    // viewport set), with the scene's set 1 for the camera. Nothing before the first update.
    void drawProbes(VkCommandBuffer commands, FrameDescriptors& descriptors, VkDescriptorSet textures, VkDescriptorSet frame);

    bool hasProbes() const;

    // The atlases as the scene set samples them (valid after prepare()).
    SampledImage sampledIrradiance() const;
    SampledImage sampledDistance() const;

    // The atlases, for tests: valid after the first update.
    VulkanImage& getIrradianceAtlas();
    VulkanImage& getDistanceAtlas();

private:
    const VulkanDevice& device;
    VkSampler sampler;

    OwnedHandle<VkDescriptorSetLayout> raysLayout;
    OwnedHandle<VkPipelineLayout> traceLayout;
    std::unique_ptr<ComputePipeline> tracePipeline;
    std::unique_ptr<ComputePipeline> irradiancePipeline;
    std::unique_ptr<ComputePipeline> distancePipeline;

    OwnedHandle<VkDescriptorSetLayout> ballsSetLayout;
    OwnedHandle<VkPipelineLayout> ballsLayout;
    std::unique_ptr<VulkanPipeline> ballsPipeline;

    std::optional<ProbeVolume> currentVolume;
    VulkanImage irradiance;
    VulkanImage distance;
    VulkanBuffer rays;
    std::uint64_t frameNumber = 0;

    // Set by prepare() when the atlases are new: the next update keeps nothing of them.
    bool freshAtlases = false;

    // The push constants of the last update, which the debug balls reuse.
    GpuProbeVolume lastPacked{};

    void createTracePipeline(VkDescriptorSetLayout texturesLayout, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout rayLayout);
    void createBallsPipeline(DescriptorLayouts& layouts, VkDescriptorSetLayout texturesLayout, VkDescriptorSetLayout frameLayout,
                             VkFormat colourFormat, VkFormat depthFormat);

    // New atlases for a new volume, cleared to zero (no light, no distance) in this frame's commands.
    void createAtlases(VkCommandBuffer commands, const ProbeVolume& volume);

    void trace(VkCommandBuffer commands, FrameDescriptors& descriptors, const ProbeSceneSets& sets, int probes, int raysPerProbe);
    void blend(VkCommandBuffer commands, FrameDescriptors& descriptors, const ComputePipeline& pipeline, VulkanImage& atlas, int probes);
};
