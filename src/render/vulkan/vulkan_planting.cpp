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
    // shaders drawing placed plants (render/vulkan/vulkan_placed_drawing.h) and, where the GPU has them, the mesh shaders
    // drawing their mesh clusters.
    BufferUse slotsUsed(const VulkanDevice& device) {
        BufferUse use{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT};

        if (device.supportsMeshShaders()) {
            use.stages |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
        }

        return use;
    }
}

VulkanPlanting::VulkanPlanting(const VulkanDevice& device, ImmediateCommands& uploads, std::uint32_t framesInFlight):
    device(device),
    uploads(uploads),
    candidatesPass(plantingPipeline(device, "assets/shaders/planting_candidates.comp.spv")),
    spacingPass(plantingPipeline(device, "assets/shaders/planting_spacing.comp.spv")),
    writePass(plantingPipeline(device, "assets/shaders/planting_write.comp.spv")),
    framesInFlight(framesInFlight),
    readbacks(framesInFlight)
{
}

/*
* Everything sized by the field and the settings, made again together: the frames in flight may read the old buffers,
* so the GPU is waited for first. Each set planting a layer gets its layers, layout and slots, its slots after the set
* before's in the shared plants and counts; the candidates' scratch is the largest set's (the sets' passes run one set
* after another). Every cell is made anew into the new slots.
*/
void VulkanPlanting::upload(const PlantingCellSettings& settings) {
    device.waitIdle();
    const std::vector<std::uint32_t> words = packPlantingMaps(field->maps);
    sets.clear();
    std::uint32_t plantTotal = 0;
    std::uint32_t countTotal = 0;
    VkDeviceSize candidateCount = 0;

    for (int which = 0; which < plantingCellSetCount; ++which) {
        PlantingSetLayers layers = plantingSetLayers(field->layers, which);

        if (std::find(layers.written.begin(), layers.written.end(), true) == layers.written.end()) {
            continue;
        }

        CellSet& set = sets.emplace_back();
        set.settings = settings.sets[static_cast<std::size_t>(which)];
        set.layout = plantingSlotLayout(layers.layers, set.settings.cellMetres, layers.written);
        set.layers = std::move(layers);
        set.slots = plantingSlotCount(set.settings);
        set.places = PlantingSetPlaces{plantTotal, countTotal};
        plantTotal += static_cast<std::uint32_t>(set.slots) * set.layout.plantsASlot;
        countTotal += static_cast<std::uint32_t>(set.slots) * static_cast<std::uint32_t>(set.layers.layers.size());
        const auto cellsAFrame = static_cast<VkDeviceSize>(std::max(set.settings.cellsAFrame, 1));
        candidateCount = std::max(candidateCount, cellsAFrame * set.layout.candidatesACell);

        const std::vector<GpuPlantingLayer> records = packPlantingLayers(set.layers.layers);
        set.layerRecords = gpuBufferHolding(device, uploads, records.data(), atLeastAWord(records.size() * sizeof(GpuPlantingLayer)),
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        const VkDeviceSize jobBytes = atLeastAWord(cellsAFrame * set.layers.layers.size() * sizeof(GpuPlantingJob));

        for (std::uint32_t frame = 0; frame < framesInFlight; ++frame) {
            set.jobs.emplace_back(device.getAllocator(), jobBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload);
        }
    }

    maps = gpuBufferHolding(device, uploads, words.data(), words.size() * sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    candidates = clearedBuffer(device, uploads, candidateCount * sizeof(GpuPlantingCandidate), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const VkBufferUsageFlags readable = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    plants = clearedBuffer(device, uploads, static_cast<VkDeviceSize>(plantTotal) * sizeof(GpuPlacedPlant), readable);
    counts = clearedBuffer(device, uploads, static_cast<VkDeviceSize>(countTotal) * sizeof(std::uint32_t), readable);
    problems = clearedBuffer(device, uploads, PLANTING_PROBLEMS * sizeof(std::uint32_t), readable);

    for (std::optional<Readback>& readback : readbacks) {
        readback.reset();
    }

    uploadedField = field.get();
    uploadedSettings = settings.sets;
}

void VulkanPlanting::forget() {
    sets.clear();
    field.reset();
    heights.reset();
    uploadedField = nullptr;
}

bool VulkanPlanting::prepare(const std::shared_ptr<const PlantingField>& frameField, std::uint64_t fieldVersion,
                             const std::shared_ptr<const TerrainHeights>& frameHeights, const PlantingCellSettings& settings, const Vec3& camera,
                             std::uint32_t frameIndex) {
    for (CellSet& set : sets) {
        set.made.clear();
    }

    if (!settings.enabled || !frameField || !frameHeights || frameField->layers.empty()) {
        if (field) {
            forget();
        }

        return false;
    }

    field = frameField;
    heights = frameHeights;

    if (field.get() != uploadedField || fieldVersion != uploadedVersion || settings.sets != uploadedSettings) {
        upload(settings);
        uploadedVersion = fieldVersion;
    }

    bool makes = false;

    for (CellSet& set : sets) {
        set.made = set.ring.update(Vec2{camera.x, camera.z}, set.settings);
        makes = makes || !set.made.empty();
        const std::vector<GpuPlantingJob> frameJobs = packPlantingJobs(set.layers.layers, set.layout, set.made, set.settings.cellMetres, set.places);
        VulkanBuffer& buffer = set.jobs.at(frameIndex);

        if (!frameJobs.empty()) {
            std::memcpy(buffer.getMapped(), frameJobs.data(), frameJobs.size() * sizeof(GpuPlantingJob));
            buffer.flush();
        }
    }

    return makes || compareAsked;
}

/*
* Set by set, a layer at a time: its candidates over each cell's candidates' lattice (a workgroup row a cell), then its
* spacing over the standing lattice, each waiting for the pass before (the next layer reads what this one kept). Then
* each written layer's writing pass, a workgroup a cell; the next set waits for them before using the candidates again.
* Asked for a comparison, the slots, counts and problems are copied out after.
*/
void VulkanPlanting::record(VkCommandBuffer commands, FrameDescriptors& descriptors, VkBuffer heightsBuffer, std::uint32_t frameIndex) {
    // Last frame's passes and copies are done with the candidates and the slots before this frame's write them.
    bufferBarrier(commands, candidates.get(), computeUse, computeUse);
    bufferBarrier(commands, plants.get(), slotsUsed(device), computeUse);
    bufferBarrier(commands, counts.get(), slotsUsed(device), computeUse);

    for (const CellSet& set : sets) {
        const auto cells = static_cast<std::uint32_t>(set.made.size());

        if (cells == 0) {
            continue;
        }

        const std::array<ComputeBinding, bindingCount> bindings = bindingsOf(set, heightsBuffer, frameIndex);
        const auto layerCount = static_cast<std::uint32_t>(set.layers.layers.size());
        const auto dispatch = [&](const ComputePipeline& pipeline, std::uint32_t layer, std::uint32_t x, std::uint32_t y) {
            const std::array<std::uint32_t, 4> pushed{layer, layerCount, cells, 0};
            bindComputeSet(commands, device.getDevice(), descriptors, pipeline, bindings);
            vkCmdPushConstants(commands, pipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes, pushed.data());
            vkCmdDispatch(commands, x, y, 1);
        };

        for (std::uint32_t layer = 0; layer < layerCount; ++layer) {
            const std::uint32_t groups = workgroupCount(set.layout.scratchCapacity[layer], PLANTING_GROUP_SIZE);
            dispatch(*candidatesPass, layer, groups, cells);
            bufferBarrier(commands, candidates.get(), computeUse, computeUse);
            dispatch(*spacingPass, layer, groups, cells);
            bufferBarrier(commands, candidates.get(), computeUse, computeUse);
        }

        for (std::uint32_t layer = 0; layer < layerCount; ++layer) {
            if (set.layout.written[layer]) {
                dispatch(*writePass, layer, cells, 1);
            }
        }

        bufferBarrier(commands, candidates.get(), computeUse, computeUse);
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
        .sets = {},
        .field = field,
        .heights = heights,
    };

    for (const CellSet& set : sets) {
        readback.sets.push_back(ReadbackSet{set.ring.getHeld(), set.layers, set.layout, set.places, set.settings.cellMetres});
    }

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
* For each set, the CPU plants its layers over the box round every cell it holds once (a box alone is the whole),
* splits its plants by cell, and compares each cell's written layers with what the GPU's slot holds, each in the field's
* place for its layer (so every set's add up by layer).
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
    const std::size_t fieldLayers = readback->field->layers.size();

    PlantingComparison comparison;
    std::copy(gpuProblems, gpuProblems + PLANTING_PROBLEMS, comparison.problems.begin());

    for (const ReadbackSet& set : readback->sets) {
        std::vector<PlantingCell> cells;

        for (const auto& [cell, slot] : set.held) {
            cells.push_back(cell);
        }

        if (cells.empty()) {
            continue;
        }

        const std::vector<PlantingLayerRecord>& layers = set.layers.layers;
        const auto cpu = plantingByCell(plantLayers(layers, readback->field->maps, *readback->heights, plantingCellsBox(cells, set.cellMetres), jobSystem),
                                        set.cellMetres);

        for (const auto& [cell, slot] : set.held) {
            std::vector<std::vector<PlantedPlant>> cpuCell(fieldLayers);
            std::vector<std::vector<GpuPlacedPlant>> gpuCell(fieldLayers);
            const auto found = cpu.find(cell);

            for (std::size_t layer = 0; layer < layers.size(); ++layer) {
                if (!set.layout.written[layer]) {
                    continue;
                }

                const auto into = static_cast<std::size_t>(set.layers.fieldLayer[layer]);
                const std::size_t first = set.places.plantBase + static_cast<std::size_t>(slot) * set.layout.plantsASlot + set.layout.layerFirst[layer];
                const std::uint32_t count = gpuCounts[set.places.countBase + static_cast<std::size_t>(slot) * layers.size() + layer];
                gpuCell[into].assign(gpuPlants + first, gpuPlants + first + count);

                if (found != cpu.end()) {
                    cpuCell[into] = found->second[layer];
                }
            }

            comparePlantingCell(comparison, cpuCell, gpuCell);
        }
    }

    report = describePlantingComparison(comparison);
    readback.reset();
}

std::size_t VulkanPlanting::getHeldCells() const {
    std::size_t held = 0;

    for (const CellSet& set : sets) {
        held += set.ring.getHeld().size();
    }

    return held;
}

std::size_t VulkanPlanting::getMadeCells() const {
    std::size_t made = 0;

    for (const CellSet& set : sets) {
        made += set.made.size();
    }

    return made;
}

std::array<ComputeBinding, PLANTING_BINDINGS> VulkanPlanting::bindingsOf(const CellSet& set, VkBuffer heightsBuffer, std::uint32_t frameIndex) const {
    return {
        storageBufferBinding(maps.get()),        storageBufferBinding(set.layerRecords.get()), storageBufferBinding(heightsBuffer),
        storageBufferBinding(set.jobs.at(frameIndex).get()), storageBufferBinding(candidates.get()), storageBufferBinding(plants.get()),
        storageBufferBinding(counts.get()),      storageBufferBinding(problems.get()),
    };
}

std::array<ComputeBinding, PLANTING_BINDINGS> VulkanPlanting::getBindings(VkBuffer heightsBuffer, std::uint32_t frameIndex) const {
    return bindingsOf(sets.at(0), heightsBuffer, frameIndex);
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

std::vector<PlantingSetSlots> VulkanPlanting::getSlots() const {
    std::vector<PlantingSetSlots> slots;

    for (const CellSet& set : sets) {
        slots.push_back(PlantingSetSlots{set.ring.getHeld(), set.layout, set.layers.fieldLayer, set.places, set.settings.cellMetres,
                                         static_cast<std::uint32_t>(plantingSlotSide(set.settings))});
    }

    return slots;
}

const std::vector<PlantingCellMade>& VulkanPlanting::getMade(std::size_t set) const {
    return sets.at(set).made;
}

bool VulkanPlanting::holdsCells() const {
    return getHeldCells() > 0;
}

const std::shared_ptr<const TerrainHeights>& VulkanPlanting::getHeights() const {
    return heights;
}

const std::shared_ptr<const PlantingField>& VulkanPlanting::getField() const {
    return field;
}

void VulkanPlanting::setFramesInFlight(std::uint32_t count) {
    framesInFlight = count;
    readbacks.clear();
    readbacks.resize(count);
    uploadedField = nullptr;
}
