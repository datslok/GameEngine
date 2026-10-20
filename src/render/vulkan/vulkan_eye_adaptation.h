#pragma once

#include "render/gpu/eye_adaptation.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"

#include "volk/volk.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class VulkanDevice;

/*
* Eye adaptation on the GPU (render/gpu/eye_adaptation.h): the histogram pass over the lit scene image
* (eye_histogram.comp) into a small buffer, then the adaptation pass (eye_adaptation.comp) into the adapted exposure,
* which stays in GPU memory from frame to frame and which tone mapping reads the same frame, so the CPU never waits for
* it. A copy goes to read-back memory per frame in flight, for the readout only.
*/
class VulkanEyeAdaptation {
public:
    // sampler: exact texels, clamped (the scene is read pixel by pixel).
    VulkanEyeAdaptation(const VulkanDevice& device, VkSampler sampler, std::uint32_t framesInFlight);

    // Between frames only: any reading not yet collected is dropped.
    void setFramesInFlight(std::uint32_t count);

    /*
    * Record this frame's measurement of the scene image (left readable by fragment and compute shaders) and its step
    * towards the target, seconds after the last (snap: straight to the target), leaving the adapted exposure readable
    * by fragment shaders.
    */
    void adapt(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& scene, const ToneMappingSettings& settings,
               float seconds, bool snap);

    // The adapted exposure, for tone mapping (its contents only meaningful once adapt has run).
    VkDescriptorBufferInfo describeAdaptation() const;

    // Once the frame's commands have finished (its fence passed): what it adapted to. Each reading is returned once.
    std::optional<GpuEyeAdaptation> collect(std::uint32_t frameIndex);

    // For tests.
    const VulkanBuffer& getHistogram() const;

private:
    struct Reading {
        VulkanBuffer readback;
        bool pending = false;
    };

    const VulkanDevice& device;
    VkSampler sampler;
    std::unique_ptr<ComputePipeline> histogramPipeline;
    std::unique_ptr<ComputePipeline> adaptationPipeline;

    VulkanBuffer histogram;
    VulkanBuffer adaptation;
    std::vector<Reading> readings;
};
