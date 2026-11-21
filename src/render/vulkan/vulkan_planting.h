#pragma once

#include "core/job_system.h"
#include "math/vec3.h"
#include "render/gpu/planting_records.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "scene/planting_cells.h"
#include "scene/terrain_heights.h"
#include "volk/volk.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ImmediateCommands;
class VulkanDevice;

/*
* The planting cells on the GPU (ROADMAP phase 18, the valley's stages 2b1 and e1; docs/architecture/planting.md): the
* field's maps and each set of cells' layers uploaded when it changes, each set's cells round the camera made a few a
* frame into its fixed slots (every set's slots in one plants buffer and one counts buffer), three passes a layer
* (candidates, spacing, writing; the writing only for the layers a set keeps), and placement-compare reading the slots
* back against plantLayers.
*/
class VulkanPlanting {
public:
    VulkanPlanting(const VulkanDevice& device, ImmediateCommands& uploads, std::uint32_t framesInFlight);

    /*
    * The frame's cells: none without a field, a terrain or the setting (the cells forgotten); the field uploaded again
    * when it or a set's settings changed (every cell made anew); each set's cells round the camera updated and this
    * frame's jobs written. Returns whether the passes run.
    */
    bool prepare(const std::shared_ptr<const PlantingField>& field, std::uint64_t fieldVersion, const std::shared_ptr<const TerrainHeights>& heights,
                 const PlantingCellSettings& settings, const Vec3& camera, std::uint32_t frameIndex);

    // Records the passes (after prepare() returned true): heights is the terrain's heights buffer the scene set reads.
    void record(VkCommandBuffer commands, FrameDescriptors& descriptors, VkBuffer heights, std::uint32_t frameIndex);

    // Asks for the next frame the passes run in to be read back and compared; returns the last comparison's report.
    std::string compare();

    // After a frame's fence: a comparison it was read back for, made (the CPU planting the same cells).
    void collect(std::uint32_t frameIndex, JobSystem& jobs);

    // A new number of frames in flight: their jobs and read-backs made again with the field's next upload.
    void setFramesInFlight(std::uint32_t count);

    // The cells held and made this frame, every set's, for F4.
    std::size_t getHeldCells() const;
    std::size_t getMadeCells() const;

    // What the placed drawing reads (render/vulkan/vulkan_placed_drawing.h): the passes' eight bindings this frame (the
    // slots' plants and counts among them; the first set planting's layers and jobs), the slots' plants alone, each set
    // planting's slots (a view, while this frame's cells stand), the field (none while nothing is planted), the ground
    // the cells were planted on, and the cells a set made this frame (by its place among getSlots').
    std::array<ComputeBinding, PLANTING_BINDINGS> getBindings(VkBuffer heights, std::uint32_t frameIndex) const;
    VkBuffer getPlants() const;
    VkDeviceSize getPlantsBytes() const;
    VkDeviceSize getCountsBytes() const;
    std::vector<PlantingSetSlots> getSlots() const;
    const std::vector<PlantingCellMade>& getMade(std::size_t set) const;
    bool holdsCells() const;
    const std::shared_ptr<const PlantingField>& getField() const;
    const std::shared_ptr<const TerrainHeights>& getHeights() const;

private:
    // A set of cells planting at least one layer: its settings, its layers, slots and their places, its ring, the cells
    // it made this frame, its layers' records and a jobs buffer a frame in flight.
    struct CellSet {
        PlantingCellSetSettings settings;
        PlantingSetLayers layers;
        PlantingSlotLayout layout;
        PlantingSetPlaces places;
        int slots = 0;
        PlantingCellRing ring;
        std::vector<PlantingCellMade> made;
        VulkanBuffer layerRecords;
        std::vector<VulkanBuffer> jobs;
    };

    // One set's cells held when read back, and what the CPU needs to plant them.
    struct ReadbackSet {
        std::map<PlantingCell, int> held;
        PlantingSetLayers layers;
        PlantingSlotLayout layout;
        PlantingSetPlaces places;
        float cellMetres = 0.0f;
    };

    // A frame read back for a comparison: every set's cells held then.
    struct Readback {
        VulkanBuffer plants;
        VulkanBuffer counts;
        VulkanBuffer problems;
        std::vector<ReadbackSet> sets;
        std::shared_ptr<const PlantingField> field;
        std::shared_ptr<const TerrainHeights> heights;
    };

    const VulkanDevice& device;
    ImmediateCommands& uploads;
    std::unique_ptr<ComputePipeline> candidatesPass;
    std::unique_ptr<ComputePipeline> spacingPass;
    std::unique_ptr<ComputePipeline> writePass;

    const PlantingField* uploadedField = nullptr;
    std::uint64_t uploadedVersion = 0;
    std::array<PlantingCellSetSettings, plantingCellSetCount> uploadedSettings{};
    std::uint32_t framesInFlight = 0;
    std::shared_ptr<const PlantingField> field;
    std::shared_ptr<const TerrainHeights> heights;
    std::vector<CellSet> sets;

    VulkanBuffer maps;
    VulkanBuffer candidates;
    VulkanBuffer plants;
    VulkanBuffer counts;
    VulkanBuffer problems;
    std::vector<std::optional<Readback>> readbacks;

    bool compareAsked = false;
    std::string report;

    void upload(const PlantingCellSettings& settings);
    void forget();
    std::array<ComputeBinding, PLANTING_BINDINGS> bindingsOf(const CellSet& set, VkBuffer heights, std::uint32_t frameIndex) const;
};
