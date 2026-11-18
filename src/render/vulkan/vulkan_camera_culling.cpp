#include "render/vulkan/vulkan_camera_culling.h"
#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/vulkan_device.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace {
    // Room for a few records of any kind before the first growth: no buffer is ever empty.
    constexpr VkDeviceSize smallestBytes = 256;

    // Grown (to twice its size, or what is asked) when too small; true when made afresh, so its contents are gone.
    bool ensureBuffer(const VulkanDevice& device, VulkanBuffer& buffer, VkDeviceSize bytes, VkBufferUsageFlags usage, BufferMemory memory) {
        bytes = std::max(bytes, smallestBytes);

        if (buffer.getSize() >= bytes) {
            return false;
        }

        buffer = VulkanBuffer{device.getAllocator(), std::max(bytes, 2 * buffer.getSize()), usage, memory};
        return true;
    }

    // Copies a span's bytes into an upload buffer at a byte offset.
    template<typename Element>
    void writeAt(VulkanBuffer& buffer, VkDeviceSize offset, std::span<const Element> elements) {
        if (!elements.empty()) {
            std::memcpy(static_cast<std::byte*>(buffer.getMapped()) + offset, elements.data(), elements.size_bytes());
        }
    }

    constexpr VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    constexpr BufferUse computeWrite{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    constexpr BufferUse computeReadWrite{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    constexpr BufferUse transferRead{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
    constexpr BufferUse transferWrite{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    constexpr BufferUse hostRead{VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};

    ComputePipelineDescription passDescription(const char* shader, std::uint32_t buffers) {
        ComputePipelineDescription description;
        description.shader = shader;
        description.storageBuffers = buffers;
        return description;
    }
}

VulkanCameraCulling::VulkanCameraCulling(const VulkanDevice& device, std::uint32_t framesInFlight):
    device(device)
{
    flagPass = std::make_unique<ComputePipeline>(device, passDescription("assets/shaders/camera_cull_flags.comp.spv", 8));
    scanPass = std::make_unique<ComputePipeline>(device, passDescription("assets/shaders/camera_cull_scan.comp.spv", 3));
    writePass = std::make_unique<ComputePipeline>(device, passDescription("assets/shaders/camera_cull_write.comp.spv", 9));
    compactPass = std::make_unique<ComputePipeline>(device, passDescription("assets/shaders/camera_cull_compact.comp.spv", 8));
    setFramesInFlight(framesInFlight);
}

void VulkanCameraCulling::setFramesInFlight(std::uint32_t count) {
    frames.clear();
    frames.resize(count);
}

/*
* The still records and candidates go first in their buffers and are written only when the frame in flight holds an
* older still part (or its buffer grew); the moving draws' follow them every frame, as do the levels, groups and views.
*/
void VulkanCameraCulling::upload(PerFrame& frame, const CameraCullFrame& table, std::span<const GpuCameraCull> views) {
    const std::size_t recordCount = table.stillRecords.size() + table.movingRecords.size();
    const std::size_t candidateCount = table.stillCandidates.size() + table.movingCandidates.size();
    bool rewriteStill = frame.heldGeneration != table.stillGeneration;
    rewriteStill = ensureBuffer(device, frame.records, recordCount * sizeof(GpuCullRecord), storage, BufferMemory::Upload) || rewriteStill;
    rewriteStill = ensureBuffer(device, frame.candidates, candidateCount * sizeof(GpuCullCandidate), storage, BufferMemory::Upload) || rewriteStill;
    ensureBuffer(device, frame.levels, table.meshLevels.size() * sizeof(GpuMeshLevels), storage, BufferMemory::Upload);
    ensureBuffer(device, frame.groups, table.groups.size() * sizeof(GpuCullGroup), storage, BufferMemory::Upload);
    ensureBuffer(device, frame.views, views.size_bytes(), storage, BufferMemory::Upload);
    ensureBuffer(device, frame.order, table.drawOrder.order.size() * sizeof(std::uint32_t), storage, BufferMemory::Upload);

    if (rewriteStill) {
        writeAt(frame.records, 0, table.stillRecords);
        writeAt(frame.candidates, 0, table.stillCandidates);
        frame.heldGeneration = table.stillGeneration;
    }

    writeAt(frame.records, table.stillRecords.size_bytes(), std::span<const GpuCullRecord>{table.movingRecords});
    writeAt(frame.candidates, table.stillCandidates.size_bytes(), std::span<const GpuCullCandidate>{table.movingCandidates});
    writeAt(frame.levels, 0, std::span<const GpuMeshLevels>{table.meshLevels});
    writeAt(frame.groups, 0, std::span<const GpuCullGroup>{table.groups});
    writeAt(frame.views, 0, views);
    writeAt(frame.order, 0, std::span<const std::uint32_t>{table.drawOrder.order});

    for (const VulkanBuffer* buffer : {&frame.records, &frame.candidates, &frame.levels, &frame.groups, &frame.views, &frame.order}) {
        buffer->flush();
    }
}


// A batch's views bound from its first: the shaders read views[gl_WorkGroupID.y]. Offsets of a whole batch of views are
// a multiple of every device's storage buffer offset alignment (at most 256 bytes).
static_assert((cullScratchViews * sizeof(GpuCameraCull)) % 256 == 0, "A batch of views must start at an aligned offset");

/*
* The counts cleared, then the passes: flags, scan and writes in batches of cullScratchViews views (the dispatch's second
* dimension), each batch reusing the flags, offsets and block sums once the batch before has written its lists; then the
* packing over every view at once. Each pass waits for the one before's writes. The working buffers and the commands are
* the frame in flight's own, last used by its previous frame, whose fence has passed. Afterwards the lists are read by
* the vertex shaders and the commands by the indirect draws, and both copied for the CPU.
*/
void VulkanCameraCulling::cull(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, const CameraCullFrame& table,
                               std::span<const GpuCameraCull> asked, VkBuffer drawList, bool readLists) {
    if (asked.empty()) {
        throw std::invalid_argument("GPU culling takes at least the camera's view");
    }

    PerFrame& frame = frames.at(frameIndex);
    const std::uint32_t candidateCount = static_cast<std::uint32_t>(table.stillCandidates.size() + table.movingCandidates.size());
    const std::uint32_t groupCount = static_cast<std::uint32_t>(table.groups.size());
    const std::uint32_t entriesEach = static_cast<std::uint32_t>(table.stillRecords.size() + table.movingRecords.size());
    const std::uint32_t viewCount = static_cast<std::uint32_t>(asked.size());
    std::vector<GpuCameraCull> views(asked.begin(), asked.end());

    for (std::uint32_t view = 0; view < viewCount; ++view) {
        placeCullView(views[view], view, candidateCount, table.drawOrder);
    }

    const std::uint32_t blocks = views[0].blocks;
    upload(frame, table, views);

    const CullViewSizes sizes = cullViewSizes(viewCount, candidateCount, table.drawOrder);
    const VkBufferUsageFlags commandUsage = storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    const VkDeviceSize commandBytes = static_cast<VkDeviceSize>(sizes.commands) * sizeof(GpuDrawCommand);
    const VkDeviceSize countBytes = static_cast<VkDeviceSize>(viewCount) * sizeof(GpuCameraCullCounts);
    const VkBufferUsageFlags drawUsage = storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    ensureBuffer(device, frame.flags, static_cast<VkDeviceSize>(sizes.flags) * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    ensureBuffer(device, frame.offsets, static_cast<VkDeviceSize>(sizes.flags) * 2 * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    ensureBuffer(device, frame.blockSums, static_cast<VkDeviceSize>(sizes.blocks) * 2 * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    ensureBuffer(device, frame.counts, countBytes, storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Gpu);
    ensureBuffer(device, frame.mainCommands, commandBytes, commandUsage, BufferMemory::Gpu);
    ensureBuffer(device, frame.motionCommands, commandBytes, commandUsage, BufferMemory::Gpu);
    ensureBuffer(device, frame.places, static_cast<VkDeviceSize>(sizes.places) * 2 * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    ensureBuffer(device, frame.packedMain, commandBytes, drawUsage, BufferMemory::Gpu);
    ensureBuffer(device, frame.packedMotion, commandBytes, drawUsage, BufferMemory::Gpu);
    ensureBuffer(device, frame.drawCounts, static_cast<VkDeviceSize>(viewCount) * CAMERA_CULL_DRAW_COUNTS * sizeof(std::uint32_t), drawUsage,
                 BufferMemory::Gpu);
    ensureBuffer(device, frame.readback, countBytes + 2 * commandBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback);

    // The previous copy of the counts (its frame finished) before they are cleared; cleared before the passes add to them.
    bufferBarrier(commands, frame.counts.get(), transferRead, transferWrite);
    vkCmdFillBuffer(commands, frame.counts.get(), 0, VK_WHOLE_SIZE, 0);
    bufferBarrier(commands, frame.counts.get(), transferWrite, computeReadWrite);

    const std::uint32_t writers = std::max(candidateCount, groupCount);

    for (std::uint32_t first = 0; first < viewCount; first += static_cast<std::uint32_t>(cullScratchViews)) {
        const std::uint32_t batch = std::min(viewCount - first, static_cast<std::uint32_t>(cullScratchViews));
        ComputeBinding batchViews = storageBufferBinding(frame.views.get());
        batchViews.buffer.offset = static_cast<VkDeviceSize>(first) * sizeof(GpuCameraCull);
        batchViews.buffer.range = static_cast<VkDeviceSize>(batch) * sizeof(GpuCameraCull);

        // The batch before's writes have read the scratch buffers this batch's flags overwrite.
        if (first > 0) {
            for (VkBuffer buffer : {frame.flags.get(), frame.offsets.get(), frame.blockSums.get()}) {
                bufferBarrier(commands, buffer, computeReadWrite, computeReadWrite);
            }
        }

        if (blocks > 0) {
            const std::array<ComputeBinding, 8> bindings{
                storageBufferBinding(frame.records.get()),   storageBufferBinding(frame.levels.get()), storageBufferBinding(frame.candidates.get()),
                batchViews,                                  storageBufferBinding(frame.flags.get()),  storageBufferBinding(frame.offsets.get()),
                storageBufferBinding(frame.blockSums.get()), storageBufferBinding(frame.counts.get()),
            };
            bindComputeSet(commands, device.getDevice(), descriptors, *flagPass, bindings);
            vkCmdDispatch(commands, blocks, batch, 1);
        }

        // The flags pass's block totals and level counts before the scan sums them; its flags and offsets wait for pass 3.
        for (VkBuffer buffer : {frame.blockSums.get(), frame.counts.get(), frame.flags.get(), frame.offsets.get()}) {
            bufferBarrier(commands, buffer, computeWrite, computeReadWrite);
        }

        const std::array<ComputeBinding, 3> scanBindings{batchViews, storageBufferBinding(frame.blockSums.get()), storageBufferBinding(frame.counts.get())};
        bindComputeSet(commands, device.getDevice(), descriptors, *scanPass, scanBindings);
        vkCmdDispatch(commands, 1, batch, 1);

        // The blocks' first places before the writes read them.
        bufferBarrier(commands, frame.blockSums.get(), computeWrite, computeReadWrite);

        if (writers > 0) {
            const std::array<ComputeBinding, 9> writeBindings{
                batchViews,                                storageBufferBinding(frame.candidates.get()), storageBufferBinding(frame.flags.get()),
                storageBufferBinding(frame.offsets.get()), storageBufferBinding(frame.blockSums.get()),  storageBufferBinding(frame.groups.get()),
                storageBufferBinding(drawList),            storageBufferBinding(frame.mainCommands.get()), storageBufferBinding(frame.motionCommands.get()),
            };
            bindComputeSet(commands, device.getDevice(), descriptors, *writePass, writeBindings);
            vkCmdDispatch(commands, (writers + cameraCullGroupSize - 1) / cameraCullGroupSize, batch, 1);
        }
    }

    // The lists for the scene's vertex shaders (and the copy below); the commands for the packing, the indirect draws of
    // the groups drawn one by one and the copy; the counts, finished by the scan, for the copy.
    bufferBarrier(commands, drawList, computeWrite,
                  {VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT});

    for (VkBuffer buffer : {frame.mainCommands.get(), frame.motionCommands.get()}) {
        bufferBarrier(commands, buffer, computeWrite,
                      {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT});
    }

    // The packing: last frame's draws from this frame in flight's packed commands finished long ago (its fence), so they
    // are written straight away, then read by the indirect draws with their counts.
    const std::array<ComputeBinding, 8> compactBindings{
        storageBufferBinding(frame.views.get()),        storageBufferBinding(frame.order.get()),
        storageBufferBinding(frame.mainCommands.get()), storageBufferBinding(frame.motionCommands.get()),
        storageBufferBinding(frame.packedMain.get()),   storageBufferBinding(frame.packedMotion.get()),
        storageBufferBinding(frame.places.get()),       storageBufferBinding(frame.drawCounts.get()),
    };
    bindComputeSet(commands, device.getDevice(), descriptors, *compactPass, compactBindings);
    vkCmdDispatch(commands, 1, viewCount, 1);

    for (VkBuffer buffer : {frame.packedMain.get(), frame.packedMotion.get(), frame.drawCounts.get()}) {
        bufferBarrier(commands, buffer, computeWrite, {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT});
    }

    bufferBarrier(commands, frame.counts.get(), computeWrite, transferRead);

    // Read back: every view's counts, then every view's commands of each list.
    const std::array<VkBufferCopy, 3> copies{
        VkBufferCopy{0, 0, countBytes},
        VkBufferCopy{0, countBytes, commandBytes},
        VkBufferCopy{0, countBytes + commandBytes, commandBytes},
    };
    vkCmdCopyBuffer(commands, frame.counts.get(), frame.readback.get(), 1, &copies[0]);

    if (groupCount > 0) {
        vkCmdCopyBuffer(commands, frame.mainCommands.get(), frame.readback.get(), 1, &copies[1]);
        vkCmdCopyBuffer(commands, frame.motionCommands.get(), frame.readback.get(), 1, &copies[2]);
    }

    frame.groupCount = groupCount;
    frame.viewCount = viewCount;
    frame.listsPending = readLists && entriesEach > 0;

    // For a comparison: each view's two lists, each as long as the draws.
    if (frame.listsPending) {
        const VkDeviceSize listBytes = static_cast<VkDeviceSize>(entriesEach) * sizeof(std::uint32_t);
        ensureBuffer(device, frame.listReadback, 2 * viewCount * listBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback);
        std::vector<VkBufferCopy> lists;

        for (std::uint32_t view = 0; view < viewCount; ++view) {
            lists.push_back(VkBufferCopy{static_cast<VkDeviceSize>(views[view].mainBase) * sizeof(std::uint32_t), 2 * view * listBytes, listBytes});
            lists.push_back(VkBufferCopy{static_cast<VkDeviceSize>(views[view].motionBase) * sizeof(std::uint32_t), (2 * view + 1) * listBytes, listBytes});
        }

        vkCmdCopyBuffer(commands, drawList, frame.listReadback.get(), static_cast<std::uint32_t>(lists.size()), lists.data());
        bufferBarrier(commands, frame.listReadback.get(), transferWrite, hostRead);
        frame.listEntries = entriesEach;
    }

    bufferBarrier(commands, frame.readback.get(), transferWrite, hostRead);
    frame.pending = true;
}

PassDraws VulkanCameraCulling::mainDraws(std::uint32_t frameIndex, const CameraCullFrame& table, std::uint32_t view) const {
    const PerFrame& frame = frames.at(frameIndex);
    return PassDraws{nullptr, &table, frame.mainCommands.get(), frame.packedMain.get(), frame.drawCounts.get(),
                     view * CAMERA_CULL_DRAW_COUNTS, static_cast<std::uint32_t>(view * table.groups.size())};
}

PassDraws VulkanCameraCulling::motionDraws(std::uint32_t frameIndex, const CameraCullFrame& table, std::uint32_t view) const {
    const PerFrame& frame = frames.at(frameIndex);
    return PassDraws{nullptr, &table, frame.motionCommands.get(), frame.packedMotion.get(), frame.drawCounts.get(),
                     view * CAMERA_CULL_DRAW_COUNTS + static_cast<std::uint32_t>(cullStyles), static_cast<std::uint32_t>(view * table.groups.size())};
}

std::optional<VulkanCameraCulling::Collected> VulkanCameraCulling::collect(std::uint32_t frameIndex) {
    PerFrame& frame = frames.at(frameIndex);

    if (!frame.pending) {
        return std::nullopt;
    }

    frame.pending = false;
    frame.readback.invalidate();
    const std::byte* bytes = static_cast<const std::byte*>(frame.readback.getMapped());
    const std::size_t countBytes = frame.viewCount * sizeof(GpuCameraCullCounts);
    const std::size_t viewCommandBytes = static_cast<std::size_t>(frame.groupCount) * sizeof(GpuDrawCommand);
    const std::byte* mainCommands = bytes + countBytes;
    const std::byte* motionCommands = mainCommands + frame.viewCount * viewCommandBytes;
    Collected collected;
    collected.counts.resize(frame.viewCount);
    collected.mainCommands.resize(frame.viewCount);
    collected.motionCommands.resize(frame.viewCount);
    std::memcpy(collected.counts.data(), bytes, countBytes);

    for (std::uint32_t view = 0; view < frame.viewCount; ++view) {
        collected.mainCommands[view].resize(frame.groupCount);
        collected.motionCommands[view].resize(frame.groupCount);

        if (frame.groupCount > 0) {
            std::memcpy(collected.mainCommands[view].data(), mainCommands + view * viewCommandBytes, viewCommandBytes);
            std::memcpy(collected.motionCommands[view].data(), motionCommands + view * viewCommandBytes, viewCommandBytes);
        }
    }

    if (frame.listsPending) {
        frame.listsPending = false;
        frame.listReadback.invalidate();
        const std::uint32_t* entries = static_cast<const std::uint32_t*>(frame.listReadback.getMapped());
        collected.mainEntries.resize(frame.viewCount);
        collected.motionEntries.resize(frame.viewCount);

        for (std::uint32_t view = 0; view < frame.viewCount; ++view) {
            const std::uint32_t* main = entries + 2 * view * frame.listEntries;
            collected.mainEntries[view].assign(main, main + frame.listEntries);
            collected.motionEntries[view].assign(main + frame.listEntries, main + 2 * frame.listEntries);
        }

        collected.lists = true;
    }

    return collected;
}
