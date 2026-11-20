#include "render/vulkan/vulkan_planting.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/vulkan_device.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace {
    constexpr std::uint32_t pushBytes = 16;
    constexpr std::uint32_t bindingCount = PLANTING_BINDINGS;

    std::unique_ptr<ComputePipeline> plantingPipeline(const VulkanDevice& device, const char* shader) {
        ComputePipelineDescription description;
        description.shader = shader;
        description.storageBuffers = bindingCount;
        description.pushConstantBytes = pushBytes;
        return std::make_unique<ComputePipeline>(device, description);
    }

    VkDeviceSize atLeastAWord(VkDeviceSize bytes) {
        return std::max<VkDeviceSize>(bytes, 16);
    }

    // A buffer in GPU memory cleared to zero.
    VulkanBuffer clearedBuffer(const VulkanDevice& device, ImmediateCommands& uploads, VkDeviceSize bytes, VkBufferUsageFlags usage) {
        VulkanBuffer buffer{device.getAllocator(), atLeastAWord(bytes), usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Gpu};
        uploads.run([&](VkCommandBuffer commands) {
            vkCmdFillBuffer(commands, buffer.get(), 0, VK_WHOLE_SIZE, 0);
        });
        return buffer;
    }

    // Compute shaders reading and writing a buffer, or copying it out for a comparison.
    constexpr BufferUse computeUse{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    constexpr BufferUse copyRead{VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};

    // Every earlier use of the slots: the planting's and the placed drawing's passes, a comparison's copy, the vertex
    // shaders drawing placed plants (render/vulkan/vulkan_placed_drawing.h).
    constexpr BufferUse slotsUsed{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT};
}

VulkanPlanting::VulkanPlanting(const VulkanDevice& device, ImmediateCommands& uploads, std::uint32_t framesInFlight):
    device(device),
    uploads(uploads),
    candidatesPass(plantingPipeline(device, "assets/shaders/planting_candidates.comp.spv")),
    spacingPass(plantingPipeline(device, "assets/shaders/planting_spacing.comp.spv")),
    writePass(plantingPipeline(device, "assets/shaders/planting_write.comp.spv")),
    jobs(framesInFlight),
    readbacks(framesInFlight)
{
}

