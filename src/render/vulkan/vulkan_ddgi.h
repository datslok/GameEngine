#pragma once

#include "render/gpu/ddgi.h"
#include "render/gpu/probe_cascades.h"
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
* Global illumination's light probes on the GPU (render/gpu/ddgi.h), in cascades (render/gpu/probe_cascades.h): the
* three atlases holding every cascade's probes one after another (irradiance from still lights and from fast lights,
* RGBA16F, and distances, RG16F, written by compute as storage images) and the adaptive memory's change statistics
* beside the still irradiance, the buffers of the frame's probe rays (still light and distance, fast light as half
* floats, and which probes a fast light reached), each frame in flight's cascades (GpuProbeCascade), states and scene
* event restarts (GpuProbeRestart), and the passes that fill them: ddgi_seed.comp seeds the probes that start,
* ddgi_trace.comp traces every probe's rays, then ddgi_update_irradiance.comp (both irradiance atlases) and
* ddgi_update_distance.comp blend them into the atlases, one workgroup per probe, and ddgi_share.comp shares light
* between neighbours. Between updates the atlases are sampled (SurfaceRead): by the main pass, the debug balls, the
* seeds and the next frame's trace (last frame's probes, for the light that bounced more than once). They are shared by
* the frames in flight: barriers order each frame's update after the last frame's reads. Also draws the probes' debug
* balls. Needs ray queries.
*
* The cascades follow the camera: when one moves a cell, the plane its scroll brings in waits off (never traced or
* read) for its relocation (ddgi_relocate.comp, its records written into that frame in flight's read-back memory and
* read when the frame comes round again, so walking never waits for the GPU), then starts, seeded. Each frame's states
* (offsets, and which probes are traced) are worked out on the CPU from the records and what moves. Until the first
* records arrive every probe is traced where the grid puts it.
*/
class VulkanDdgi {
public:
    // Layouts of the scene's sets 0 and 1 and the ray tracing set, shared with the passes that bind them; sampler: linear,
    // clamped, for every read of the atlases.
    VulkanDdgi(const VulkanDevice& device, DescriptorLayouts& layouts, VkDescriptorSetLayout texturesLayout, VkDescriptorSetLayout frameLayout,
               VkDescriptorSetLayout rayLayout, VkSampler sampler, VkFormat colourFormat, VkFormat depthFormat);

    /*
    * Ready the atlases for this frame's cascades, before the frame's scene set (which points at them) is written:
    * cascades of another size or spacing than last frame's (or the first) get new atlases, cleared, which their first
    * update fills outright; cascades that moved forget the planes they brought in. Also picks what this frame reads: the
    * shared atlases while the settings share light between probes (switching that rewrites the probes' turns, after
    * waiting for the GPU). Leaves them readable. Then this frame's probe states, from the relocation's records (those
    * this frame in flight asked for when it last ran, its fence passed) and the draws that move, and the cascades,
    * measured from cameraOrigin, written to frame in flight frameIndex's upload memory (describeProbeStates,
    * describeProbeCascades). Outside any rendering block.
    */
    void prepare(VkCommandBuffer commands, std::span<const ProbeCascade> cascades, const GlobalIlluminationSettings& settings,
                 std::span<const DrawItem> draws, const Vec3& cameraOrigin, std::uint32_t frameIndex);

    /*
    * Record this frame's probe update for the cascades prepare() readied: relocate, seed, trace, then blend and share,
    * leaving the atlases readable for the frame's later passes. fastLights: the frame lists a fast light for the probe
    * rays' hits (ProbeHitLights::fast), so probes within its reach trace more rays. events: the scene events to start
    * this frame (none while the settings ignore them; a geometry change also relocates the probes); frameIndex: the frame
    * in flight, whose upload memory holds the restarts and the states prepare() wrote. Outside any rendering block.
    */
    void update(VkCommandBuffer commands, FrameDescriptors& descriptors, const ProbeSceneSets& sets, const GlobalIlluminationSettings& settings,
                const Vec3& sky, float frameSeconds, bool fastLights, std::span<const ProbeEvent> events, std::uint32_t frameIndex);

    // Read every relocation still out, once the GPU is idle (the frames in flight changed, so some frame in flight may
    // never come round again to read its own).
    void collectAllRelocations();

    // Draw cascade number cascade's last updated probes (the coarsest if there are fewer) as balls into the open rendering
    // block (colour and depth bound, the viewport set), with the scene's set 1 for the camera, the states and the
    // cascades. Nothing before the first update.
    void drawProbes(VkCommandBuffer commands, FrameDescriptors& descriptors, VkDescriptorSet textures, VkDescriptorSet frame, int cascade);

    bool hasProbes() const;

    // This frame in flight's probe states (GpuProbeState) and cascades (GpuProbeCascade), as prepare() wrote them, for
    // the scene set.
    VkDescriptorBufferInfo describeProbeStates(std::uint32_t frameIndex) const;
    VkDescriptorBufferInfo describeProbeCascades(std::uint32_t frameIndex) const;

    // The probes' states this frame and the relocation's records (empty until the first arrive), for tests and readouts.
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

    OwnedHandle<VkDescriptorSetLayout> seedSetLayout;
    OwnedHandle<VkPipelineLayout> seedLayout;
    std::unique_ptr<ComputePipeline> seedPipeline;

    OwnedHandle<VkDescriptorSetLayout> relocateSetLayout;
    OwnedHandle<VkPipelineLayout> relocateLayout;
    std::unique_ptr<ComputePipeline> relocatePipeline;

