#include "render/vulkan/vulkan_placed_drawing.h"
#include "render/gpu/compute_dispatch.h"
#include "render/gpu/mesh_cluster_culling.h"
#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/vulkan_device.h"
#include "render/vulkan/vulkan_planting.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>
#include <utility>

namespace {
    constexpr std::uint32_t pushBytes = 16;
    constexpr VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    // A buffer every frame in flight uses (not one a frame), grown as ensureBuffer grows one, once the GPU is done with
    // the old one: a frame still in flight may be reading it. Growing doubles, so this waits only a few times a run.
    void ensureSharedBuffer(const VulkanDevice& device, VulkanBuffer& buffer, VkDeviceSize bytes, VkBufferUsageFlags usage, BufferMemory memory) {
        if (buffer.get() != VK_NULL_HANDLE && buffer.getSize() < std::max(bytes, smallestRecordBufferBytes)) {
            device.waitIdle();
        }

        ensureBuffer(device.getAllocator(), buffer, bytes, usage, memory);
    }

    // An upload buffer holding a span's elements (grown to fit), written and flushed.
    template<typename Element>
    void upload(const VulkanDevice& device, VulkanBuffer& buffer, std::span<const Element> elements) {
        ensureBuffer(device.getAllocator(), buffer, elements.size_bytes(), storage, BufferMemory::Upload);

        if (!elements.empty()) {
            std::memcpy(buffer.getMapped(), elements.data(), elements.size_bytes());
        }

        buffer.flush();
    }

    std::unique_ptr<ComputePipeline> passPipeline(const VulkanDevice& device, const char* shader) {
        ComputePipelineDescription description;
        description.shader = shader;
        description.storageBuffers = PLACED_BINDINGS;
        description.pushConstantBytes = pushBytes;
        return std::make_unique<ComputePipeline>(device, description);
    }

