#pragma once

#include "render/gpu/ddgi.h"
#include "render/gpu/probe_relocation.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/owned_handle.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/scene_layout.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "render/vulkan/vulkan_pipeline.h"

#include "volk/volk.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

class VulkanDevice;

// The descriptor sets a probe update reads the scene through, all already written for this frame.
struct ProbeSceneSets {
    VkDescriptorSet textures = VK_NULL_HANDLE;   // set 0, every texture
    VkDescriptorSet frame = VK_NULL_HANDLE;      // set 1, the frame's scene (instances, materials, lights)
    VkDescriptorSet rayScene = VK_NULL_HANDLE;   // set 2, the frame's top-level acceleration structure
};

/*
* Global illumination's light probes on the GPU (render/gpu/ddgi.h): the three atlases (irradiance from still lights and
* from fast lights, RGBA16F, and distances, RG16F, written by compute as storage images, the renderer's first) and the
* adaptive memory's change statistics beside the still irradiance, the
* buffers of the frame's probe rays (still light and distance, fast light as half floats, and which probes a fast light
* reached), each frame in flight's scene event restarts (GpuProbeRestart), and the passes that fill them: ddgi_trace.comp traces every probe's rays, then ddgi_update_irradiance.comp
* (both irradiance atlases) and ddgi_update_distance.comp blend them into the atlases, one workgroup per probe. Between
* updates the atlases are sampled (SurfaceRead): by the main pass, the debug balls and the next frame's trace (last
* frame's probes, for the light that bounced more than once). They are shared by the frames in flight: barriers order
* each frame's update after the last frame's reads. Also draws the probes' debug balls. Needs ray queries.
*
* Relocation and states (render/gpu/probe_relocation.h): a new volume, a geometry change event or switching relocation
* runs ddgi_relocate.comp once, which moves each probe off the static level and writes its record into read-back
* memory; the next frame waits for it (a one-off hitch) and reads it. From then on each frame's states (offsets, and
* which probes are traced) are worked out on the CPU from those records and what moves, written to that frame in
* flight's upload memory and read by every probe pass and the main pass. Until the first records arrive every probe is
* traced where the grid puts it.
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
    * Also picks what this frame reads: the shared atlases while the settings share light between probes (switching
    * that rewrites the probes' turns, after waiting for the GPU). Leaves them readable. And this frame's probe states,
    * from the relocation's records (waiting for a relocation recorded last frame) and the draws that move, written to
    * frame in flight frameIndex's upload memory (describeProbeStates). Outside any rendering block.
    */
    void prepare(VkCommandBuffer commands, const ProbeVolume& volume, const GlobalIlluminationSettings& settings, std::span<const DrawItem> draws,
                 std::uint32_t frameIndex);

    /*
    * Record this frame's probe update for the volume prepare() readied: trace, then blend, leaving the atlases readable
    * for the frame's later passes. fastLights: the frame lists a fast light for the probe rays' hits (ProbeHitLights::
    * fast), so probes within its reach trace more rays. events: the scene events to start this frame (none while the
    * settings ignore them; a geometry change also relocates the probes); frameIndex: the frame in flight, whose upload
    * memory holds the restarts and the states prepare() wrote. Outside any rendering block.
    */
    void update(VkCommandBuffer commands, FrameDescriptors& descriptors, const ProbeSceneSets& sets, const ProbeVolume& volume,
                const GlobalIlluminationSettings& settings, const Vec3& sky, float frameSeconds, bool fastLights,
                std::span<const ProbeEvent> events, std::uint32_t frameIndex);

    // Draw the last updated volume's probes as balls into the open rendering block (colour and depth bound, the
    // viewport set), with the scene's set 1 for the camera. Nothing before the first update.
    void drawProbes(VkCommandBuffer commands, FrameDescriptors& descriptors, VkDescriptorSet textures, VkDescriptorSet frame);

    bool hasProbes() const;

    // This frame in flight's probe states (GpuProbeState), as prepare() wrote them, for the scene set.
    VkDescriptorBufferInfo describeProbeStates(std::uint32_t frameIndex) const;

    // The probes' states this frame and the last relocation's records (empty until they arrive), for tests and readouts.
    const std::vector<GpuProbeState>& getProbeStates() const;
    const std::vector<GpuProbeRelocation>& getRelocations() const;

    // The atlases as the scene set samples them (valid after prepare()): the shared ones while probes share light.
    SampledImage sampledIrradiance() const;
    SampledImage sampledDistance() const;
    SampledImage sampledFastIrradiance() const;

    // The atlases, for tests: valid after the first update.
    VulkanImage& getIrradianceAtlas();
    VulkanImage& getDistanceAtlas();
    VulkanImage& getFastIrradianceAtlas();
    VulkanImage& getSharedIrradianceAtlas();

