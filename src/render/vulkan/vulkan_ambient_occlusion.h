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
* the denoiser's while it runs, else the
* accumulation's, else the pass's own.
*
* The accumulation's history is two pairs of images taking turns: each frame reads last frame's pair and writes the
* other, since a pixel reads its neighbours' history and cannot overwrite it in place.
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

    // The image the main pass samples (set 1, binding 25): the denoised one when blurred is true, else the average the
    // next occlude writes when accumulated is true, else the pass's own. Asked before occlude, as the scene set is
    // written first.
    SampledImage sampledOcclusion(bool blurred, bool accumulated) const;

    // Start the accumulation afresh next frame: a capture, or a frame without ambient occlusion in between.
    void forgetHistory();

    /*
    * Record the pass, the accumulation while the pass record says to accumulate (its history carried over when last
    * frame's was made the same way and previousCameraKnown), and the denoiser when blur is true (with at least one
    * pass): the depth (the prepass's, left readable by compute) and last frame's finished picture's pyramid in (every
    * level, readable by compute), or standIn when there is none to gather from, and the blue noise tile (AO_BLUE_NOISE_SIZE
    * a side, its three channels the pass's noises, aoNoise), the occlusion left readable by fragment
    * shaders for the main pass.
    */
    void occlude(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& depth, const SampledImage* gatherPicture,
                 const SampledImage& standIn, const SampledImage& blueNoise, const GpuAmbientOcclusionPass& pass, bool blur, bool previousCameraKnown);

private:
    const VulkanDevice& device;
    VkSampler nearest;
    std::unique_ptr<ComputePipeline> occlusionPipeline;
    std::unique_ptr<ComputePipeline> temporalPipeline;
    std::unique_ptr<ComputePipeline> denoisePipeline;
    std::vector<VulkanBuffer> passRecords;

    RenderSize size;
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

    void denoiseInto(VkCommandBuffer commands, FrameDescriptors& descriptors, const VulkanBuffer& record, VulkanImage& source, int passes, std::uint32_t groupsAcross,
                     std::uint32_t groupsDown);
    void accumulate(VkCommandBuffer commands, FrameDescriptors& descriptors, const VulkanBuffer& record, const SampledImage& depthSample, bool usable,
                    std::uint32_t groupsAcross, std::uint32_t groupsDown);
};
