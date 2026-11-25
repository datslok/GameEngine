#include "render/vulkan/vulkan_liana_trees.h"

#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/vulkan_device.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace {
    constexpr std::uint32_t pushBytes = 32;

    // The motion pass's push constants: the anchors (liana_motion.comp's LianaMotionPush).
    constexpr std::uint32_t motionPushBytes = sizeof(std::uint32_t);

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
    constexpr BufferUse computeRead{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};

    // What reads the motions after the motion pass: the mesh cluster passes (compute), the copy back.
    constexpr BufferUse motionsRead{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT};

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

VulkanLianaTrees::VulkanLianaTrees(const VulkanDevice& device, VkDescriptorSetLayout textures, VkDescriptorSetLayout scene, std::uint32_t framesInFlight):
    device(device)
{
    ComputePipelineDescription description;
    description.shader = "assets/shaders/liana_trees.comp.spv";
    description.storageBuffers = LIANA_TREES_BINDINGS;
    description.pushConstantBytes = pushBytes;
    pass = std::make_unique<ComputePipeline>(device, description);
    ComputePipelineDescription motion;
    motion.shader = "assets/shaders/liana_motion.comp.spv";
    motion.storageBuffers = 1;
    motion.pushConstantBytes = motionPushBytes;
    motion.sharedSets = {textures, scene};
    motionPass = std::make_unique<ComputePipeline>(device, motion);
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
    motions = VulkanBuffer{device.getAllocator(), bytes * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Gpu};
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

VkBuffer VulkanLianaTrees::getMotions() const {
    return motions.get();
}

/*
* The motions waited on (every earlier frame's reads), the found plants (written by record) made ready for this
* compute pass too, the pass a thread an anchor, then the motions made ready for the cluster passes and the copy back.
*/
void VulkanLianaTrees::recordMotions(VkCommandBuffer commands, FrameDescriptors& descriptors, VkDescriptorSet textures, VkDescriptorSet scene,
                                     std::uint32_t anchors, std::uint32_t frameIndex, bool readBack) {
    if (anchors == 0 || motions.get() == VK_NULL_HANDLE) {
        return;
    }

    PerFrame& frame = frames.at(frameIndex);
    const std::uint32_t moved = std::min(anchors, static_cast<std::uint32_t>(motions.getSize() / sizeof(std::array<float, 4>)));
    bufferBarrier(commands, motions.get(), anyEarlierUse, computeWrite);
    bufferBarrier(commands, found.get(), computeWrite, computeRead);
    const std::vector<ComputeBinding> bindings{storageBufferBinding(motions.get())};
    const std::array<VkDescriptorSet, 2> shared{textures, scene};
    bindComputeSet(commands, device.getDevice(), descriptors, *motionPass, bindings);
    bindSharedSets(commands, *motionPass, shared);
    vkCmdPushConstants(commands, motionPass->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, motionPushBytes, &moved);
    vkCmdDispatch(commands, (moved + LIANA_TREES_GROUP_SIZE - 1) / LIANA_TREES_GROUP_SIZE, 1, 1);
    bufferBarrier(commands, motions.get(), computeWrite, motionsRead);
    frame.motionsPending = readBack;
    frame.motionAnchors = moved;

    if (readBack) {
        const VkDeviceSize bytes = VkDeviceSize{moved} * sizeof(std::array<float, 4>);
        ensureBuffer(device.getAllocator(), frame.motionsReadback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback);
        const VkBufferCopy copy{0, 0, bytes};
        vkCmdCopyBuffer(commands, motions.get(), frame.motionsReadback.get(), 1, &copy);
    }
}

std::vector<std::array<float, 4>> VulkanLianaTrees::collectMotions(std::uint32_t frameIndex) {
    PerFrame& frame = frames.at(frameIndex);

    if (!frame.motionsPending) {
        return {};
    }

    frame.motionsPending = false;
    frame.motionsReadback.invalidate();
    const auto* read = static_cast<const std::array<float, 4>*>(frame.motionsReadback.getMapped());
    return std::vector<std::array<float, 4>>(read, read + frame.motionAnchors);
}
