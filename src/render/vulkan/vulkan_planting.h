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
* The planting cells on the GPU (ROADMAP phase 18, the valley's stage 2b1; docs/architecture/planting.md): the field's
* maps and layers uploaded when it changes, the cells round the camera made a few a frame into fixed slots, three passes a
* layer (candidates, spacing, writing), and placement-compare reading the slots back against plantLayers. Nothing draws
* the plants yet (stage 2b2).
*/
class VulkanPlanting {
public:
    VulkanPlanting(const VulkanDevice& device, ImmediateCommands& uploads, std::uint32_t framesInFlight);

    /*
    * The frame's cells: none without a field, a terrain or the setting (the cells forgotten); the field uploaded again
    * when it or the cells' size changed (every cell made anew); the cells round the camera updated and this frame's jobs
    * written. Returns whether the passes run.
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

    // The cells held and made this frame, for F4.
    std::size_t getHeldCells() const;
    std::size_t getMadeCells() const;

    // What the placed drawing reads (render/vulkan/vulkan_placed_drawing.h): the passes' eight bindings this frame (the
    // slots' plants and counts among them), the slots' plants alone, the cells held, the slots' layout, the field (none
    // while nothing is planted).
    std::array<ComputeBinding, PLANTING_BINDINGS> getBindings(VkBuffer heights, std::uint32_t frameIndex) const;
    VkBuffer getPlants() const;
    VkDeviceSize getPlantsBytes() const;
    VkDeviceSize getCountsBytes() const;
    const std::map<PlantingCell, int>& getHeld() const;
    const PlantingSlotLayout& getLayout() const;
    const std::shared_ptr<const PlantingField>& getField() const;

private:
    // A frame read back for a comparison: the cells held then, and what the CPU needs to plant them.
    struct Readback {
        VulkanBuffer plants;
        VulkanBuffer counts;
        VulkanBuffer problems;
        std::map<PlantingCell, int> held;
        std::shared_ptr<const PlantingField> field;
        std::shared_ptr<const TerrainHeights> heights;
        PlantingSlotLayout layout;
        float cellMetres = 0.0f;
    };

    const VulkanDevice& device;
    ImmediateCommands& uploads;
    std::unique_ptr<ComputePipeline> candidatesPass;
    std::unique_ptr<ComputePipeline> spacingPass;
    std::unique_ptr<ComputePipeline> writePass;

    const PlantingField* uploadedField = nullptr;
    std::uint64_t uploadedVersion = 0;
    float uploadedCellMetres = 0.0f;
    int uploadedSlots = 0;
    int uploadedCellsAFrame = 0;
    std::shared_ptr<const PlantingField> field;
    std::shared_ptr<const TerrainHeights> heights;
    PlantingSlotLayout layout;
    std::uint32_t layerCount = 0;

    VulkanBuffer maps;
    VulkanBuffer layers;
    VulkanBuffer candidates;
    VulkanBuffer plants;
    VulkanBuffer counts;
    VulkanBuffer problems;
    std::vector<VulkanBuffer> jobs;
    std::vector<std::optional<Readback>> readbacks;

    PlantingCellRing ring;
    std::vector<PlantingCellMade> made;
    float cellMetres = 0.0f;
    bool compareAsked = false;
    std::string report;

    void upload(const PlantingCellSettings& settings);
    void forget();
};
