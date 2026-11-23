#pragma once

#include "math/vec3.h"
#include "render/gpu/liana_trees.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

class VulkanDevice;

/*
* The lianas' trees found among the planted plants each frame (ROADMAP phase 21 stage 3c-iii-c-iii;
* render/gpu/liana_trees.h), after the cells are planted and before the bones are posed: liana_trees.comp, a thread an
* anchor of every liana record held, writes the plant its tree is (BONE_NO_TREE none) for the lianas' vertex shaders
* (scene binding 53), which move the anchor by that plant's tree's bones.
*
* The tables (each planting layer's record, each set's cells) are each frame's own, written from the frame's sets; the
* found plants are shared by every frame in flight (each frame's pass waits for the frames before to finish reading).
* For bones-compare a frame's found plants and the slots' counts are copied back with the tables it used.
*/
class VulkanLianaTrees {
public:
    VulkanLianaTrees(const VulkanDevice& device, std::uint32_t framesInFlight);

    // Between frames only: anything not yet collected is dropped.
    void setFramesInFlight(std::uint32_t count);

    // Room for anchors anchors' plants (two a liana record held), before the scene set names the buffer; growing waits
    // for the GPU first (the draws in flight read it).
    void prepare(std::uint32_t anchors);

    // What the pass reads: the lianas' records, the slots' plants and counts, each set's slots, the field's layers.
    struct Inputs {
        VkBuffer records = VK_NULL_HANDLE;
        VkBuffer plants = VK_NULL_HANDLE;
        VkBuffer counts = VK_NULL_HANDLE;
        VkDeviceSize countsBytes = 0;
        std::vector<PlantingSetSlots> sets;
        std::size_t fieldLayers = 0;
        std::uint32_t anchors = 0;
        Vec3 eye;
        float fadeEnd = 0.0f;
        bool readBack = false; // for bones-compare: the found plants and the counts copied back
    };

    // Record the pass (outside any rendering block, after the planting and placed passes). Nothing without anchors.
    void record(VkCommandBuffer commands, FrameDescriptors& descriptors, const Inputs& inputs, std::uint32_t frameIndex);

    // A frame read back: the tables it found by, each anchor's plant, the slots' counts.
    struct Collected {
        LianaTreeTables tables;
        std::vector<std::uint32_t> found;
        std::vector<std::uint32_t> counts;
    };

    // After a frame's fence: what it read back (none unless asked).
    std::optional<Collected> collect(std::uint32_t frameIndex);

    // Each anchor's plant (VK_NULL_HANDLE before any room).
    VkBuffer getFound() const;

private:
    struct PerFrame {
        VulkanBuffer layers;
        VulkanBuffer cells;
        VulkanBuffer foundReadback;
        VulkanBuffer countsReadback;
        LianaTreeTables tables;
        std::uint32_t anchors = 0;
        VkDeviceSize countsBytes = 0;
        bool pending = false;
    };

    const VulkanDevice& device;
    std::unique_ptr<ComputePipeline> pass;
    std::vector<PerFrame> frames;
    VulkanBuffer found;
};