    constexpr BufferUse computeReadWrite{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    constexpr BufferUse computeRead{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    // What draws placed plants reads their entries and plants with: the vertex shaders, and where the GPU has them, the
    // camera's mesh cluster passes (compute) and mesh shaders (phase 21, 2f).
    BufferUse drawingRead(const VulkanDevice& device) {
        BufferUse use{VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};

        if (device.supportsMeshShaders()) {
            use.stages |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
        }

        return use;
    }
    constexpr BufferUse indirectRead{VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
    constexpr BufferUse transferRead{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
    constexpr BufferUse transferWrite{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    constexpr BufferUse hostRead{VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
}

VulkanPlacedDrawing::VulkanPlacedDrawing(const VulkanDevice& device, std::uint32_t framesInFlight):
    device(device),
    countPass(passPipeline(device, "assets/shaders/placed_cull_count.comp.spv")),
    scanPass(passPipeline(device, "assets/shaders/placed_cull_scan.comp.spv")),
    commandsPass(passPipeline(device, "assets/shaders/placed_cull_commands.comp.spv")),
    writePass(passPipeline(device, "assets/shaders/placed_cull_write.comp.spv"))
{
    setFramesInFlight(framesInFlight);
}

void VulkanPlacedDrawing::setFramesInFlight(std::uint32_t count) {
    frames.clear();
    frames.resize(count);
}

/*
* Nothing drawn this frame: every tile once if something was drawn the frame before (its shadows go), else nothing. The
* boxes after the table, and the entries and commands made with the views.
*/
bool VulkanPlacedDrawing::prepare(const FrameDescription& description, const VulkanPlanting& planting,
                                  const std::function<MeshPlacement(MeshHandle)>& placement, std::uint32_t frameIndex) {
    PerFrame& frame = frames.at(frameIndex);
    frame.blocks.clear();
    frame.runs.clear();
    frame.chunks.clear();
    frame.views = PlacedViews{};
    frame.field = planting.getField();
    shadowChanges = PlantedShadowChanges{};
    bool draws = !description.plantedParts.empty() && frame.field && planting.holdsCells();

    if (draws) {
        frame.table = buildPlacedTable(description.plantedParts, description.plantedKinds, frame.field->layers.size(), placement,
                                       description.plantedLayers);
        frame.blocks = placedBlocks(planting.getSlots(), frame.table);
        draws = !frame.blocks.empty() && !frame.table.groups.empty();
    }

    if (!draws) {
        shadowChanges.all = drawnSignature != 0;
        drawnCells.clear();
        drawnSignature = 0;
        drawnField = nullptr;
        return false;
    }

    findBoxes(frame, planting);
    return true;
}

/*
* A cell's ground range taken once while it is held (its ground's points: grows with the cells made). Its change record
* stands for every drawn layer's box. Made this frame (the planting's own list, so a cell made again in the same place
* counts) or dropped since the frame before (its old record): those tiles redraw; a new table or field: all.
*/
void VulkanPlacedDrawing::findBoxes(PerFrame& frame, const VulkanPlanting& planting) {
    const std::shared_ptr<const TerrainHeights>& ground = planting.getHeights();
    const std::vector<PlantingSetSlots> sets = planting.getSlots();

    if (ground != rangedGround) {
        groundRanges.clear();
        rangedGround = ground;
    }

    std::map<HeldCell, TerrainHeightRange> ranges;

    for (std::size_t set = 0; set < sets.size(); ++set) {
        for (const auto& [cell, slot] : sets[set].held) {
            const HeldCell held{static_cast<int>(set), cell};
            const auto found = groundRanges.find(held);

            if (found != groundRanges.end()) {
                ranges.insert(*found);
                continue;
            }

            const PlantingBox box = plantingCellBox(cell, sets[set].cellMetres);
            ranges.emplace(held, ground ? terrainHeightRange(*ground, box.west, box.east, box.north, box.south) : TerrainHeightRange{});
        }
    }

    groundRanges = std::move(ranges);
    frame.runs = placedBlockRuns(sets, frame.table);

    for (const PlacedBlockRun& run : frame.runs) {
        const PlantingBox box = plantingCellBox(run.cell, sets.at(static_cast<std::size_t>(run.set)).cellMetres);
        frame.chunks.push_back(placedCellChunk(box, groundRanges.at(HeldCell{run.set, run.cell}), frame.field->layers.at(run.layer),
                                               frame.table.layerReach.at(run.layer)));
    }

    // Each cell's runs follow one another (set by set, the held maps' order), so its boxes are one span.
    std::map<HeldCell, GpuCullRecord> cells;

    for (std::size_t first = 0; first < frame.runs.size();) {
        std::size_t past = first;

        while (past < frame.runs.size() && frame.runs[past].set == frame.runs[first].set && frame.runs[past].cell == frame.runs[first].cell) {
            ++past;
        }

        cells.emplace(HeldCell{frame.runs[first].set, frame.runs[first].cell},
                      placedChangeRecord(std::span<const GpuCullChunk>{frame.chunks}.subspan(first, past - first)));
        first = past;
    }

    const std::uint64_t signature = placedTableSignature(frame.table);

    if (signature != drawnSignature || frame.field.get() != drawnField) {
        shadowChanges.all = true;
    } else {
        for (std::size_t set = 0; set < sets.size(); ++set) {
            for (const PlantingCellMade& made : planting.getMade(set)) {
                if (const auto found = cells.find(HeldCell{static_cast<int>(set), made.cell}); found != cells.end()) {
                    shadowChanges.cells.push_back(found->second);
                }
            }
        }

        for (const auto& [cell, record] : drawnCells) {
            if (!cells.contains(cell)) {
                shadowChanges.cells.push_back(record);
            }
        }
    }

    drawnCells = std::move(cells);
    drawnSignature = signature;
    drawnField = frame.field.get();
}

const PlantedShadowChanges& VulkanPlacedDrawing::getShadowChanges() const {
    return shadowChanges;
}

void VulkanPlacedDrawing::setViews(std::span<const GpuCameraCull> views, std::vector<std::string> names, std::uint32_t frameIndex) {
    PerFrame& frame = frames.at(frameIndex);
    frame.views = placedViews(views, frame.runs, frame.chunks, frame.table);
    frame.names = std::move(names);
    const VkDeviceSize entryBytes = static_cast<VkDeviceSize>(frame.views.entries) * sizeof(GpuPlacedEntry);
    ensureBuffer(device.getAllocator(), frame.entries, entryBytes, storage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Gpu);
    ensureBuffer(device.getAllocator(), frame.commands, views.size() * frame.table.groups.size() * sizeof(GpuDrawCommand),
                 storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Gpu);
}

const PlacedTable& VulkanPlacedDrawing::getTable(std::uint32_t frameIndex) const {
    return frames.at(frameIndex).table;
}

VkDescriptorBufferInfo VulkanPlacedDrawing::describeEntries(std::uint32_t frameIndex) const {
    return VkDescriptorBufferInfo{frames.at(frameIndex).entries.get(), 0, VK_WHOLE_SIZE};
}

/*
* The table, blocks, views and items written whole into the frame in flight's upload buffers. Each pass waits for the
* one before: the counts are summed by view and group, the sums give each view's commands, and the writing pass reads
* both. Items are the counting and writing passes' workgroups, in rows of PLACED_ITEM_ROW (every device takes 65,535 a
* dimension); the scan's are (group, view), the commands' a view each.
*/
void VulkanPlacedDrawing::record(VkCommandBuffer commands, FrameDescriptors& descriptors, const VulkanPlanting& planting, VkBuffer heights,
                                 std::uint32_t frameIndex) {
    PerFrame& frame = frames.at(frameIndex);
    const PlacedTable& table = frame.table;
    upload(device, frame.parts, std::span<const GpuPlacedPart>{table.parts});
    upload(device, frame.kinds, std::span<const GpuPlacedKind>{table.kinds});
    upload(device, frame.blockRecords, std::span<const GpuPlacedBlock>{frame.blocks});
    upload(device, frame.viewRecords, std::span<const GpuPlacedView>{frame.views.views});
    upload(device, frame.groups, std::span<const GpuPlacedGroup>{table.groups});
    upload(device, frame.items, std::span<const GpuPlacedItem>{frame.views.items});
    const auto items = static_cast<std::uint32_t>(frame.views.items.size());
    const auto views = static_cast<std::uint32_t>(frame.views.views.size());
    const auto groups = static_cast<std::uint32_t>(table.groups.size());
    ensureSharedBuffer(device, itemCounts, static_cast<VkDeviceSize>(items) * groups * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    ensureSharedBuffer(device, totals, 2 * static_cast<VkDeviceSize>(views) * groups * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    const std::array<ComputeBinding, PLANTING_BINDINGS> plantingBindings = planting.getBindings(heights, frameIndex);
    const VkBuffer plants = plantingBindings[PLANTING_PLANTS_BINDING].buffer.buffer;
    const VkBuffer counts = plantingBindings[PLANTING_COUNTS_BINDING].buffer.buffer;

    // Last frame's passes are done with the shared counts and totals, and this frame's planting with the slots, which
    // these passes and the vertex shaders read.
    bufferBarrier(commands, itemCounts.get(), computeReadWrite, computeReadWrite);
    bufferBarrier(commands, totals.get(), computeReadWrite, computeReadWrite);
    const BufferUse drawnRead = drawingRead(device);
    bufferBarrier(commands, plants, computeReadWrite, drawnRead);
    bufferBarrier(commands, counts, computeReadWrite, computeRead);

    std::vector<ComputeBinding> bindings(plantingBindings.begin(), plantingBindings.end());

    for (const VkBuffer buffer : {frame.parts.get(), frame.kinds.get(), frame.blockRecords.get(), frame.viewRecords.get(), frame.groups.get(),
                                  itemCounts.get(), totals.get(), frame.commands.get(), frame.entries.get(), frame.items.get()}) {
        bindings.push_back(storageBufferBinding(buffer));
    }

    // planting_cells.glsl's push constants, which these passes never read.
    const auto dispatch = [&](const ComputePipeline& pipeline, std::uint32_t x, std::uint32_t y) {
        const std::array<std::uint32_t, 4> pushed{0, 0, 0, 0};
        bindComputeSet(commands, device.getDevice(), descriptors, pipeline, bindings);
        vkCmdPushConstants(commands, pipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes, pushed.data());
        vkCmdDispatch(commands, x, y, 1);
    };
    const std::uint32_t itemColumns = std::min(items, placedItemRow);
    const std::uint32_t itemRows = (items + placedItemRow - 1) / placedItemRow;

    if (items > 0) {
        dispatch(*countPass, itemColumns, itemRows);
    }

    bufferBarrier(commands, itemCounts.get(), computeReadWrite, computeReadWrite);
    dispatch(*scanPass, groups, views);
    bufferBarrier(commands, itemCounts.get(), computeReadWrite, computeReadWrite);
    bufferBarrier(commands, totals.get(), computeReadWrite, computeReadWrite);
    dispatch(*commandsPass, 1, views);
    bufferBarrier(commands, totals.get(), computeReadWrite, computeReadWrite);

    if (items > 0) {
        dispatch(*writePass, itemColumns, itemRows);
    }

    // Then the vertex shaders (and the camera's mesh cluster passes and mesh shaders) read the entries, the indirect draws
    // and the cluster passes the commands (and, for a comparison, copies).
    const bool reading = compareAsked;
    const BufferUse entriesThen{drawnRead.stages | (reading ? transferRead.stages : 0), drawnRead.access | (reading ? transferRead.access : 0)};
    const BufferUse commandsThen{indirectRead.stages | computeRead.stages | (reading ? transferRead.stages : 0),
                                 indirectRead.access | computeRead.access | (reading ? transferRead.access : 0)};
    bufferBarrier(commands, frame.entries.get(), computeReadWrite, entriesThen);
    bufferBarrier(commands, frame.commands.get(), computeReadWrite, commandsThen);
    frame.readback.reset();

    if (!reading) {
        return;
    }

    // A comparison: the entries, the commands and the slots as the passes read them.
    compareAsked = false;
    bufferBarrier(commands, plants, computeRead, transferRead);
    bufferBarrier(commands, counts, computeRead, transferRead);
    const auto readable = [&](VkDeviceSize bytes) {
        return VulkanBuffer{device.getAllocator(), bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback};
    };
    Readback readback{
        .entries = readable(frame.entries.getSize()),
        .commands = readable(frame.commands.getSize()),
        .plants = readable(planting.getPlantsBytes()),
        .counts = readable(planting.getCountsBytes()),
        .table = table,
        .blocks = frame.blocks,
        .views = frame.views,
        .names = frame.names,
        .field = frame.field,
    };

    for (const auto& [from, to] : {std::pair{frame.entries.get(), &readback.entries}, std::pair{frame.commands.get(), &readback.commands},
                                   std::pair{plants, &readback.plants}, std::pair{counts, &readback.counts}}) {
        const VkBufferCopy copy{0, 0, to->getSize()};
        vkCmdCopyBuffer(commands, from, to->get(), 1, &copy);
        bufferBarrier(commands, to->get(), transferWrite, hostRead);
    }

    // The slots go back to being read by the vertex shaders, and written by the next frame's planting.
    bufferBarrier(commands, plants, transferRead, drawnRead);
    frame.readback = std::move(readback);
}

VulkanPlacedDrawing::Draws VulkanPlacedDrawing::draws(std::uint32_t frameIndex, std::uint32_t view) const {
    const PerFrame& frame = frames.at(frameIndex);
    return Draws{frame.commands.get(), static_cast<VkDeviceSize>(view) * frame.table.groups.size() * sizeof(GpuDrawCommand), &frame.table.order};
}

VulkanPlacedDrawing::ClusterInputs VulkanPlacedDrawing::clusterInputs(std::uint32_t frameIndex) const {
    const PerFrame& frame = frames.at(frameIndex);
    ClusterInputs inputs{frame.commands.get(), frame.groups.get(), frame.entries.get(), frame.parts.get(), &frame.table.order, 0};

    if (!frame.views.views.empty()) {
        inputs.pairBound = placedMeshClusterPairBound(frame.table, frame.blocks, frame.views.items, frame.views.views.front());
    }

    return inputs;
}

std::string VulkanPlacedDrawing::compare() {
    askReadback();
    return report;
}

void VulkanPlacedDrawing::askReadback() {
    compareAsked = true;
}

std::optional<VulkanPlacedDrawing::ReadBack> VulkanPlacedDrawing::readBack(std::uint32_t frameIndex) {
    std::optional<Readback>& readback = frames.at(frameIndex).readback;

    if (!readback) {
        return std::nullopt;
    }

    for (const VulkanBuffer* buffer : {&readback->entries, &readback->commands, &readback->plants}) {
        buffer->invalidate();
    }

    const std::size_t commands = readback->views.views.size() * readback->table.groups.size();
    return ReadBack{&readback->table, {static_cast<const GpuDrawCommand*>(readback->commands.getMapped()), commands},
                    {static_cast<const GpuPlacedEntry*>(readback->entries.getMapped()), readback->entries.getSize() / sizeof(GpuPlacedEntry)},
                    {static_cast<const GpuPlacedPlant*>(readback->plants.getMapped()), readback->plants.getSize() / sizeof(GpuPlacedPlant)}};
}

// A line a view: its name, then its comparison.
void VulkanPlacedDrawing::collect(std::uint32_t frameIndex) {
    std::optional<Readback>& readback = frames.at(frameIndex).readback;

    if (!readback) {
        return;
    }

    for (const VulkanBuffer* buffer : {&readback->entries, &readback->commands, &readback->plants, &readback->counts}) {
        buffer->invalidate();
    }

    const std::span<const GpuPlacedPlant> plants{static_cast<const GpuPlacedPlant*>(readback->plants.getMapped()),
                                                 readback->plants.getSize() / sizeof(GpuPlacedPlant)};
    const std::span<const std::uint32_t> counts{static_cast<const std::uint32_t*>(readback->counts.getMapped()),
                                                readback->counts.getSize() / sizeof(std::uint32_t)};
    const std::size_t groups = readback->table.groups.size();
    const std::span<const GpuDrawCommand> commands{static_cast<const GpuDrawCommand*>(readback->commands.getMapped()), readback->views.views.size() * groups};
    const std::span<const GpuPlacedEntry> entries{static_cast<const GpuPlacedEntry*>(readback->entries.getMapped()),
                                                  readback->entries.getSize() / sizeof(GpuPlacedEntry)};
    std::ostringstream text;

    for (std::size_t view = 0; view < readback->views.views.size(); ++view) {
        const GpuPlacedView& placed = readback->views.views[view];
        const PlacedCullResult cpu = cullPlacedOnCpu(readback->table, readback->blocks, readback->views.items, plants, counts, placed);
        const PlacedComparison comparison = comparePlacedDraws(readback->table, plants, placed, cpu, commands.subspan(view * groups, groups), entries);
        text << (view > 0 ? "\n" : "") << (view < readback->names.size() ? readback->names[view] : "View") << " (" << placed.items << " blocks): "
             << describePlacedComparison(comparison);
    }

    report = text.str();
    readback.reset();
}
