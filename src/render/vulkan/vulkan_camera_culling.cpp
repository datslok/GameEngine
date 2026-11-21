#include "render/vulkan/vulkan_camera_culling.h"
#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/vulkan_device.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace {
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
    segmentPass = std::make_unique<ComputePipeline>(device, passDescription("assets/shaders/camera_cull_segments.comp.spv", 5));
    segmentScanPass = std::make_unique<ComputePipeline>(device, passDescription("assets/shaders/camera_cull_segment_scan.comp.spv", 4));
    segmentListPass = std::make_unique<ComputePipeline>(device, passDescription("assets/shaders/camera_cull_segment_list.comp.spv", 4));
    flagPass = std::make_unique<ComputePipeline>(device, passDescription("assets/shaders/camera_cull_flags.comp.spv", 11));
    scanPass = std::make_unique<ComputePipeline>(device, passDescription("assets/shaders/camera_cull_scan.comp.spv", 4));
    writePass = std::make_unique<ComputePipeline>(device, passDescription("assets/shaders/camera_cull_write.comp.spv", 14));
    compactPass = std::make_unique<ComputePipeline>(device, passDescription("assets/shaders/camera_cull_compact.comp.spv", 8));
    records = std::make_unique<VulkanLastingBuffer>(device, sizeof(GpuCullRecord));
    chunks = std::make_unique<VulkanLastingBuffer>(device, sizeof(GpuCullChunk));
    candidates = std::make_unique<VulkanLastingBuffer>(device, sizeof(GpuCullCandidate));
    segments = std::make_unique<VulkanLastingBuffer>(device, sizeof(GpuCullSegment));
    setFramesInFlight(framesInFlight);
}

void VulkanCameraCulling::setFramesInFlight(std::uint32_t count) {
    frames.clear();
    frames.resize(count);
}

/*
* The kept records, chunks, candidates and segments: the still part as held (the same generation), what the next
* generation repacked (records, chunks and candidates by range, the segments whole when they changed), or all of it
* (another generation, or a buffer made afresh); the moving draws' after it every frame. The levels, groups, views and
* drawing order are the frame in flight's own, written whole.
*/
void VulkanCameraCulling::upload(VkCommandBuffer commands, std::uint32_t frameIndex, const CameraCullFrame& table, std::span<const GpuCameraCull> views) {
    PerFrame& frame = frames.at(frameIndex);
    const bool held = heldGeneration != 0 && table.stillGeneration == heldGeneration;
    const bool follows = heldGeneration != 0 && table.stillGeneration == heldGeneration + 1;

    // A still part's ranges this frame: none when held, the repacked ones when following, else every one.
    const auto stillRanges = [&](bool fresh, std::size_t count, const std::vector<DrawRange>& repacked) {
        return fresh || !(held || follows) ? std::vector<DrawRange>{DrawRange{0, count}} : held ? std::vector<DrawRange>{} : repacked;
    };

    const std::size_t stillRecords = table.stillRecords.size();
    std::vector<LastingWrite> recordWrites;
    const bool freshRecords = records->reserve(stillRecords + table.movingRecords.size());
    appendLastingWrites(recordWrites, stillRanges(freshRecords, stillRecords, table.stillRepacked), table.stillRecords, 0);
    appendLastingWrites(recordWrites, {DrawRange{0, table.movingRecords.size()}}, std::span<const GpuCullRecord>{table.movingRecords}, stillRecords);
    records->write(commands, frameIndex, recordWrites);

    std::vector<LastingWrite> chunkWrites;
    const bool freshChunks = chunks->reserve(table.stillChunks.size());
    appendLastingWrites(chunkWrites, stillRanges(freshChunks, table.stillChunks.size(), table.chunksRepacked), table.stillChunks, 0);
    chunks->write(commands, frameIndex, chunkWrites);

    const std::size_t stillCandidates = table.stillCandidates.size();
    std::vector<LastingWrite> candidateWrites;
    const bool freshCandidates = candidates->reserve(stillCandidates + table.movingCandidates.size());
    appendLastingWrites(candidateWrites, stillRanges(freshCandidates, stillCandidates, table.candidatesRepacked), table.stillCandidates, 0);
    appendLastingWrites(candidateWrites, {DrawRange{0, table.movingCandidates.size()}}, std::span<const GpuCullCandidate>{table.movingCandidates},
                        stillCandidates);
    candidates->write(commands, frameIndex, candidateWrites);

    const std::size_t stillSegments = table.stillSegments.size();
    const std::vector<DrawRange> segmentsChanged = table.segmentsChanged ? std::vector<DrawRange>{DrawRange{0, stillSegments}} : std::vector<DrawRange>{};
    std::vector<LastingWrite> segmentWrites;
    const bool freshSegments = segments->reserve(stillSegments + table.movingSegments.size());
    appendLastingWrites(segmentWrites, stillRanges(freshSegments, stillSegments, segmentsChanged), table.stillSegments, 0);
    appendLastingWrites(segmentWrites, {DrawRange{0, table.movingSegments.size()}}, std::span<const GpuCullSegment>{table.movingSegments}, stillSegments);
    segments->write(commands, frameIndex, segmentWrites);
    heldGeneration = table.stillGeneration;

    ensureBuffer(device.getAllocator(), frame.levels, table.meshLevels.size() * sizeof(GpuMeshLevels), storage, BufferMemory::Upload);
    ensureBuffer(device.getAllocator(), frame.groups, table.groups.size() * sizeof(GpuCullGroup), storage, BufferMemory::Upload);
    ensureBuffer(device.getAllocator(), frame.views, views.size_bytes(), storage, BufferMemory::Upload);
    ensureBuffer(device.getAllocator(), frame.order, table.drawOrder.order.size() * sizeof(std::uint32_t), storage, BufferMemory::Upload);
    writeAt(frame.levels, 0, std::span<const GpuMeshLevels>{table.meshLevels});
    writeAt(frame.groups, 0, std::span<const GpuCullGroup>{table.groups});
    writeAt(frame.views, 0, views);
    writeAt(frame.order, 0, std::span<const std::uint32_t>{table.drawOrder.order});

    for (const VulkanBuffer* buffer : {&frame.levels, &frame.groups, &frame.views, &frame.order}) {
        buffer->flush();
    }
}