/*
* Everything sized by the field and the settings, made again together: the frames in flight may read the old buffers,
* so the GPU is waited for first. Every cell is made anew into the new slots.
*/
void VulkanPlanting::upload(const PlantingCellSettings& settings) {
    device.waitIdle();
    const std::vector<std::uint32_t> words = packPlantingMaps(field->maps);
    const std::vector<GpuPlantingLayer> records = packPlantingLayers(field->layers);
    layout = plantingSlotLayout(field->layers, settings.cellMetres);
    layerCount = static_cast<std::uint32_t>(field->layers.size());
    const int slots = plantingSlotCount(settings);
    const auto cellsAFrame = static_cast<VkDeviceSize>(std::max(settings.cellsAFrame, 1));

    maps = gpuBufferHolding(device, uploads, words.data(), words.size() * sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    layers = gpuBufferHolding(device, uploads, records.data(), atLeastAWord(records.size() * sizeof(GpuPlantingLayer)), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    candidates = clearedBuffer(device, uploads, cellsAFrame * layout.candidatesACell * sizeof(GpuPlantingCandidate), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const VkBufferUsageFlags readable = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    plants = clearedBuffer(device, uploads, static_cast<VkDeviceSize>(slots) * layout.plantsASlot * sizeof(GpuPlacedPlant), readable);
    counts = clearedBuffer(device, uploads, static_cast<VkDeviceSize>(slots) * layerCount * sizeof(std::uint32_t), readable);
    problems = clearedBuffer(device, uploads, PLANTING_PROBLEMS * sizeof(std::uint32_t), readable);

    for (VulkanBuffer& frameJobs : jobs) {
        frameJobs = VulkanBuffer{device.getAllocator(), atLeastAWord(cellsAFrame * layerCount * sizeof(GpuPlantingJob)), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 BufferMemory::Upload};
    }

    for (std::optional<Readback>& readback : readbacks) {
        readback.reset();
    }

    ring.reset();
    uploadedField = field.get();
    cellMetres = settings.cellMetres;
    uploadedCellMetres = settings.cellMetres;
    uploadedSlots = slots;
    uploadedCellsAFrame = settings.cellsAFrame;
}

void VulkanPlanting::forget() {
    ring.reset();
    made.clear();
    field.reset();
    heights.reset();
    uploadedField = nullptr;
}

bool VulkanPlanting::prepare(const std::shared_ptr<const PlantingField>& frameField, std::uint64_t fieldVersion,
                             const std::shared_ptr<const TerrainHeights>& frameHeights, const PlantingCellSettings& settings, const Vec3& camera,
                             std::uint32_t frameIndex) {
    made.clear();

    if (!settings.enabled || !frameField || !frameHeights || frameField->layers.empty()) {
        if (field) {
            forget();
        }

        return false;
    }

    field = frameField;
    heights = frameHeights;

    if (field.get() != uploadedField || fieldVersion != uploadedVersion || settings.cellMetres != uploadedCellMetres ||
        plantingSlotCount(settings) != uploadedSlots || settings.cellsAFrame != uploadedCellsAFrame) {
        upload(settings);
        uploadedVersion = fieldVersion;
    }

    made = ring.update(Vec2{camera.x, camera.z}, settings);

    if (made.empty() && !compareAsked) {
        return false;
    }

    const std::vector<GpuPlantingJob> frameJobs = packPlantingJobs(field->layers, layout, made, cellMetres);
    VulkanBuffer& buffer = jobs.at(frameIndex);

    if (!frameJobs.empty()) {
        std::memcpy(buffer.getMapped(), frameJobs.data(), frameJobs.size() * sizeof(GpuPlantingJob));
        buffer.flush();
    }

    return true;
}

/*
* A layer at a time: its candidates over each cell's candidates' lattice (a workgroup row a cell), then its spacing over
* the standing lattice, each waiting for the pass before (the next layer reads what this one kept). Then each layer's
* writing pass, a workgroup a cell. Asked for a comparison, the slots, counts and problems are copied out after.
*/
void VulkanPlanting::record(VkCommandBuffer commands, FrameDescriptors& descriptors, VkBuffer heightsBuffer, std::uint32_t frameIndex) {
    const std::array<ComputeBinding, bindingCount> bindings = getBindings(heightsBuffer, frameIndex);
    const auto cells = static_cast<std::uint32_t>(made.size());

    // Last frame's passes and copies are done with the candidates and the slots before this frame's write them.
    bufferBarrier(commands, candidates.get(), computeUse, computeUse);
    bufferBarrier(commands, plants.get(), slotsUsed, computeUse);
    bufferBarrier(commands, counts.get(), slotsUsed, computeUse);

    const auto dispatch = [&](const ComputePipeline& pipeline, std::uint32_t layer, std::uint32_t x, std::uint32_t y) {
        const std::array<std::uint32_t, 4> pushed{layer, layerCount, cells, 0};
        bindComputeSet(commands, device.getDevice(), descriptors, pipeline, bindings);
        vkCmdPushConstants(commands, pipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes, pushed.data());
        vkCmdDispatch(commands, x, y, 1);
    };

    if (cells > 0) {
        for (std::uint32_t layer = 0; layer < layerCount; ++layer) {
            const std::uint32_t groups = workgroupCount(layout.scratchCapacity[layer], PLANTING_GROUP_SIZE);
            dispatch(*candidatesPass, layer, groups, cells);
            bufferBarrier(commands, candidates.get(), computeUse, computeUse);
            dispatch(*spacingPass, layer, groups, cells);
            bufferBarrier(commands, candidates.get(), computeUse, computeUse);
        }

        for (std::uint32_t layer = 0; layer < layerCount; ++layer) {
            dispatch(*writePass, layer, cells, 1);
        }
    }

    readbacks.at(frameIndex).reset();

    if (!compareAsked) {
        return;
    }

    compareAsked = false;
    bufferBarrier(commands, plants.get(), computeUse, copyRead);
    bufferBarrier(commands, counts.get(), computeUse, copyRead);
    bufferBarrier(commands, problems.get(), computeUse, copyRead);
    Readback readback{
        .plants = VulkanBuffer{device.getAllocator(), plants.getSize(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback},
        .counts = VulkanBuffer{device.getAllocator(), counts.getSize(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback},
        .problems = VulkanBuffer{device.getAllocator(), problems.getSize(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback},
        .held = ring.getHeld(),
        .field = field,
        .heights = heights,
        .layout = layout,
        .cellMetres = cellMetres,
    };

    for (const auto& [from, to] : {std::pair{&plants, &readback.plants}, std::pair{&counts, &readback.counts}, std::pair{&problems, &readback.problems}}) {
        const VkBufferCopy copy{0, 0, from->getSize()};
        vkCmdCopyBuffer(commands, from->get(), to->get(), 1, &copy);
    }

    readbacks.at(frameIndex) = std::move(readback);
}

std::string VulkanPlanting::compare() {
    compareAsked = true;
    return report;
}

/*
* The CPU plants the box round every cell held once (a box alone is the whole), splits its plants by cell, and compares
* each cell's with what the GPU's slot holds.
*/
void VulkanPlanting::collect(std::uint32_t frameIndex, JobSystem& jobSystem) {
    std::optional<Readback>& readback = readbacks.at(frameIndex);

    if (!readback) {
        return;
    }

    readback->plants.invalidate();
    readback->counts.invalidate();
    readback->problems.invalidate();
    const auto* gpuPlants = static_cast<const GpuPlacedPlant*>(readback->plants.getMapped());
    const auto* gpuCounts = static_cast<const std::uint32_t*>(readback->counts.getMapped());
    const auto* gpuProblems = static_cast<const std::uint32_t*>(readback->problems.getMapped());
    const std::vector<PlantingLayerRecord>& fieldLayers = readback->field->layers;

    std::vector<PlantingCell> cells;

    for (const auto& [cell, slot] : readback->held) {
        cells.push_back(cell);
    }

    PlantingComparison comparison;
    std::copy(gpuProblems, gpuProblems + PLANTING_PROBLEMS, comparison.problems.begin());

    if (!cells.empty()) {
        const auto cpu = plantingByCell(plantLayers(fieldLayers, readback->field->maps, *readback->heights,
                                                    plantingCellsBox(cells, readback->cellMetres), jobSystem),
                                        readback->cellMetres);
        const std::vector<std::vector<PlantedPlant>> none(fieldLayers.size());

        for (const auto& [cell, slot] : readback->held) {
            std::vector<std::vector<GpuPlacedPlant>> gpu(fieldLayers.size());

            for (std::size_t layer = 0; layer < fieldLayers.size(); ++layer) {
                const std::size_t first = static_cast<std::size_t>(slot) * readback->layout.plantsASlot + readback->layout.layerFirst[layer];
                const std::uint32_t count = gpuCounts[static_cast<std::size_t>(slot) * fieldLayers.size() + layer];
                gpu[layer].assign(gpuPlants + first, gpuPlants + first + count);
            }

            const auto found = cpu.find(cell);
            comparePlantingCell(comparison, found == cpu.end() ? none : found->second, gpu);
        }
    }

    report = describePlantingComparison(comparison);
    readback.reset();
}

std::size_t VulkanPlanting::getHeldCells() const {
    return ring.getHeld().size();
}

std::size_t VulkanPlanting::getMadeCells() const {
    return made.size();
}

std::array<ComputeBinding, PLANTING_BINDINGS> VulkanPlanting::getBindings(VkBuffer heightsBuffer, std::uint32_t frameIndex) const {
    return {
        storageBufferBinding(maps.get()),        storageBufferBinding(layers.get()), storageBufferBinding(heightsBuffer),
        storageBufferBinding(jobs.at(frameIndex).get()), storageBufferBinding(candidates.get()), storageBufferBinding(plants.get()),
        storageBufferBinding(counts.get()),      storageBufferBinding(problems.get()),
    };
}

VkBuffer VulkanPlanting::getPlants() const {
    return plants.get();
}

VkDeviceSize VulkanPlanting::getPlantsBytes() const {
    return plants.getSize();
}

VkDeviceSize VulkanPlanting::getCountsBytes() const {
    return counts.getSize();
}

const std::map<PlantingCell, int>& VulkanPlanting::getHeld() const {
    return ring.getHeld();
}

const PlantingSlotLayout& VulkanPlanting::getLayout() const {
    return layout;
}

float VulkanPlanting::getCellMetres() const {
    return cellMetres;
}

const std::shared_ptr<const TerrainHeights>& VulkanPlanting::getHeights() const {
    return heights;
}

const std::vector<PlantingCellMade>& VulkanPlanting::getMade() const {
    return made;
}

const std::shared_ptr<const PlantingField>& VulkanPlanting::getField() const {
    return field;
}

void VulkanPlanting::setFramesInFlight(std::uint32_t count) {
    jobs.clear();
    jobs.resize(count);
    readbacks.clear();
    readbacks.resize(count);
    uploadedField = nullptr;
}
