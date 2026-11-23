#include "render/vulkan/vulkan_liana_trees.h"

#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/vulkan_device.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace {
    constexpr std::uint32_t pushBytes = 32;

    struct LianaTreesPush {
        float eye[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float fadeEnd = 0.0f;
        std::uint32_t layerCount = 0;
        std::uint32_t anchorCount = 0;
        std::uint32_t unused = 0;
    };

    static_assert(sizeof(LianaTreesPush) == pushBytes, "LianaTreesPush must match liana_trees.comp's push constants");

    constexpr BufferUse computeWrite{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    constexpr BufferUse transferRead{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};

    // Whatever an earlier frame did with the found plants: the lianas' vertex and mesh shaders read them, a copy back.
    constexpr BufferUse anyEarlierUse{VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT};

    // What reads the found plants after the pass: the shadows', prepass's and main pass's vertex and mesh shaders, the copy back.
    BufferUse foundRead(const VulkanDevice& device) {
        BufferUse use{VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT};

        if (device.supportsMeshShaders()) {
            use.stages |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
        }

        return use;
    }

    template <typename Record>
    void uploadRecords(VmaAllocator allocator, VulkanBuffer& buffer, const std::vector<Record>& records) {
        const Record none{};
        uploadBytes(allocator, buffer, records.empty() ? &none : records.data(), std::max<std::size_t>(records.size(), 1) * sizeof(Record));
    }
}

VulkanLianaTrees::VulkanLianaTrees(const VulkanDevice& device, std::uint32_t framesInFlight): device(device) {
    ComputePipelineDescription description;
    description.shader = "assets/shaders/liana_trees.comp.spv";
    description.storageBuffers = LIANA_TREES_BINDINGS;
    description.pushConstantBytes = pushBytes;
    pass = std::make_unique<ComputePipeline>(device, description);
    setFramesInFlight(framesInFlight);
}

void VulkanLianaTrees::setFramesInFlight(std::uint32_t count) {
    frames.clear();
    frames.resize(count);
}

// A power of two of words at or above the ask, so a few more lianas held do not make it again.
void VulkanLianaTrees::prepare(std::uint32_t anchors) {
    const VkDeviceSize bytes = VkDeviceSize{std::bit_ceil(std::max(anchors, 64u))} * sizeof(std::uint32_t);

    if (found.get() != VK_NULL_HANDLE && found.getSize() >= bytes) {
        return;
    }

    if (found.get() != VK_NULL_HANDLE) {
        device.waitIdle();
    }

    found = VulkanBuffer{device.getAllocator(), bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Gpu};
}

/*
* The frame's tables written, the found plants waited on (every earlier frame's reads), the pass a thread an anchor,
* then the plants made ready for the shaders and the copy back; for bones-compare the counts copied back too.
*/
void VulkanLianaTrees::record(VkCommandBuffer commands, FrameDescriptors& descriptors, const Inputs& inputs, std::uint32_t frameIndex) {
    if (inputs.anchors == 0 || found.get() == VK_NULL_HANDLE) {
        return;
    }

    PerFrame& frame = frames.at(frameIndex);
    const VmaAllocator allocator = device.getAllocator();
    const std::uint32_t anchors = std::min(inputs.anchors, static_cast<std::uint32_t>(found.getSize() / sizeof(std::uint32_t)));
    frame.tables = packLianaTreeTables(inputs.sets, inputs.fieldLayers);
    uploadRecords(allocator, frame.layers, frame.tables.layers);
    uploadRecords(allocator, frame.cells, frame.tables.cells);
    bufferBarrier(commands, found.get(), anyEarlierUse, computeWrite);

    const std::vector<ComputeBinding> bindings{storageBufferBinding(inputs.records),      storageBufferBinding(inputs.plants),
                                               storageBufferBinding(inputs.counts),       storageBufferBinding(frame.layers.get()),
                                               storageBufferBinding(frame.cells.get()),   storageBufferBinding(found.get())};
    const LianaTreesPush push{.eye = {inputs.eye.x, inputs.eye.y, inputs.eye.z, 0.0f},
                              .fadeEnd = inputs.fadeEnd,
                              .layerCount = static_cast<std::uint32_t>(frame.tables.layers.size()),
                              .anchorCount = anchors};
    bindComputeSet(commands, device.getDevice(), descriptors, *pass, bindings);
    vkCmdPushConstants(commands, pass->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes, &push);
    vkCmdDispatch(commands, (anchors + LIANA_TREES_GROUP_SIZE - 1) / LIANA_TREES_GROUP_SIZE, 1, 1);
    bufferBarrier(commands, found.get(), computeWrite, foundRead(device));
    frame.pending = inputs.readBack;
    frame.anchors = anchors;

    if (inputs.readBack) {
        const VkDeviceSize foundBytes = VkDeviceSize{anchors} * sizeof(std::uint32_t);
        // The counts were written by the planting's passes and read by the placed ones and this: copied back after them.
        const BufferUse countsUsed{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
        bufferBarrier(commands, inputs.counts, countsUsed, transferRead);
        ensureBuffer(allocator, frame.foundReadback, foundBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback);
        ensureBuffer(allocator, frame.countsReadback, inputs.countsBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback);
        const VkBufferCopy foundCopy{0, 0, foundBytes};
        const VkBufferCopy countsCopy{0, 0, inputs.countsBytes};
        vkCmdCopyBuffer(commands, found.get(), frame.foundReadback.get(), 1, &foundCopy);
        vkCmdCopyBuffer(commands, inputs.counts, frame.countsReadback.get(), 1, &countsCopy);
        // Back to the shaders' reads (the next frame's planting writes them again after its own barrier).
        bufferBarrier(commands, inputs.counts, transferRead, countsUsed);
        frame.countsBytes = inputs.countsBytes;
    }
}

std::optional<VulkanLianaTrees::Collected> VulkanLianaTrees::collect(std::uint32_t frameIndex) {
    PerFrame& frame = frames.at(frameIndex);

    if (!frame.pending) {
        return std::nullopt;
    }

    frame.pending = false;
    frame.foundReadback.invalidate();
    frame.countsReadback.invalidate();
    Collected collected;
    collected.tables = frame.tables;
    const auto* readFound = static_cast<const std::uint32_t*>(frame.foundReadback.getMapped());
    const auto* readCounts = static_cast<const std::uint32_t*>(frame.countsReadback.getMapped());
    collected.found.assign(readFound, readFound + frame.anchors);
    collected.counts.assign(readCounts, readCounts + frame.countsBytes / sizeof(std::uint32_t));
    return collected;
}

VkBuffer VulkanLianaTrees::getFound() const {
    return found.get();
}