private:
    const VulkanDevice& device;
    VkSampler sampler;

    OwnedHandle<VkDescriptorSetLayout> raysLayout;
    OwnedHandle<VkPipelineLayout> traceLayout;
    std::unique_ptr<ComputePipeline> tracePipeline;
    std::unique_ptr<ComputePipeline> irradiancePipeline;
    std::unique_ptr<ComputePipeline> distancePipeline;
    std::unique_ptr<ComputePipeline> sharePipeline;

    OwnedHandle<VkDescriptorSetLayout> relocateSetLayout;
    OwnedHandle<VkPipelineLayout> relocateLayout;
    std::unique_ptr<ComputePipeline> relocatePipeline;

    OwnedHandle<VkDescriptorSetLayout> ballsSetLayout;
    OwnedHandle<VkPipelineLayout> ballsLayout;
    std::unique_ptr<VulkanPipeline> ballsPipeline;

    std::optional<ProbeVolume> currentVolume;
    VulkanImage irradiance;
    VulkanImage fastIrradiance;
    VulkanImage distance;

    // The adaptive memory's state beside the irradiance atlas's history (alpha): the change window's mean light and the
    // noise about it (render/gpu/ddgi.h, AdaptiveProbeTexel), RGBA16F, laid out as the irradiance atlas.
    VulkanImage changeStatistics;

    // What sharing between probes writes (ddgi_share.comp) and everything then reads, laid out as the irradiance atlas.
    VulkanImage sharedIrradiance;
    VulkanImage sharedFastIrradiance;

    // Each probe's own turn of its rays (GpuProbeTurn), read by the trace and the update; identity rows unless sharing.
    VulkanBuffer probeTurns;
    bool turnsIndependent = false;

    // This frame shares light between probes, still or fast (set by prepare()).
    bool sharing = false;
    VulkanBuffer rays;
    VulkanBuffer fastRays;
    VulkanBuffer nearFastLight;

    // The frame time, ray cycles and rays' clock, carried from frame to frame.
    ProbeSchedule schedule;

    // Every probe's running scene event restart, moved on each frame, and its copy for each frame in flight, which the
    // update reads where it is written.
    std::vector<GpuProbeRestart> restarts;
    std::vector<UploadArray> restartUploads;

    // Set by prepare() when the atlases are new: the next update keeps nothing of them.
    bool freshAtlases = false;

    // The relocation's records: written by the GPU into read-back memory, copied here once that frame has finished.
    VulkanBuffer relocationRecords;
    std::vector<GpuProbeRelocation> relocations;

    // A relocation is wanted at the next update, or was recorded and its records are not read yet; and whether the
    // records hold moves (settings.probeRelocation) or only classes.
    bool relocationRequested = false;
    bool relocationPending = false;
    bool relocatedWithMoves = false;

    // This frame's states, the probes that restart because of them (restartedProbes), and each frame in flight's copy.
    std::vector<GpuProbeState> states;
    std::vector<int> restartedByStates;
    std::vector<UploadArray> stateUploads;

    // The push constants of the last update, which the debug balls reuse.
    GpuProbeVolume lastPacked{};

    void createTracePipeline(VkDescriptorSetLayout texturesLayout, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout rayLayout);
    void createRelocatePipeline(VkDescriptorSetLayout texturesLayout, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout rayLayout);

    // Record the relocation of every probe of the volume, its records left for the CPU to read once the frame finishes.
    void relocate(VkCommandBuffer commands, FrameDescriptors& descriptors, const ProbeSceneSets& sets, const ProbeVolume& volume, int iterations);

    // Copy the records of a relocation recorded earlier, waiting for the GPU to finish it.
    void collectRelocation(const ProbeVolume& volume);
    void createBallsPipeline(DescriptorLayouts& layouts, VkDescriptorSetLayout texturesLayout, VkDescriptorSetLayout frameLayout,
                             VkFormat colourFormat, VkFormat depthFormat);

    // New atlases for a new volume, cleared to zero (no light, no distance) in this frame's commands.
    void createAtlases(VkCommandBuffer commands, const ProbeVolume& volume);

    void trace(VkCommandBuffer commands, FrameDescriptors& descriptors, const ProbeSceneSets& sets, int probes, int rayStride);
    void blend(VkCommandBuffer commands, FrameDescriptors& descriptors, const ComputePipeline& pipeline, std::span<VulkanImage* const> atlases,
               int probes, const UploadArray& frameRestarts, const UploadArray& frameStates);
    void shareProbes(VkCommandBuffer commands, FrameDescriptors& descriptors, const ProbeVolume& volume, const GlobalIlluminationSettings& settings,
                     float longKeep, const UploadArray& frameRestarts, const UploadArray& frameStates);

    // Point the set's binding at one of this frame in flight's upload arrays (its restarts or its states).
    void writeUpload(VkDescriptorSet set, std::uint32_t binding, const UploadArray& upload) const;

    // Fill the turns buffer: each probe's own turn, or identity rows.
    void writeProbeTurns(int probes, bool independent);

    // The ray buffers, in their bindings' order (rays, fast rays, which probes a fast light reached).
    std::array<const VulkanBuffer*, 3> rayBuffers() const;

    // What the trace and the update bind: the ray buffers, then the probes' turns.
    std::array<const VulkanBuffer*, 4> probeBuffers() const;
};