    OwnedHandle<VkDescriptorSetLayout> ballsSetLayout;
    OwnedHandle<VkPipelineLayout> ballsLayout;
    std::unique_ptr<VulkanPipeline> ballsPipeline;

    // This frame's cascades and last frame's (to find the planes a move brought in), and all their probes together.
    std::vector<ProbeCascade> cascades;
    int probes = 0;

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

    // The rays: room for rayCapacity a probe (the most the settings ask for) for tracedCapacity probes (the most traced
    // in a frame, at most maxTracedProbes), grown when either needs more; which probes a fast light reached, per probe.
    VulkanBuffer rays;
    VulkanBuffer fastRays;
    VulkanBuffer nearFastLight;
    int rayCapacity = 0;
    int tracedCapacity = 0;

    // This frame's traced probes (chooseTracedProbes) and each frame in flight's copy for the GPU: their count, then them.
    std::vector<std::int32_t> tracedProbes;
    std::vector<UploadArray> tracedUploads;

    // This frame's seeds (ddgi_seed.comp writes, the update reads; room for maxProbeSeeds), the probes they are for,
    // and each frame in flight's list of those probes.
    VulkanBuffer seeds;
    std::vector<ProbeSeed> frameSeeds;
    std::vector<UploadArray> seedUploads;

    // The frame time, ray cycles and rays' clock, carried from frame to frame, and how many frames the atlases have
    // been updated (seeds wait until the coarser cascades hold more frames than a seed stands for).
    ProbeSchedule schedule;
    int updatedFrames = 0;

    // Every probe's running scene event restart, moved on each frame, and its copy for each frame in flight, which the
    // update reads where it is written.
    std::vector<GpuProbeRestart> restarts;
    std::vector<UploadArray> restartUploads;

    // Set by prepare() when the atlases are new: the next update keeps nothing of them.
    bool freshAtlases = false;

    /*
    * The relocation: each frame in flight's read-back records and the probes (and lattice points) it asked for, read
    * when that frame comes round again; every probe's record (empty until the first arrive, pending while a scrolled-in
    * probe waits for its own); the probes to ask for at the next update (relocateAll: every one, after a new layout, a
    * geometry change or switching relocation); and whether the records hold moves (settings.probeRelocation) or only
    * classes.
    */
    struct RelocationFrame {
        VulkanBuffer records;
        std::unique_ptr<UploadArray> requests;
        std::vector<ProbeRelocationRequest> asked;
    };

    std::vector<RelocationFrame> relocationFrames;
    std::vector<GpuProbeRelocation> relocations;
    std::vector<int> relocationWanted;
    bool relocateAll = false;
    bool relocatedWithMoves = false;

    // This frame's states, the probes that start because of them or a scroll, and each frame in flight's copies of the
    // states and the cascades.
    std::vector<GpuProbeState> states;
    std::vector<int> restartedByStates;
    std::vector<UploadArray> stateUploads;
    std::vector<UploadArray> cascadeUploads;

    // The push constants of the last update, which the debug balls reuse.
    GpuProbeVolume lastPacked{};

    void createTracePipeline(VkDescriptorSetLayout texturesLayout, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout rayLayout);
    void createSeedPipeline(VkDescriptorSetLayout texturesLayout, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout rayLayout);
    void createRelocatePipeline(VkDescriptorSetLayout texturesLayout, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout rayLayout);
    void createBallsPipeline(DescriptorLayouts& layouts, VkDescriptorSetLayout texturesLayout, VkDescriptorSetLayout frameLayout,
                             VkFormat colourFormat, VkFormat depthFormat);

    // New atlases for cascades of a new size, cleared to zero (no light, no distance) in this frame's commands.
    void createAtlases(VkCommandBuffer commands, std::span<const ProbeCascade> next);

    // Room for this many rays a probe, for this many traced probes, in the ray buffers (waiting for the GPU when they grow).
    void reserveRays(int raysPerProbe, int traced);

    // Keep the records frame in flight frameIndex asked for, now that it has finished.
    void collectRelocation(std::uint32_t frameIndex);

    // Record the relocation of the wanted probes, their records left for the CPU to read when this frame comes round.
    void relocate(VkCommandBuffer commands, FrameDescriptors& descriptors, const ProbeSceneSets& sets, int iterations, std::uint32_t frameIndex);

    void seed(VkCommandBuffer commands, FrameDescriptors& descriptors, const ProbeSceneSets& sets, std::uint32_t frameIndex);
    void trace(VkCommandBuffer commands, FrameDescriptors& descriptors, const ProbeSceneSets& sets, int rayStride, const UploadArray& frameTraced);
    void blend(VkCommandBuffer commands, FrameDescriptors& descriptors, const ComputePipeline& pipeline, std::span<VulkanImage* const> atlases,
               const UploadArray& frameTraced, const UploadArray& frameRestarts, const UploadArray& frameStates, const UploadArray& frameCascades);
    void shareProbes(VkCommandBuffer commands, FrameDescriptors& descriptors, const GlobalIlluminationSettings& settings, float longKeep,
                     const UploadArray& frameRestarts, const UploadArray& frameStates, const UploadArray& frameCascades);

    // Point the set's binding at one of this frame in flight's upload arrays.
    void writeUpload(VkDescriptorSet set, std::uint32_t binding, const UploadArray& upload) const;

    // Fill the turns buffer: each probe's own turn, or identity rows.
    void writeProbeTurns(bool independent);

    // The ray buffers, in their bindings' order (rays, fast rays, which probes a fast light reached).
    std::array<const VulkanBuffer*, 3> rayBuffers() const;

    // What the trace and the update bind: the ray buffers, then the probes' turns.
    std::array<const VulkanBuffer*, 4> probeBuffers() const;
};
