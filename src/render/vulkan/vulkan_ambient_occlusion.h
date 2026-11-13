#pragma once

#include "render/gpu/ambient_occlusion.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class VulkanDevice;

/*
* Ambient occlusion on the GPU (render/gpu/ambient_occlusion.h): the pass (ambient_occlusion.comp) between the depth
* prepass and the main pass, the temporal accumulation (ambient_occlusion_temporal.comp) after it while anti-aliasing
* turns the noise, the denoiser (ambient_occlusion_denoise.comp, passes of an edge-aware a-trous filter) after that while
* there is no anti-aliasing or it gathers light, and the render-size image the main pass reads (scene set 1, binding 25):
* the denoiser's while it runs, else the accumulation's, else the pass's own.
*
* At half resolution (AmbientOcclusionSettings::halfResolution) those three run on the half-size grid, after a pass
* copying the picked pixels' depths into a half-size depth image (ambient_occlusion_downsample.comp), and an upsample
* (ambient_occlusion_upsample.comp) writes the render-size image the main pass reads instead.
*
* The accumulation's history is two pairs of images taking turns: each frame reads last frame's pair and writes the
* other, since a pixel reads its neighbours' history and cannot overwrite it in place.
*/
class VulkanAmbientOcclusion {
public:
    // nearest samples the depth's and the occlusion's exact texels; the pass reads the scene's sets (the textures'
    // and the frame's, laid out as texturesLayout and frameLayout) for the fog it takes off what it gathers.
    VulkanAmbientOcclusion(const VulkanDevice& device, VkSampler nearest, std::uint32_t framesInFlight, VkDescriptorSetLayout texturesLayout,
                           VkDescriptorSetLayout frameLayout);

    // One pass record per frame in flight, since the CPU writes the next frame's while the GPU may read the last.
    void setFramesInFlight(std::uint32_t count);

    // The images for this render size, at it or at half of it, remade when either changes; the GPU must be idle (the
    // renderer waits).
    void ensureSize(RenderSize render, bool halfResolution);

    // Whether the images exist (ensureSize was called), so the scene set can point at them, and whether they are made
    // for this render size and resolution.
    bool hasImages() const;
    bool fits(RenderSize render, bool halfResolution) const;
    bool isHalfResolution() const;

    // The image the main pass samples (set 1, binding 25): at half resolution the upsampled one; else the denoised one
    // when blurred is true, else the average the next occlude writes when accumulated is true, else the pass's own.
    // Asked before occlude, as the scene set is written first.
    SampledImage sampledOcclusion(bool blurred, bool accumulated) const;

    // Start the accumulation afresh next frame: a capture, or a frame without ambient occlusion in between.
    void forgetHistory();

    /*
    * A frame's ambient occlusion is recorded in four calls, always all four in this order, so the renderer can time
    * each apart (GpuPass::AmbientOcclusion, AoHistory, AoDenoise, AoUpsample); a step with nothing to do records nothing.
    *
    * occlude records the pass (at half resolution after the half-size depths, from the pass record's pick): the depth (the prepass's, left readable by compute) and last frame's finished picture's
    * pyramid in (every level, readable by compute), or standIn when there is none to gather from, and the blue noise
    * tile (AO_BLUE_NOISE_SIZE a side, its three channels the pass's noises, aoNoise). textures and frameSet are the
    * scene's sets, written already (the fog's grid in them integrated).
    */
    void occlude(VkCommandBuffer commands, FrameDescriptors& descriptors, VkDescriptorSet textures, VkDescriptorSet frameSet, std::uint32_t frameIndex,
                 VulkanImage& depth, const SampledImage* gatherPicture,
                 const SampledImage& standIn, const SampledImage& blueNoise, const GpuAmbientOcclusionPass& pass, bool previousCameraKnown);

    // The accumulation, while the pass record says to accumulate (its history carried over when last frame's was made
    // the same way and the previous camera is known).
    void accumulateHistory(VkCommandBuffer commands, FrameDescriptors& descriptors);

    // The denoiser when blur is true (with at least one pass); at the render's size, the occlusion is then left
    // readable by fragment shaders for the main pass either way.
    void denoise(VkCommandBuffer commands, FrameDescriptors& descriptors, bool blur);

    // At half resolution, the result brought to the render's size, left readable by fragment shaders for the main pass.
    void upsample(VkCommandBuffer commands, FrameDescriptors& descriptors);

private:
    const VulkanDevice& device;
    VkSampler nearest;
    std::unique_ptr<ComputePipeline> occlusionPipeline;
    std::unique_ptr<ComputePipeline> temporalPipeline;
    std::unique_ptr<ComputePipeline> denoisePipeline;
    std::unique_ptr<ComputePipeline> downsamplePipeline;
    std::unique_ptr<ComputePipeline> upsamplePipeline;
    std::vector<VulkanBuffer> passRecords;

    // The render's size and the grid the passes run on (half of it at half resolution, else the same).
    RenderSize renderSize;
    RenderSize size;
    bool halfResolution = false;
    VulkanImage halfDepth; // at half resolution: the picked pixels' depths
    VulkanImage upsampled; // at half resolution: the result at the render's size, which the main pass reads
    VulkanImage noisy;
    VulkanImage blurred;      // the denoiser's result
    VulkanImage denoiseSpare; // the denoiser's other image, its passes taking turns
    VulkanImage geometry;     // the occlusion pass's normals and view depths, for the denoiser

    // The accumulation's averages and their view depths and ages, two of each taking turns; latest is the pair last
    // written, and what it was made for (none while there is no history to carry over).
    std::array<VulkanImage, 2> historyLight;
    std::array<VulkanImage, 2> historyDepthAge;
    int latest = 0;
    std::optional<AoHistoryKey> lastHistory;

    // What occlude leaves for the steps after it in the same frame: its pass record, the depths the grid reads (the
    // half-size ones at half resolution) and the render's, the workgroups, whether to accumulate and whether last frame's
    // history is usable, the denoiser's passes, and the image the denoiser, the upsample or the main pass reads (the
    // pass's own, or the accumulation's average once it has run, or the denoiser's).
    struct FrameSteps {
        VulkanBuffer* record = nullptr;
        SampledImage depth;
        VulkanImage* renderDepth = nullptr;
        std::uint32_t groupsAcross = 0;
        std::uint32_t groupsDown = 0;
        AoHistoryKey key;
        bool accumulates = false;
        bool usable = false;
        int denoisePasses = 0;
        VulkanImage* result = nullptr;
    };
    FrameSteps steps;

    void denoiseInto(VkCommandBuffer commands, FrameDescriptors& descriptors, const VulkanBuffer& record, VulkanImage& source, int passes, std::uint32_t groupsAcross,
                     std::uint32_t groupsDown);
    void accumulate(VkCommandBuffer commands, FrameDescriptors& descriptors, const VulkanBuffer& record, const SampledImage& depthSample, bool usable,
                    std::uint32_t groupsAcross, std::uint32_t groupsDown);
};
