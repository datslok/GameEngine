#include "render/vulkan/vulkan_placed_drawing.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/vulkan_device.h"
#include "render/vulkan/vulkan_planting.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace {
    // Room for a few records of any kind before the first growth: no buffer is ever empty.
    constexpr VkDeviceSize smallestBytes = 256;
    constexpr std::uint32_t pushBytes = 16;
    constexpr VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    // Grown (to twice its size, or what is asked) when too small.
    void ensureBuffer(const VulkanDevice& device, VulkanBuffer& buffer, VkDeviceSize bytes, VkBufferUsageFlags usage, BufferMemory memory) {
        bytes = std::max(bytes, smallestBytes);

        if (buffer.getSize() < bytes) {
            buffer = VulkanBuffer{device.getAllocator(), std::max(bytes, 2 * buffer.getSize()), usage, memory};
        }
    }

    // An upload buffer holding a span's elements (grown to fit), written and flushed.
    template<typename Element>
    void upload(const VulkanDevice& device, VulkanBuffer& buffer, std::span<const Element> elements) {
        ensureBuffer(device, buffer, elements.size_bytes(), storage, BufferMemory::Upload);

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
    constexpr BufferUse vertexRead{VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
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

bool VulkanPlacedDrawing::prepare(const FrameDescription& description, const VulkanPlanting& planting,
                                  const std::function<MeshPlacement(MeshHandle)>& placement, std::uint32_t frameIndex) {
    PerFrame& frame = frames.at(frameIndex);
    frame.blocks.clear();
    frame.field = planting.getField();

    if (description.plantedParts.empty() || !frame.field || planting.getHeld().empty()) {
        return false;
    }

    frame.table = buildPlacedTable(description.plantedParts, description.plantedKinds, frame.field->layers.size(), placement);
    frame.blocks = placedBlocks(planting.getHeld(), planting.getLayout(), frame.table);
    frame.region = description.plantedRegion;

    if (frame.blocks.empty() || frame.table.groups.empty()) {
        return false;
    }

    // The entries and commands made now, so the scene set (written before the passes are recorded) can point at them.
    const VkDeviceSize entryBytes = static_cast<VkDeviceSize>(placedEntryRoom(frame.blocks.size(), frame.table)) * sizeof(GpuPlacedEntry);
    ensureBuffer(device, frame.entries, entryBytes, storage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Gpu);
    ensureBuffer(device, frame.commands, frame.table.groups.size() * sizeof(GpuDrawCommand),
                 storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Gpu);
    return true;
}

const PlacedTable& VulkanPlacedDrawing::getTable(std::uint32_t frameIndex) const {
    return frames.at(frameIndex).table;
}

VkDescriptorBufferInfo VulkanPlacedDrawing::describeEntries(std::uint32_t frameIndex) const {
    return VkDescriptorBufferInfo{frames.at(frameIndex).entries.get(), 0, VK_WHOLE_SIZE};
}

/*
* The table, blocks and view written whole into the frame in flight's upload buffers. Each pass waits for the one
* before: the counts are summed by group, the sums give the commands, and the writing pass reads both. Blocks are a
* dispatch's first dimension (at least 65,535 workgroups on every device: a 300 m radius takes about 2,000).
*/
void VulkanPlacedDrawing::record(VkCommandBuffer commands, FrameDescriptors& descriptors, const GpuCameraCull& camera, const VulkanPlanting& planting,
                                 VkBuffer heights, std::uint32_t frameIndex) {
    PerFrame& frame = frames.at(frameIndex);
    const PlacedTable& table = frame.table;
    const GpuPlacedView view = packPlacedView(camera, frame.region, frame.blocks.size(), table);
    const std::array<GpuPlacedView, 1> views{view};
    upload(device, frame.parts, std::span<const GpuPlacedPart>{table.parts});
    upload(device, frame.kinds, std::span<const GpuPlacedKind>{table.kinds});
    upload(device, frame.blockRecords, std::span<const GpuPlacedBlock>{frame.blocks});
    upload(device, frame.view, std::span<const GpuPlacedView>{views});
    upload(device, frame.groups, std::span<const GpuPlacedGroup>{table.groups});
    const auto blocks = static_cast<std::uint32_t>(frame.blocks.size());
    const auto groups = static_cast<std::uint32_t>(table.groups.size());
    ensureBuffer(device, blockCounts, static_cast<VkDeviceSize>(blocks) * groups * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    ensureBuffer(device, totals, 2 * placedMostGroups * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    const std::array<ComputeBinding, PLANTING_BINDINGS> plantingBindings = planting.getBindings(heights, frameIndex);
    const VkBuffer plants = plantingBindings[PLANTING_PLANTS_BINDING].buffer.buffer;
    const VkBuffer counts = plantingBindings[PLANTING_COUNTS_BINDING].buffer.buffer;

    // Last frame's passes are done with the shared counts and totals, and this frame's planting with the slots, which
    // these passes and the vertex shaders read.
    bufferBarrier(commands, blockCounts.get(), computeReadWrite, computeReadWrite);
    bufferBarrier(commands, totals.get(), computeReadWrite, computeReadWrite);
    bufferBarrier(commands, plants, computeReadWrite, {computeRead.stages | vertexRead.stages, computeRead.access});
    bufferBarrier(commands, counts, computeReadWrite, computeRead);

    std::vector<ComputeBinding> bindings(plantingBindings.begin(), plantingBindings.end());

    for (const VkBuffer buffer : {frame.parts.get(), frame.kinds.get(), frame.blockRecords.get(), frame.view.get(), frame.groups.get(), blockCounts.get(),
                                  totals.get(), frame.commands.get(), frame.entries.get()}) {
        bindings.push_back(storageBufferBinding(buffer));
    }

    // planting_cells.glsl's push constants, which these passes never read.
    const auto dispatch = [&](const ComputePipeline& pipeline, std::uint32_t workgroups) {
        const std::array<std::uint32_t, 4> pushed{0, 0, 0, 0};
        bindComputeSet(commands, device.getDevice(), descriptors, pipeline, bindings);
        vkCmdPushConstants(commands, pipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes, pushed.data());
        vkCmdDispatch(commands, workgroups, 1, 1);
    };

    dispatch(*countPass, blocks);
    bufferBarrier(commands, blockCounts.get(), computeReadWrite, computeReadWrite);
    dispatch(*scanPass, groups);
    bufferBarrier(commands, blockCounts.get(), computeReadWrite, computeReadWrite);
    bufferBarrier(commands, totals.get(), computeReadWrite, computeReadWrite);
    dispatch(*commandsPass, 1);
    bufferBarrier(commands, totals.get(), computeReadWrite, computeReadWrite);
    dispatch(*writePass, blocks);

    // Then the vertex shaders read the entries and the indirect draws the commands (and, for a comparison, copies).
    const bool reading = compareAsked;
    const BufferUse entriesThen{vertexRead.stages | (reading ? transferRead.stages : 0), vertexRead.access | (reading ? transferRead.access : 0)};
    const BufferUse commandsThen{indirectRead.stages | (reading ? transferRead.stages : 0), indirectRead.access | (reading ? transferRead.access : 0)};
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
        .view = view,
        .field = frame.field,
    };

    for (const auto& [from, to] : {std::pair{frame.entries.get(), &readback.entries}, std::pair{frame.commands.get(), &readback.commands},
                                   std::pair{plants, &readback.plants}, std::pair{counts, &readback.counts}}) {
        const VkBufferCopy copy{0, 0, to->getSize()};
        vkCmdCopyBuffer(commands, from, to->get(), 1, &copy);
        bufferBarrier(commands, to->get(), transferWrite, hostRead);
    }

    // The slots go back to being read by the vertex shaders, and written by the next frame's planting.
    bufferBarrier(commands, plants, transferRead, vertexRead);
    frame.readback = std::move(readback);
}

VulkanPlacedDrawing::Draws VulkanPlacedDrawing::draws(std::uint32_t frameIndex) const {
    const PerFrame& frame = frames.at(frameIndex);
    return Draws{frame.commands.get(), &frame.table.order};
}

std::string VulkanPlacedDrawing::compare() {
    compareAsked = true;
    return report;
}

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
    const std::span<const GpuDrawCommand> commands{static_cast<const GpuDrawCommand*>(readback->commands.getMapped()), readback->table.groups.size()};
    const std::span<const GpuPlacedEntry> entries{static_cast<const GpuPlacedEntry*>(readback->entries.getMapped()),
                                                  readback->entries.getSize() / sizeof(GpuPlacedEntry)};
    const PlacedCullResult cpu = cullPlacedOnCpu(readback->table, readback->blocks, plants, counts, readback->field->maps, readback->view);
    report = describePlacedComparison(comparePlacedDraws(readback->table, plants, readback->view, cpu, commands, entries));
    readback.reset();
}
