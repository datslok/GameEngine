#pragma once

#include "render/frames_in_flight.h"
#include "render/gpu/stars.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/owned_handle.h"
#include "render/vulkan/scene_layout.h"
#include "render/vulkan/vulkan_pipeline.h"

#include "volk/volk.h"

#include <array>
#include <memory>
#include <optional>

class VulkanDevice;

/*
* The stars on the GPU (render/gpu/stars.h): drawn over the sky in the main pass, one instance of six vertices each, no
* vertex buffer. The stars' records live in a buffer for each frame in flight, written only when the frame's sky holds
* a different catalogue than that buffer last did, so a frame never rewrites what an earlier frame still reads.
*/
class VulkanStars {
public:
    VulkanStars(const VulkanDevice& device, DescriptorLayouts& layouts, VkFormat colourFormat, VkFormat depthFormat);

    /*
    * Draw the sky's stars into the open rendering block (the scene image and its depth, for the equal test at the far
    * depth), dimmed by the air: the atmosphere's numbers in atmosphere, its transmittance table in transmittance, the
    * camera's rays in camera.
    */
    void draw(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, const StarrySky& sky,
              const VkDescriptorBufferInfo& atmosphere, const SampledImage& transmittance, const GpuRayDebugView& camera);

private:
    // One frame in flight's star records, and the catalogue they were written from (none yet).
    struct FrameStars {
        std::optional<UploadArray> records;
        std::shared_ptr<const std::vector<Star>> writtenFrom;
    };

    const VulkanDevice& device;
    OwnedHandle<VkDescriptorSetLayout> setLayout;
    OwnedHandle<VkPipelineLayout> layout;
    std::unique_ptr<VulkanPipeline> pipeline;
    std::array<FrameStars, maxFramesInFlight> frames;
};
