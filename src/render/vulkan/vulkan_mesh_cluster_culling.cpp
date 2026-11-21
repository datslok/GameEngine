#include "render/vulkan/vulkan_mesh_cluster_culling.h"
#include "render/vulkan/buffer_barrier.h"
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
    constexpr BufferUse transferRead{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
    constexpr BufferUse transferWrite{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    constexpr BufferUse hostRead{VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
    constexpr BufferUse indirectRead{VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};

    // The dispatch's three words: x the blocks (written by the count pass), y and z 1.
    constexpr std::uint32_t dispatchWords = 3;

    ComputePipelineDescription passDescription(const char* shader, std::uint32_t buffers) {
        ComputePipelineDescription description;
        description.shader = shader;
        description.storageBuffers = buffers;
        return description;
    }

    // An upload buffer holding the bytes, written and flushed.
    void uploadBytes(VmaAllocator allocator, VulkanBuffer& buffer, const void* bytes, VkDeviceSize size) {
        ensureBuffer(allocator, buffer, size, storage, BufferMemory::Upload);
        std::memcpy(buffer.getMapped(), bytes, size);
        buffer.flush();
    }
}

VulkanMeshClusterCulling::VulkanMeshClusterCulling(const VulkanDevice& device, std::uint32_t framesInFlight):
    device(device)
{
    countPass = std::make_unique<ComputePipeline>(device, passDescription("assets/shaders/mesh_cluster_count.comp.spv", 12));
    testPass = std::make_unique<ComputePipeline>(device, passDescription("assets/shaders/mesh_cluster_test.comp.spv", 12));
    scanPass = std::make_unique<ComputePipeline>(device, passDescription("assets/shaders/mesh_cluster_scan.comp.spv", 15));
    writePass = std::make_unique<ComputePipeline>(device, passDescription("assets/shaders/mesh_cluster_write.comp.spv", 14));
    setFramesInFlight(framesInFlight);
}

void VulkanMeshClusterCulling::setFramesInFlight(std::uint32_t count) {
    frames.clear();
    frames.resize(count);
}

std::uint32_t VulkanMeshClusterCulling::getRoom() const {
    return room;
}

VkBuffer VulkanMeshClusterCulling::getTaskCommands(std::uint32_t frameIndex) const {
    return frames.at(frameIndex).taskCommands.get();
}

/*
* The list sized by the room as it stands (the room grows only between frames, in collect), the totals and the task
* commands; the list and totals reached by address from the mesh shaders.
*/
VulkanMeshClusterCulling::ListAddresses VulkanMeshClusterCulling::prepare(std::uint32_t frameIndex) {
    PerFrame& frame = frames.at(frameIndex);
    const VmaAllocator allocator = device.getAllocator();
    const VkBufferUsageFlags readable = storage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    frame.room = room;
    ensureBuffer(allocator, frame.items, VkDeviceSize{frame.room} * sizeof(GpuMeshClusterItem), readable, BufferMemory::Gpu);
    ensureBuffer(allocator, frame.totals, MESH_CLUSTER_TOTAL_WORDS * sizeof(std::uint32_t), readable, BufferMemory::Gpu);
    ensureBuffer(allocator, frame.taskCommands, MESH_CLUSTER_STYLES * 3 * sizeof(std::uint32_t), storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                 BufferMemory::Gpu);
    return ListAddresses{frame.items.getDeviceAddress(), frame.totals.getDeviceAddress()};
}

/*
* The frame in flight's buffers sized (the block sums by the bound on pairs, the list by the room), its view and setup
* written and its dispatch's y and z set; then the four passes, each waiting for the one before's writes. The camera
* culling's draw list was last made ready for the vertex shaders, so it is made ready for these passes too. Afterwards
* the totals (and the list, when asked) are copied for the CPU.
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

    ensureBuffer(allocator, frame.workEnds, VkDeviceSize{std::max(inputs.pooled, 1u)} * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    ensureBuffer(allocator, frame.work, MESH_CLUSTER_WORK_WORDS * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    ensureBuffer(allocator, frame.dispatch, dispatchWords * sizeof(std::uint32_t),
                 storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Gpu);
    ensureBuffer(allocator, frame.blockCounts, VkDeviceSize{std::max(blockRoom, 1u)} * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    ensureBuffer(allocator, frame.blockFirst, (VkDeviceSize{blockRoom} + 1) * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    const VkDeviceSize itemBytes = VkDeviceSize{frame.room} * sizeof(GpuMeshClusterItem);
    const VkDeviceSize totalBytes = MESH_CLUSTER_TOTAL_WORDS * sizeof(std::uint32_t);
    ensureBuffer(allocator, frame.readback, totalBytes + (readItems ? itemBytes : 0), VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback);

    const std::uint32_t setup[4] = {inputs.pooled, blockRoom, frame.room, 0};
    uploadBytes(allocator, frame.view, &inputs.view, sizeof(GpuCameraCull));
    uploadBytes(allocator, frame.setup, setup, sizeof(setup));

    // The dispatch's y and z; the count pass writes its x. The previous frame in flight's indirect reads finished long ago.
    const std::uint32_t started[dispatchWords] = {0, 1, 1};
    bufferBarrier(commands, frame.dispatch.get(), indirectRead, transferWrite);
    vkCmdUpdateBuffer(commands, frame.dispatch.get(), 0, sizeof(started), started);
    bufferBarrier(commands, frame.dispatch.get(), transferWrite, computeWrite);

    // The camera culling's entries, last made ready for the vertex shaders, read here too.
    bufferBarrier(commands, inputs.drawList, computeWrite, computeRead);

    ComputeBinding instances = storageBufferBinding(inputs.instances.buffer);
    instances.buffer = inputs.instances;
    const std::array<ComputeBinding, 11> shared{
        storageBufferBinding(frame.view.get()),    storageBufferBinding(inputs.order),   storageBufferBinding(inputs.groups),
        storageBufferBinding(inputs.commands),     storageBufferBinding(frame.workEnds.get()), storageBufferBinding(frame.work.get()),
        storageBufferBinding(inputs.drawList),     instances,                            storageBufferBinding(inputs.records),
        storageBufferBinding(inputs.clusters),     storageBufferBinding(frame.setup.get()),
    };

    // A pass's bindings: the shared ones, then its own.
    const auto bindings = [&](std::initializer_list<VkBuffer> own) {
        std::vector<ComputeBinding> all(shared.begin(), shared.end());

        for (VkBuffer buffer : own) {
            all.push_back(storageBufferBinding(buffer));
        }

        return all;
    };

    // 1. The pairs to test: each place's end, the work, the dispatch's blocks.
    bindComputeSet(commands, device.getDevice(), descriptors, *countPass, bindings({frame.dispatch.get()}));
    vkCmdDispatch(commands, 1, 1, 1);

    for (VkBuffer buffer : {frame.workEnds.get(), frame.work.get()}) {
        bufferBarrier(commands, buffer, computeWrite, computeRead);
    }

    bufferBarrier(commands, frame.dispatch.get(), computeWrite, indirectRead);

    // 2. Each block's survivors (indirect: the work's blocks).
    bindComputeSet(commands, device.getDevice(), descriptors, *testPass, bindings({frame.blockCounts.get()}));
    vkCmdDispatchIndirect(commands, frame.dispatch.get(), 0);
    bufferBarrier(commands, frame.blockCounts.get(), computeWrite, computeRead);

    // 3. The blocks' first places, the totals and runs. The totals' last copy (this frame in flight's previous frame) is
    // long finished.
    bindComputeSet(commands, device.getDevice(), descriptors, *scanPass, bindings({frame.blockCounts.get(), frame.blockFirst.get(), frame.totals.get(), frame.taskCommands.get()}));
    vkCmdDispatch(commands, 1, 1, 1);
    bufferBarrier(commands, frame.blockFirst.get(), computeWrite, computeRead);
    bufferBarrier(commands, frame.totals.get(), computeWrite, computeReadWrite);

    // 4. Each survivor at its place, their triangles added to the totals.
    bindComputeSet(commands, device.getDevice(), descriptors, *writePass, bindings({frame.blockFirst.get(), frame.items.get(), frame.totals.get()}));
    vkCmdDispatchIndirect(commands, frame.dispatch.get(), 0);

    // The list and totals for the mesh shaders that draw them (while the GPU has them) and the copies back; the task
    // commands, finished by the scan, for the indirect draws.
    BufferUse read{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};

    if (device.supportsMeshShaders()) {
        read.stages |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
        read.access |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    }

    bufferBarrier(commands, frame.totals.get(), computeWrite, read);
    bufferBarrier(commands, frame.items.get(), computeWrite, read);
    bufferBarrier(commands, frame.taskCommands.get(), computeWrite, indirectRead);

    // Read back: the totals, and the list when asked.
    const VkBufferCopy totalCopy{0, 0, totalBytes};
    vkCmdCopyBuffer(commands, frame.totals.get(), frame.readback.get(), 1, &totalCopy);
    frame.itemsPending = readItems;

    if (readItems) {
        const VkBufferCopy itemCopy{0, totalBytes, itemBytes};
        vkCmdCopyBuffer(commands, frame.items.get(), frame.readback.get(), 1, &itemCopy);
    }

    bufferBarrier(commands, frame.readback.get(), transferWrite, hostRead);
    frame.pending = true;
}

/*
* A list that overflowed its room grows the room to the next power of two holding it, for the frames after (each frame in
* flight's list grows when it is next used).
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
    std::memcpy(collected.totals.data(), words, MESH_CLUSTER_TOTAL_WORDS * sizeof(std::uint32_t));
    room = std::min(std::max(room, std::bit_ceil(collected.totals[MESH_CLUSTER_TOTAL_KEPT])), largestMeshClusterRoom);

    if (frame.itemsPending) {
        frame.itemsPending = false;
        const auto* items = reinterpret_cast<const GpuMeshClusterItem*>(words + MESH_CLUSTER_TOTAL_WORDS);
        collected.items.assign(items, items + std::min(collected.totals[MESH_CLUSTER_TOTAL_KEPT], frame.room));
        collected.itemsRead = true;
    }

    return collected;
}
