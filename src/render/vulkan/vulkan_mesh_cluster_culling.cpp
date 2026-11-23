#include "render/vulkan/vulkan_mesh_cluster_culling.h"
#include "render/gpu/taa.h"
#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/vulkan_device.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace {
    constexpr VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    constexpr BufferUse computeRead{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    constexpr BufferUse computeWrite{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    constexpr BufferUse computeReadWrite{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    constexpr BufferUse transferWrite{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    constexpr BufferUse hostRead{VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
    constexpr BufferUse indirectRead{VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};

    // The dispatch's three words: x the blocks (written by the count pass), y and z 1.
    constexpr std::uint32_t dispatchWords = 3;

    // The shared storage buffers (mesh_cluster_pass.glsl's: the camera's tables, then the placed plants').
    constexpr std::uint32_t sharedBuffers = MESH_CLUSTER_PASS_BINDINGS;

    // The unused record bound for each placed buffer without placed plants: as large as the largest record read.
    constexpr VkDeviceSize noPlacedBytes = std::max({sizeof(GpuDrawCommand), sizeof(GpuPlacedGroup), sizeof(GpuPlacedEntry), sizeof(GpuInstance),
                                                     sizeof(GpuPlacedPart), sizeof(GpuPlacedPlant)});

    // Every pass pushes the list it makes and the frame's bones stamp (mesh_cluster_pass.glsl's MeshClusterList); those
    // testing pairs read the two pyramids after their storage buffers.
    constexpr std::uint32_t pushBytes = 2 * sizeof(std::uint32_t);

    ComputePipelineDescription passDescription(const char* shader, std::uint32_t buffers, bool pyramids) {
        ComputePipelineDescription description;
        description.shader = shader;
        description.storageBuffers = buffers;
        description.sampledImages = pyramids ? 2 : 0;
        description.pushConstantBytes = pushBytes;
        return description;
    }

    void pushList(VkCommandBuffer commands, const ComputePipeline& pipeline, std::uint32_t list, std::uint32_t boneStamp) {
        const std::uint32_t words[2] = {list, boneStamp};
        vkCmdPushConstants(commands, pipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes, words);
    }

    constexpr VkDeviceSize totalBytes = MESH_CLUSTER_LISTS * MESH_CLUSTER_TOTAL_WORDS * sizeof(std::uint32_t);
}

VulkanMeshClusterCulling::VulkanMeshClusterCulling(const VulkanDevice& device, VkSampler nearest, std::uint32_t framesInFlight):
    device(device),
    nearest(nearest)
{
    countPass = std::make_unique<ComputePipeline>(device, passDescription("assets/shaders/mesh_cluster_count.comp.spv", sharedBuffers + 1, false));
    testPass = std::make_unique<ComputePipeline>(device, passDescription("assets/shaders/mesh_cluster_test.comp.spv", sharedBuffers + 1, true));
    scanPass = std::make_unique<ComputePipeline>(device, passDescription("assets/shaders/mesh_cluster_scan.comp.spv", sharedBuffers + 4, false));
    writePass = std::make_unique<ComputePipeline>(device, passDescription("assets/shaders/mesh_cluster_write.comp.spv", sharedBuffers + 3, true));
    history = std::make_unique<VulkanDepthPyramid>(device, nearest, DepthReduction::Farthest, "Occlusion pyramid");
    earlyDepth = std::make_unique<VulkanDepthPyramid>(device, nearest, DepthReduction::Farthest, "Occlusion pyramid, early list");
    setFramesInFlight(framesInFlight);
}

void VulkanMeshClusterCulling::setFramesInFlight(std::uint32_t count) {
    frames.clear();
    frames.resize(count);
}

// The pyramids follow the size once made; until then only the size is kept (no memory while mesh clusters are off).
void VulkanMeshClusterCulling::ensureSize(RenderSize render) {
    if (render == pyramidSize) {
        return;
    }

    pyramidSize = render;
    historyKept = false;

    if (pyramidsMade) {
        history->ensureSize(render);
        earlyDepth->ensureSize(render);
    }
}

std::uint32_t VulkanMeshClusterCulling::getRoom() const {
    return room;
}

VkBuffer VulkanMeshClusterCulling::getTaskCommands(std::uint32_t frameIndex) const {
    return frames.at(frameIndex).taskCommands.get();
}

/*
* The lists sized by the room as it stands (the room grows only between frames, in collect), each list a room's run of
* the items; the totals and the task commands; the lists and totals reached by address from the mesh shaders.
*/
VulkanMeshClusterCulling::ListAddresses VulkanMeshClusterCulling::prepare(std::uint32_t frameIndex) {
    PerFrame& frame = frames.at(frameIndex);
    const VmaAllocator allocator = device.getAllocator();
    const VkBufferUsageFlags readable = storage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    frame.room = room;
    ensureBuffer(allocator, frame.items, MESH_CLUSTER_LISTS * VkDeviceSize{frame.room} * sizeof(GpuMeshClusterItem), readable, BufferMemory::Gpu);
    ensureBuffer(allocator, frame.totals, totalBytes, readable, BufferMemory::Gpu);
    ensureBuffer(allocator, frame.taskCommands, MESH_CLUSTER_LISTS * MESH_CLUSTER_STYLES * 3 * sizeof(std::uint32_t),
                 storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, BufferMemory::Gpu);
    return ListAddresses{frame.items.getDeviceAddress(), frame.totals.getDeviceAddress()};
}

void VulkanMeshClusterCulling::makeReadable(VkCommandBuffer commands, VulkanDepthPyramid& pyramid, bool& built) {
    if (pyramid.getImage().getState() == ImageState::Undefined) {
        pyramid.getImage().transition(commands, ImageState::ComputeRead, ImageContents::Discard);
        built = false;
    }
}

std::vector<ComputeBinding> VulkanMeshClusterCulling::passBindings(const PerFrame& frame, std::initializer_list<VkBuffer> own, bool pyramids) const {
    const PlacedInputs& placed = frame.inputs.placed;
    const auto placedOr = [&](VkBuffer buffer) { return storageBufferBinding(buffer != VK_NULL_HANDLE ? buffer : frame.noPlaced.get()); };
    std::vector<ComputeBinding> all{
        storageBufferBinding(frame.view.get()),  storageBufferBinding(frame.order.get()),         storageBufferBinding(frame.inputs.groups),
        storageBufferBinding(frame.inputs.commands), storageBufferBinding(frame.workEnds.get()), storageBufferBinding(frame.work.get()),
        storageBufferBinding(frame.inputs.drawList), storageBufferBinding(frame.inputs.instances.buffer), storageBufferBinding(frame.inputs.records),
        storageBufferBinding(frame.inputs.clusters), storageBufferBinding(frame.setup.get()),     storageBufferBinding(frame.occlusionRecord.get()),
        placedOr(placed.commands),                   placedOr(placed.groups),                    placedOr(placed.entries),
        placedOr(placed.partInstances.buffer),       placedOr(placed.parts),                     placedOr(placed.plants),
        placedOr(frame.inputs.twigs.regions),        placedOr(frame.inputs.twigs.entries),       placedOr(frame.inputs.twigs.twigs),
        placedOr(frame.inputs.bones.plantTrees),     placedOr(frame.inputs.bones.trees),         placedOr(frame.inputs.bones.poses),
        placedOr(frame.inputs.bones.twigBindings),
    };
    all[7].buffer = frame.inputs.instances;

    if (placed.partInstances.buffer != VK_NULL_HANDLE) {
        all[15].buffer = placed.partInstances;
    }

    for (VkBuffer buffer : own) {
        all.push_back(storageBufferBinding(buffer));
    }

    if (pyramids) {
        all.push_back(sampledBinding(nearest, history->getImage().getView()));
        all.push_back(sampledBinding(nearest, earlyDepth->getImage().getView()));
    }

    return all;
}

/*
* The frame in flight's buffers sized (the block sums by the bound on pairs, the lists by the room), its view, setup and
* occlusion record written and its dispatch's y and z set; then the count pass and the early list's passes, each
* waiting for the one before's writes. The camera culling's draw list was last made ready for the vertex shaders, so it
* is made ready for these passes too. Without occlusion the totals (and the list, when asked) are copied back here.
*
* Last frame's pyramid is the history only when the cull before this one kept it (keepDepth); then the early list tests
* against it through last frame's view-projection, taking this frame's camera-relative points.
*/
void VulkanMeshClusterCulling::cull(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, const Inputs& inputs,
                                    bool readItems) {
    PerFrame& frame = frames.at(frameIndex);
    const VmaAllocator allocator = device.getAllocator();
    const std::uint64_t blocks = (inputs.pairBound + meshClusterGroupSize - 1) / meshClusterGroupSize;
    const std::uint32_t blockRoom = static_cast<std::uint32_t>(std::min<std::uint64_t>(blocks, 0xFFFFFFFEu));

    if (frame.items.get() == VK_NULL_HANDLE) {
        throw std::logic_error("Mesh cluster culling needs prepare before cull, each frame");
    }

    if (pyramidSize.width == 0 || pyramidSize.height == 0) {
        throw std::logic_error("Mesh cluster culling needs the render size before cull");
    }

    if (!pyramidsMade) {
        history->ensureSize(pyramidSize);
        earlyDepth->ensureSize(pyramidSize);
        pyramidsMade = true;
    }

    ++culls;
    const bool hasHistory = inputs.occlusion && historyKept && historyCull + 1 == culls;
    frame.inputs = inputs;
    frame.occlusion = inputs.occlusion;
    frame.itemsPending = readItems;
    frameViewProjection = inputs.viewProjection;
    frameOrigin = inputs.origin;

    const auto places = static_cast<std::uint32_t>(inputs.order.size());
    ensureBuffer(allocator, frame.workEnds, VkDeviceSize{std::max(places, 1u)} * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    ensureBuffer(allocator, frame.work, MESH_CLUSTER_WORK_WORDS * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    ensureBuffer(allocator, frame.dispatch, dispatchWords * sizeof(std::uint32_t),
                 storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Gpu);
    ensureBuffer(allocator, frame.blockCounts, VkDeviceSize{std::max(blockRoom, 1u)} * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    ensureBuffer(allocator, frame.blockFirst, (VkDeviceSize{blockRoom} + 1) * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    const VkDeviceSize itemBytes = MESH_CLUSTER_LISTS * VkDeviceSize{frame.room} * sizeof(GpuMeshClusterItem);
    ensureBuffer(allocator, frame.readback, totalBytes + (readItems ? itemBytes : 0), VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback);

    const std::uint32_t setup[4] = {places, blockRoom, frame.room, inputs.drawGroups};
    MeshClusterOcclusionView view;
    view.previous = hasHistory ? previousRelativeViewProjection(historyViewProjection, historyOrigin, inputs.origin) : inputs.viewProjection;
    view.current = inputs.viewProjection;
    view.origin = inputs.origin;
    view.on = inputs.occlusion;
    view.history = hasHistory;
    const GpuMeshClusterOcclusion occlusion = packMeshClusterOcclusion(view, pyramidSize, history->getLevels());
    uploadBytes(allocator, frame.view, &inputs.view, sizeof(GpuCameraCull));
    const std::uint32_t noPlaces = 0;
    uploadBytes(allocator, frame.order, places > 0 ? static_cast<const void*>(inputs.order.data()) : &noPlaces,
                std::max<VkDeviceSize>(places, 1) * sizeof(std::uint32_t));
    uploadBytes(allocator, frame.setup, setup, sizeof(setup));

    if (frame.noPlaced.get() == VK_NULL_HANDLE) {
        const std::array<std::byte, noPlacedBytes> unused{};
        uploadBytes(allocator, frame.noPlaced, unused.data(), unused.size());
    }
    uploadBytes(allocator, frame.occlusionRecord, &occlusion, sizeof(occlusion));

    // The pyramids are bound whether built or not; one not built is never read (no history, or the early list only).
    makeReadable(commands, *history, historyBuilt);
    makeReadable(commands, *earlyDepth, earlyDepthBuilt);

    // The dispatch's y and z; the count pass writes its x. The previous frame in flight's indirect reads finished long ago.
    const std::uint32_t started[dispatchWords] = {0, 1, 1};
    bufferBarrier(commands, frame.dispatch.get(), indirectRead, transferWrite);
    vkCmdUpdateBuffer(commands, frame.dispatch.get(), 0, sizeof(started), started);
    bufferBarrier(commands, frame.dispatch.get(), transferWrite, computeWrite);

    // The camera culling's entries, last made ready for the vertex shaders, read here too.
    bufferBarrier(commands, inputs.drawList, computeWrite, computeRead);

    // 1. The pairs to test: each place's end, the work, the dispatch's blocks (both lists').
    bindComputeSet(commands, device.getDevice(), descriptors, *countPass, passBindings(frame, {frame.dispatch.get()}, false));
    pushList(commands, *countPass, MESH_CLUSTER_EARLY, inputs.bones.stamp);
    vkCmdDispatch(commands, 1, 1, 1);

    for (VkBuffer buffer : {frame.workEnds.get(), frame.work.get()}) {
        bufferBarrier(commands, buffer, computeWrite, computeRead);
    }

    bufferBarrier(commands, frame.dispatch.get(), computeWrite, indirectRead);
    recordList(commands, descriptors, frame, MESH_CLUSTER_EARLY);

    if (!inputs.occlusion) {
        readBack(commands, frame);
    }
}

/*
* 2. Each block's survivors (indirect: the work's blocks); 3. the blocks' first places, the list's totals and runs;
* 4. each survivor at its place, its triangles (and candidates) added to the totals. The late list reuses the block sums
* the early list's passes read, and writes the totals, items and task commands the early list's draws read (other
* parts of them), so it waits for those reads first. Then the list and totals for the mesh shaders that draw them (and
* the copies back), the task commands for the indirect draws.
*/
void VulkanMeshClusterCulling::recordList(VkCommandBuffer commands, FrameDescriptors& descriptors, PerFrame& frame, std::uint32_t list) {
    BufferUse drawRead{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};

    if (device.supportsMeshShaders()) {
        drawRead.stages |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
        drawRead.access |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    }

    if (list == MESH_CLUSTER_LATE) {
        bufferBarrier(commands, frame.blockCounts.get(), computeRead, computeWrite);
        bufferBarrier(commands, frame.blockFirst.get(), computeRead, computeWrite);
        bufferBarrier(commands, frame.totals.get(), drawRead, computeReadWrite);
        bufferBarrier(commands, frame.items.get(), drawRead, computeWrite);
        bufferBarrier(commands, frame.taskCommands.get(), indirectRead, computeWrite);
    }

    // 2. Each block's survivors.
    bindComputeSet(commands, device.getDevice(), descriptors, *testPass, passBindings(frame, {frame.blockCounts.get()}, true));
    pushList(commands, *testPass, list, frame.inputs.bones.stamp);
    vkCmdDispatchIndirect(commands, frame.dispatch.get(), 0);
    bufferBarrier(commands, frame.blockCounts.get(), computeWrite, computeRead);

    // 3. The blocks' first places, the totals and runs. The totals' last copy (this frame in flight's previous frame) is
    // long finished.
    bindComputeSet(commands, device.getDevice(), descriptors, *scanPass,
                   passBindings(frame, {frame.blockCounts.get(), frame.blockFirst.get(), frame.totals.get(), frame.taskCommands.get()}, false));
    pushList(commands, *scanPass, list, frame.inputs.bones.stamp);
    vkCmdDispatch(commands, 1, 1, 1);
    bufferBarrier(commands, frame.blockFirst.get(), computeWrite, computeRead);
    bufferBarrier(commands, frame.totals.get(), computeWrite, computeReadWrite);

    // 4. Each survivor at its place, their triangles added to the totals.
    bindComputeSet(commands, device.getDevice(), descriptors, *writePass,
                   passBindings(frame, {frame.blockFirst.get(), frame.items.get(), frame.totals.get()}, true));
    pushList(commands, *writePass, list, frame.inputs.bones.stamp);
    vkCmdDispatchIndirect(commands, frame.dispatch.get(), 0);

    bufferBarrier(commands, frame.totals.get(), computeWrite, drawRead);
    bufferBarrier(commands, frame.items.get(), computeWrite, drawRead);
    bufferBarrier(commands, frame.taskCommands.get(), computeWrite, indirectRead);
}

// This frame's pyramid from the early list's depth, then the late list against it; both lists copied back.
void VulkanMeshClusterCulling::cullLate(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& depth) {
    PerFrame& frame = frames.at(frameIndex);

    if (!frame.occlusion) {
        throw std::logic_error("Mesh cluster culling's late list needs occlusion on in cull");
    }

    earlyDepth->build(commands, descriptors, depth);
    earlyDepthBuilt = true;
    recordList(commands, descriptors, frame, MESH_CLUSTER_LATE);
    readBack(commands, frame);
}

// The whole frame's depth as the next frame's history, with this frame's view-projection and camera.
void VulkanMeshClusterCulling::keepDepth(VkCommandBuffer commands, FrameDescriptors& descriptors, VulkanImage& depth) {
    history->build(commands, descriptors, depth);
    historyBuilt = true;
    historyKept = true;
    historyCull = culls;
    historyViewProjection = frameViewProjection;
    historyOrigin = frameOrigin;
}

// Both lists' totals, and the lists when asked (the totals and items made ready for transfer by recordList).
void VulkanMeshClusterCulling::readBack(VkCommandBuffer commands, PerFrame& frame) {
    const VkBufferCopy totalCopy{0, 0, totalBytes};
    vkCmdCopyBuffer(commands, frame.totals.get(), frame.readback.get(), 1, &totalCopy);

    if (frame.itemsPending) {
        const VkBufferCopy itemCopy{0, totalBytes, MESH_CLUSTER_LISTS * VkDeviceSize{frame.room} * sizeof(GpuMeshClusterItem)};
        vkCmdCopyBuffer(commands, frame.items.get(), frame.readback.get(), 1, &itemCopy);
    }

    bufferBarrier(commands, frame.readback.get(), transferWrite, hostRead);
    frame.pending = true;
}

/*
* A list that overflowed its room grows the room to the next power of two holding it, for the frames after (each frame in
* flight's lists grow when they are next used). Without occlusion the late list's totals were never written: zero.
*/
std::optional<VulkanMeshClusterCulling::Collected> VulkanMeshClusterCulling::collect(std::uint32_t frameIndex) {
    PerFrame& frame = frames.at(frameIndex);

    if (!frame.pending) {
        return std::nullopt;
    }

    frame.pending = false;
    frame.readback.invalidate();
    const auto* words = static_cast<const std::uint32_t*>(frame.readback.getMapped());
    Collected collected;
    collected.occlusion = frame.occlusion;

    for (std::uint32_t list = 0; list < (frame.occlusion ? MESH_CLUSTER_LISTS : 1u); ++list) {
        std::memcpy(collected.totals[list].data(), words + list * MESH_CLUSTER_TOTAL_WORDS, MESH_CLUSTER_TOTAL_WORDS * sizeof(std::uint32_t));
        room = std::min(std::max(room, std::bit_ceil(collected.totals[list][MESH_CLUSTER_TOTAL_KEPT])), largestMeshClusterRoom);
    }

    if (frame.itemsPending) {
        frame.itemsPending = false;
        const auto* items = reinterpret_cast<const GpuMeshClusterItem*>(words + MESH_CLUSTER_LISTS * MESH_CLUSTER_TOTAL_WORDS);

        for (std::uint32_t list = 0; list < (frame.occlusion ? MESH_CLUSTER_LISTS : 1u); ++list) {
            const GpuMeshClusterItem* first = items + VkDeviceSize{list} * frame.room;
            collected.items[list].assign(first, first + std::min(collected.totals[list][MESH_CLUSTER_TOTAL_KEPT], frame.room));
        }

        collected.itemsRead = true;
    }

    return collected;
}