std::size_t VulkanCameraCulling::getCopiedBytes() const {
    return records->getCopiedBytes() + chunks->getCopiedBytes() + candidates->getCopiedBytes() + segments->getCopiedBytes();
}

VulkanCameraCulling::Outputs VulkanCameraCulling::outputs(std::uint32_t frameIndex) const {
    const PerFrame& frame = frames.at(frameIndex);
    return Outputs{frame.views.get(), frame.order.get(), frame.groups.get(), frame.mainCommands.get(), frame.motionCommands.get(), records->get()};
}

const GpuCameraCull& VulkanCameraCulling::placedView(std::uint32_t frameIndex, std::uint32_t view) const {
    return frames.at(frameIndex).placed.at(view);
}


// A batch's views bound from its first: the shaders read views[gl_WorkGroupID.y]. Offsets of a whole batch of views are
// a multiple of every device's storage buffer offset alignment (at most 256 bytes).
static_assert((cullScratchViews * sizeof(GpuCameraCull)) % 256 == 0, "A batch of views must start at an aligned offset");

namespace {
    // Each batch's indirect dispatches start this many bytes after the one before's: an offset every device allows for
    // a storage buffer.
    constexpr VkDeviceSize dispatchStride = 256;
    static_assert(CAMERA_CULL_DISPATCH_WORDS * sizeof(std::uint32_t) <= dispatchStride, "A batch's dispatches fit their stride");

