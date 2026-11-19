#pragma once

#include "render/gpu/placed_drawing.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

class VulkanDevice;
class VulkanPlanting;

/*
* The placed plants drawn (ROADMAP phase 18, the valley's stage 2b2; render/gpu/placed_drawing.h): each frame the table of
* the frame's planted parts and the blocks of the planting's held slots made on the CPU, then four passes over the
* blocks (placed_cull_count, placed_cull_scan, placed_cull_commands, placed_cull_write) write the camera's placed
* entries and one indirect command a group, which the prepass and the main pass draw style by style (draws()). The
* table, blocks and view are each frame in flight's own in upload memory; the entries and commands its own in GPU
* memory; the block counts and totals shared (each frame's passes finish with them before the next frame's start).
*/
class VulkanPlacedDrawing {
public:
    VulkanPlacedDrawing(const VulkanDevice& device, std::uint32_t framesInFlight);

    // Between frames only: anything not yet collected is dropped.
    void setFramesInFlight(std::uint32_t count);

    /*
    * The frame's table and blocks, after the planting's prepare: none (returns false) without planted parts, a field
    * or a held cell. placement says where a mesh's indices are. Throws std::invalid_argument for parts the passes
    * cannot draw (buildPlacedTable).
    */
    bool prepare(const FrameDescription& frame, const VulkanPlanting& planting, const std::function<MeshPlacement(MeshHandle)>& placement,
                 std::uint32_t frameIndex);

    // The frame's table (after prepare returned true): its parts' draws, to pack their instances in its order.
    const PlacedTable& getTable(std::uint32_t frameIndex) const;

    // The camera's entries for the scene set (after prepare returned true).
    VkDescriptorBufferInfo describeEntries(std::uint32_t frameIndex) const;

    /*
    * Records the passes (after prepare returned true and the planting's passes): camera is the camera's culling view,
    * heights the terrain's heights buffer the planting reads. Then the entries readable by the vertex shaders and the
    * commands by the indirect draws; with a comparison asked, copied out with the slots.
    */
    void record(VkCommandBuffer commands, FrameDescriptors& descriptors, const GpuCameraCull& camera, const VulkanPlanting& planting,
                VkBuffer heights, std::uint32_t frameIndex);

    // The frame's commands and their order, style by style, for the passes that draw them.
    struct Draws {
        VkBuffer commands = VK_NULL_HANDLE;
        const CullDrawOrder* order = nullptr;
    };

    Draws draws(std::uint32_t frameIndex) const;

    // Asks for the next frame drawing placed plants to be read back and compared; returns the last comparison's report.
    std::string compare();

    // After a frame's fence: a comparison it was read back for, made (cullPlacedOnCpu over the slots it read back).
    void collect(std::uint32_t frameIndex);

private:
    struct Readback {
        VulkanBuffer entries;
        VulkanBuffer commands;
        VulkanBuffer plants;
        VulkanBuffer counts;
        PlacedTable table;
        std::vector<GpuPlacedBlock> blocks;
        GpuPlacedView view;
        std::shared_ptr<const PlantingField> field;
    };

    struct PerFrame {
        PlacedTable table;
        std::vector<GpuPlacedBlock> blocks;
        PlantedRegion region;
        std::shared_ptr<const PlantingField> field;

        // Upload memory: the parts, kinds, blocks, view and groups.
        VulkanBuffer parts;
        VulkanBuffer kinds;
        VulkanBuffer blockRecords;
        VulkanBuffer view;
        VulkanBuffer groups;

        // GPU memory: the commands and the entries.
        VulkanBuffer commands;
        VulkanBuffer entries;

        std::optional<Readback> readback;
    };

    const VulkanDevice& device;
    std::unique_ptr<ComputePipeline> countPass;
    std::unique_ptr<ComputePipeline> scanPass;
    std::unique_ptr<ComputePipeline> commandsPass;
    std::unique_ptr<ComputePipeline> writePass;
    std::vector<PerFrame> frames;
    VulkanBuffer blockCounts;
    VulkanBuffer totals;
    bool compareAsked = false;
    std::string report;
};
