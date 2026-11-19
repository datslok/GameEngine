#pragma once

#include "render/frames_in_flight.h"
#include "render/gpu/beacon_splat.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/owned_handle.h"
#include "render/vulkan/scene_layout.h"
#include "render/vulkan/vulkan_pipeline.h"

#include "volk/volk.h"

#include <array>
#include <memory>
#include <optional>
#include <span>

class VulkanDevice;

/*
* The beacons on the GPU (render/gpu/beacon_splat.h): drawn over the fogged picture, before the glass, one instance of six
* vertices each, no vertex buffer. They read the scene's sets (the frame's view and its fog, to dim each beacon by the fog
* in front of it) and their records, rewritten every frame (their levels flash) into a buffer for each frame in flight.
*/
class VulkanBeacons {
public:
    // bindlessLayout and sceneLayout: the scene's sets 0 and 1, kept in place; the records are set 2.
    VulkanBeacons(const VulkanDevice& device, DescriptorLayouts& layouts, VkDescriptorSetLayout bindlessLayout, VkDescriptorSetLayout sceneLayout,
                  VkFormat colourFormat, VkFormat depthFormat);

    /*
    * Draw the beacons into the open rendering block (the picture, added to, and its depth, tested and not written), each
    * measured from cameraOrigin, with the frame's scene sets.
    */
    void draw(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, std::span<const PlacedBeacon> beacons,
              const Vec3& cameraOrigin, VkDescriptorSet bindlessSet, VkDescriptorSet sceneSet);

private:
    const VulkanDevice& device;
    OwnedHandle<VkDescriptorSetLayout> setLayout;
    OwnedHandle<VkPipelineLayout> layout;
    std::unique_ptr<VulkanPipeline> pipeline;
    std::array<std::optional<UploadArray>, maxFramesInFlight> records;
};