    constexpr BufferUse indirectRead{VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
}

/*
* The counts cleared and every batch's indirect dispatches started (the flags pass's at none, the write pass's at the
* groups' workgroups), then for each batch of cullScratchViews views (the dispatch's second dimension), reusing the
* scratch buffers once the batch before has written its lists: the segment pass (each segment's chunk tested), the
* segment scan (each view's visible candidates, raising the batch's dispatches), the visible segment list, then the
* flags pass over the visible candidates only (indirect), the scan, and the writes (indirect); then the packing over
* every view at once. Each pass waits for the one before's writes. The working buffers and the commands are the frame in
* flight's own, last used by its previous frame, whose fence has passed. Afterwards the lists are read by the vertex
* shaders and the commands by the indirect draws, and both copied for the CPU.
*/
void VulkanCameraCulling::cull(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, const CameraCullFrame& table,
                               std::span<const GpuCameraCull> asked, VkBuffer drawList, bool readLists) {
    if (asked.empty()) {
        throw std::invalid_argument("GPU culling takes at least the camera's view");
    }

    PerFrame& frame = frames.at(frameIndex);
    const std::uint32_t candidateCount = static_cast<std::uint32_t>(table.stillCandidates.size() + table.movingCandidates.size());
    const std::uint32_t segmentCount = static_cast<std::uint32_t>(table.stillSegments.size() + table.movingSegments.size());
    const std::uint32_t groupCount = static_cast<std::uint32_t>(table.groups.size());
    const std::uint32_t entriesEach = static_cast<std::uint32_t>(table.stillRecords.size() + table.movingRecords.size());
    const std::uint32_t viewCount = static_cast<std::uint32_t>(asked.size());
    const std::uint32_t batches = (viewCount + static_cast<std::uint32_t>(cullScratchViews) - 1) / static_cast<std::uint32_t>(cullScratchViews);
    std::vector<GpuCameraCull> views(asked.begin(), asked.end());

    for (std::uint32_t view = 0; view < viewCount; ++view) {
        placeCullView(views[view], view, candidateCount, segmentCount, table.drawOrder);
    }

    const std::uint32_t segmentBlocks = views[0].segmentBlocks;
    const std::uint32_t groupBlocks = (groupCount + cameraCullGroupSize - 1) / cameraCullGroupSize;
    upload(commands, frameIndex, table, views);
    frame.placed = views;

    const CullViewSizes sizes = cullViewSizes(viewCount, candidateCount, segmentCount, table.drawOrder);
    const VkBufferUsageFlags commandUsage = storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    const VkDeviceSize commandBytes = static_cast<VkDeviceSize>(sizes.commands) * sizeof(GpuDrawCommand);
    const VkDeviceSize countBytes = static_cast<VkDeviceSize>(viewCount) * sizeof(GpuCameraCullCounts);
    const VkBufferUsageFlags drawUsage = storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    ensureBuffer(device.getAllocator(), frame.segmentOffsets, static_cast<VkDeviceSize>(sizes.segments) * 2 * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    ensureBuffer(device.getAllocator(), frame.visibleSegments, static_cast<VkDeviceSize>(sizes.segments) * 2 * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    ensureBuffer(device.getAllocator(), frame.segmentBlockSums, static_cast<VkDeviceSize>(sizes.segmentBlocks) * 2 * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    ensureBuffer(device.getAllocator(), frame.work, static_cast<VkDeviceSize>(viewCount) * sizeof(GpuCullViewWork), storage, BufferMemory::Gpu);
    ensureBuffer(device.getAllocator(), frame.dispatches, static_cast<VkDeviceSize>(batches) * dispatchStride, storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 BufferMemory::Gpu);
    ensureBuffer(device.getAllocator(), frame.flags, static_cast<VkDeviceSize>(sizes.flags) * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    ensureBuffer(device.getAllocator(), frame.offsets, static_cast<VkDeviceSize>(sizes.flags) * 2 * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    ensureBuffer(device.getAllocator(), frame.blockSums, static_cast<VkDeviceSize>(sizes.blocks) * 2 * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    ensureBuffer(device.getAllocator(), frame.counts, countBytes, storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Gpu);
    ensureBuffer(device.getAllocator(), frame.mainCommands, commandBytes, commandUsage, BufferMemory::Gpu);
    ensureBuffer(device.getAllocator(), frame.motionCommands, commandBytes, commandUsage, BufferMemory::Gpu);
    ensureBuffer(device.getAllocator(), frame.places, static_cast<VkDeviceSize>(sizes.places) * 2 * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    ensureBuffer(device.getAllocator(), frame.packedMain, commandBytes, drawUsage, BufferMemory::Gpu);
    ensureBuffer(device.getAllocator(), frame.packedMotion, commandBytes, drawUsage, BufferMemory::Gpu);
    ensureBuffer(device.getAllocator(), frame.drawCounts, static_cast<VkDeviceSize>(viewCount) * CAMERA_CULL_DRAW_COUNTS * sizeof(std::uint32_t), drawUsage,
                 BufferMemory::Gpu);
    ensureBuffer(device.getAllocator(), frame.readback, countBytes + 2 * commandBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback);

    // The previous copy of the counts (its frame finished) before they are cleared; cleared before the passes add to them.
    bufferBarrier(commands, frame.counts.get(), transferRead, transferWrite);
    vkCmdFillBuffer(commands, frame.counts.get(), 0, VK_WHOLE_SIZE, 0);
    bufferBarrier(commands, frame.counts.get(), transferWrite, computeReadWrite);

    // Every batch's dispatches started: the flags pass at no workgroups, the write pass at the groups', each y the batch's
    // views; the segment scan raises them. Their previous frame's reads finished long ago (its fence).
    std::vector<std::uint32_t> dispatchWords(static_cast<std::size_t>(batches) * dispatchStride / sizeof(std::uint32_t), 0);

    for (std::uint32_t batch = 0; batch < batches; ++batch) {
        const std::uint32_t inBatch = std::min(viewCount - batch * static_cast<std::uint32_t>(cullScratchViews), static_cast<std::uint32_t>(cullScratchViews));
        std::uint32_t* words = dispatchWords.data() + batch * dispatchStride / sizeof(std::uint32_t);
        words[0] = 0;
        words[1] = inBatch;
        words[2] = 1;
        words[3] = groupBlocks;
        words[4] = inBatch;
        words[5] = 1;
    }

    bufferBarrier(commands, frame.dispatches.get(), indirectRead, transferWrite);
    vkCmdUpdateBuffer(commands, frame.dispatches.get(), 0, dispatchWords.size() * sizeof(std::uint32_t), dispatchWords.data());
    bufferBarrier(commands, frame.dispatches.get(), transferWrite, computeReadWrite);

    for (std::uint32_t batchIndex = 0; batchIndex < batches; ++batchIndex) {
        const std::uint32_t first = batchIndex * static_cast<std::uint32_t>(cullScratchViews);
        const std::uint32_t batch = std::min(viewCount - first, static_cast<std::uint32_t>(cullScratchViews));
        const VkDeviceSize dispatchOffset = static_cast<VkDeviceSize>(batchIndex) * dispatchStride;
        ComputeBinding batchViews = storageBufferBinding(frame.views.get());
        batchViews.buffer.offset = static_cast<VkDeviceSize>(first) * sizeof(GpuCameraCull);
        batchViews.buffer.range = static_cast<VkDeviceSize>(batch) * sizeof(GpuCameraCull);
        ComputeBinding batchDispatches = storageBufferBinding(frame.dispatches.get());
        batchDispatches.buffer.offset = dispatchOffset;
        batchDispatches.buffer.range = CAMERA_CULL_DISPATCH_WORDS * sizeof(std::uint32_t);

        // The batch before's passes have read the scratch buffers this batch overwrites.
        if (first > 0) {
            for (VkBuffer buffer : {frame.segmentOffsets.get(), frame.segmentBlockSums.get(), frame.visibleSegments.get(), frame.flags.get(), frame.offsets.get(),
                                    frame.blockSums.get()}) {
                bufferBarrier(commands, buffer, computeReadWrite, computeReadWrite);
            }
        }

        // The segments: each one's chunk tested, then summed in blocks.
        if (segmentBlocks > 0) {
            const std::array<ComputeBinding, 5> segmentBindings{
                batchViews, storageBufferBinding(chunks->get()), storageBufferBinding(segments->get()), storageBufferBinding(frame.segmentOffsets.get()),
                storageBufferBinding(frame.segmentBlockSums.get()),
            };
            bindComputeSet(commands, device.getDevice(), descriptors, *segmentPass, segmentBindings);
            vkCmdDispatch(commands, segmentBlocks, batch, 1);
        }

        for (VkBuffer buffer : {frame.segmentBlockSums.get(), frame.segmentOffsets.get()}) {
            bufferBarrier(commands, buffer, computeWrite, computeReadWrite);
        }

        // The segment scan: the blocks' first places, each view's work, the batch's dispatches raised.
        const std::array<ComputeBinding, 4> segmentScanBindings{batchViews, storageBufferBinding(frame.segmentBlockSums.get()),
                                                                storageBufferBinding(frame.work.get()), batchDispatches};
        bindComputeSet(commands, device.getDevice(), descriptors, *segmentScanPass, segmentScanBindings);
        vkCmdDispatch(commands, 1, batch, 1);

        bufferBarrier(commands, frame.segmentBlockSums.get(), computeWrite, computeReadWrite);
        bufferBarrier(commands, frame.work.get(), computeWrite, computeReadWrite);
        bufferBarrier(commands, frame.dispatches.get(), computeReadWrite,
                      {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});

        // The visible segments listed in order.
        if (segmentBlocks > 0) {
            const std::array<ComputeBinding, 4> listBindings{batchViews, storageBufferBinding(frame.segmentOffsets.get()),
                                                             storageBufferBinding(frame.segmentBlockSums.get()), storageBufferBinding(frame.visibleSegments.get())};
            bindComputeSet(commands, device.getDevice(), descriptors, *segmentListPass, listBindings);
            vkCmdDispatch(commands, segmentBlocks, batch, 1);
        }

        bufferBarrier(commands, frame.visibleSegments.get(), computeWrite, computeReadWrite);

        // The visible candidates' flags (indirect: the batch's most visible candidates' workgroups).
        const std::array<ComputeBinding, 11> flagBindings{
            storageBufferBinding(records->get()),        storageBufferBinding(frame.levels.get()),      storageBufferBinding(candidates->get()),
            batchViews,                                  storageBufferBinding(frame.flags.get()),       storageBufferBinding(frame.offsets.get()),
            storageBufferBinding(frame.blockSums.get()), storageBufferBinding(frame.counts.get()),      storageBufferBinding(segments->get()),
            storageBufferBinding(frame.visibleSegments.get()), storageBufferBinding(frame.work.get()),
        };
        bindComputeSet(commands, device.getDevice(), descriptors, *flagPass, flagBindings);
        vkCmdDispatchIndirect(commands, frame.dispatches.get(), dispatchOffset);

        // The flags pass's block totals and level counts before the scan sums them; its flags and offsets wait for the writes.
        for (VkBuffer buffer : {frame.blockSums.get(), frame.counts.get(), frame.flags.get(), frame.offsets.get()}) {
            bufferBarrier(commands, buffer, computeWrite, computeReadWrite);
        }

        const std::array<ComputeBinding, 4> scanBindings{batchViews, storageBufferBinding(frame.blockSums.get()), storageBufferBinding(frame.counts.get()),
                                                         storageBufferBinding(frame.work.get())};
        bindComputeSet(commands, device.getDevice(), descriptors, *scanPass, scanBindings);
        vkCmdDispatch(commands, 1, batch, 1);

        // The blocks' first places before the writes read them.
        bufferBarrier(commands, frame.blockSums.get(), computeWrite, computeReadWrite);

        // The writes (indirect: the visible candidates' or the groups' workgroups, whichever are more).
        const std::array<ComputeBinding, 14> writeBindings{
            batchViews,                                     storageBufferBinding(candidates->get()),         storageBufferBinding(frame.flags.get()),
            storageBufferBinding(frame.offsets.get()),      storageBufferBinding(frame.blockSums.get()),     storageBufferBinding(frame.groups.get()),
            storageBufferBinding(drawList),                 storageBufferBinding(frame.mainCommands.get()),  storageBufferBinding(frame.motionCommands.get()),
            storageBufferBinding(frame.segmentOffsets.get()), storageBufferBinding(frame.segmentBlockSums.get()), storageBufferBinding(frame.work.get()),
            storageBufferBinding(records->get()),            storageBufferBinding(frame.levels.get()),
        };
        bindComputeSet(commands, device.getDevice(), descriptors, *writePass, writeBindings);
        vkCmdDispatchIndirect(commands, frame.dispatches.get(), dispatchOffset + 3 * sizeof(std::uint32_t));
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
        ensureBuffer(device.getAllocator(), frame.listReadback, 2 * viewCount * listBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback);
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
