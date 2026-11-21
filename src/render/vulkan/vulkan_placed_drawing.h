#pragma once

#include "render/gpu/placed_drawing.h"
#include "render/gpu/shadow_culling.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

class VulkanDevice;
class VulkanPlanting;

/*
* The placed plants drawn in every view (ROADMAP phase 18, the valley's stages 2b2 and 2c1; render/gpu/placed_drawing.h):
* each frame the table of the frame's planted parts, the blocks of the planting's held slots and each held cell's drawn
* layer's box made on the CPU; then the views (the camera, the probe faces and shadow tiles drawn),
* each taking the blocks of the boxes it may see; then four passes over every view's items (placed_cull_count,
* placed_cull_scan, placed_cull_commands, placed_cull_write) write each view's entries and one indirect command a group,
* which the passes drawing that view draw style by style (draws()). The cells made and dropped since the frame before,
* and a new table, are the shadow tiles' changes. The table, blocks, views and items are each frame in flight's own in
* upload memory; the entries and commands its own in GPU memory; the item counts and totals shared (each frame's passes
* finish with them before the next frame's start).
*/
class VulkanPlacedDrawing {
public:
    VulkanPlacedDrawing(const VulkanDevice& device, std::uint32_t framesInFlight);

    // Between frames only: anything not yet collected is dropped.
    void setFramesInFlight(std::uint32_t count);

    /*
    * The frame's table, blocks and boxes, after the planting's prepare: none (returns false) without planted parts, a
    * field or a held cell. placement says where a mesh's indices are. Then the shadow changes (getShadowChanges): every
    * tile when the table or field changed or the placed plants start or stop, else the cells made or dropped.
    * Throws std::invalid_argument for parts the passes cannot draw (buildPlacedTable).
    */
    bool prepare(const FrameDescription& frame, const VulkanPlanting& planting, const std::function<MeshPlacement(MeshHandle)>& placement,
                 std::uint32_t frameIndex);

    // What this frame's placed plants changed in the shadow tiles (after prepare, whatever it returned).
    const PlantedShadowChanges& getShadowChanges() const;

    /*
    * The frame's views (after prepare returned true, before the scene set is written, which points at the entries),
    * each named for placement-compare's report: their items and the entries' and commands' room.
    */
    void setViews(std::span<const GpuCameraCull> views, std::vector<std::string> names, std::uint32_t frameIndex);

    // The frame's table (after prepare returned true): its parts' draws, to pack their instances in its order.
    const PlacedTable& getTable(std::uint32_t frameIndex) const;

    // Every view's entries for the scene set (after setViews).
    VkDescriptorBufferInfo describeEntries(std::uint32_t frameIndex) const;

    /*
    * Records the passes (after setViews and the planting's passes): heights the terrain's heights buffer the planting
    * reads. Then the entries readable by the vertex shaders and the commands by the indirect draws; with a comparison
    * asked, copied out with the slots.
    */
    void record(VkCommandBuffer commands, FrameDescriptors& descriptors, const VulkanPlanting& planting, VkBuffer heights, std::uint32_t frameIndex);

    // A view's commands (from offset in the buffer) and their order, style by style, for the passes that draw it.
    struct Draws {
        VkBuffer commands = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        const CullDrawOrder* order = nullptr;
    };

    Draws draws(std::uint32_t frameIndex, std::uint32_t view) const;

    /*
    * What the camera's mesh cluster passes read of its placed plants (after setViews; render/vulkan/
    * vulkan_mesh_cluster_culling.h): the commands (the camera's first), groups, entries and parts, the drawing order,
    * and the bound on its pairs (placedMeshClusterPairBound).
    */
    struct ClusterInputs {
        VkBuffer commands = VK_NULL_HANDLE;
        VkBuffer groups = VK_NULL_HANDLE;
        VkBuffer entries = VK_NULL_HANDLE;
        VkBuffer parts = VK_NULL_HANDLE;
        const CullDrawOrder* order = nullptr;
        std::uint64_t pairBound = 0;
    };

    ClusterInputs clusterInputs(std::uint32_t frameIndex) const;

    // Asks for the next frame drawing placed plants to be read back and compared; returns the last comparison's report.
    std::string compare();

    // Asks for the next frame drawing placed plants to be read back (cluster-compare reads it; the comparison is made too).
    void askReadback();

    // A frame read back, after its fence and before collect: its table, every view's commands, the entries, the slots' plants.
    struct ReadBack {
        const PlacedTable* table = nullptr;
        std::span<const GpuDrawCommand> commands;
        std::span<const GpuPlacedEntry> entries;
        std::span<const GpuPlacedPlant> plants;
    };

    std::optional<ReadBack> readBack(std::uint32_t frameIndex);

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
        PlacedViews views;
        std::vector<std::string> names;
        std::shared_ptr<const PlantingField> field;
    };

    struct PerFrame {
        PlacedTable table;
        std::vector<GpuPlacedBlock> blocks;
        std::vector<PlacedBlockRun> runs;
        std::vector<GpuCullChunk> chunks;
        PlacedViews views;
        std::vector<std::string> names;
        std::shared_ptr<const PlantingField> field;

        // Upload memory: the parts, kinds, blocks, views, groups and items.
        VulkanBuffer parts;
        VulkanBuffer kinds;
        VulkanBuffer blockRecords;
        VulkanBuffer viewRecords;
        VulkanBuffer groups;
        VulkanBuffer items;

        // GPU memory: the commands and the entries.
        VulkanBuffer commands;
        VulkanBuffer entries;

        std::optional<Readback> readback;
    };

    // Each held cell's drawn layers' boxes (from its ground's range, kept while it is held) and the frame's changes.
    void findBoxes(PerFrame& frame, const VulkanPlanting& planting);

    const VulkanDevice& device;
    std::unique_ptr<ComputePipeline> countPass;
    std::unique_ptr<ComputePipeline> scanPass;
    std::unique_ptr<ComputePipeline> commandsPass;
    std::unique_ptr<ComputePipeline> writePass;
    std::vector<PerFrame> frames;
    VulkanBuffer itemCounts;
    VulkanBuffer totals;
    bool compareAsked = false;
    std::string report;

    // A held cell: its set's place among the planting's sets, and the cell.
    using HeldCell = std::pair<int, PlantingCell>;

    // The ground's range under each held cell (for the ground it was taken from), and each held cell's change record
    // with what it stood for when last drawn: the table's signature and field (0 and none while nothing is drawn).
    std::map<HeldCell, TerrainHeightRange> groundRanges;
    std::shared_ptr<const TerrainHeights> rangedGround;
    std::map<HeldCell, GpuCullRecord> drawnCells;
    std::uint64_t drawnSignature = 0;
    const PlantingField* drawnField = nullptr;
    PlantedShadowChanges shadowChanges;
};
