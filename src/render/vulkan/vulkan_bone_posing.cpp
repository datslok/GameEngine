#include "render/vulkan/vulkan_bone_posing.h"

#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/vulkan_device.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace {
    constexpr VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    constexpr VkBufferUsageFlags copied = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    constexpr VkDeviceSize headerBytes = BONE_HEADER_WORDS * sizeof(std::uint32_t);
    constexpr std::uint32_t pushBytes = 32;

    struct BonePush {
        std::uint32_t stamp = 0;
        std::uint32_t poolBones = 0;
        float fadeEnd = 0.0f;
        std::uint32_t unused = 0;
        float eye[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    };

    static_assert(sizeof(BonePush) == pushBytes, "BonePush must match bone_pass.glsl's push constants");

    constexpr BufferUse computeWrite{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    constexpr BufferUse computeReadWrite{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    constexpr BufferUse transferWrite{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    constexpr BufferUse indirectAndCompute{VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                           VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};

    // Whatever an earlier frame did with the shared buffers (the passes, the shaders reading the poses, the copies back).
    constexpr BufferUse anyEarlierUse{VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
                                          VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
    constexpr BufferUse clearedOrPosed{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                       VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};

    // What reads the poses after the passes: the shadows', prepass's and main pass's vertex and mesh shaders, the
    // cluster passes (compute), the copy back.
    BufferUse posesRead(const VulkanDevice& device) {
        BufferUse use{VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT};

        if (device.supportsMeshShaders()) {
            use.stages |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
        }

        return use;
    }

    std::unique_ptr<ComputePipeline> passPipeline(const VulkanDevice& device, const char* shader, VkDescriptorSetLayout textures, VkDescriptorSetLayout scene) {
        ComputePipelineDescription description;
        description.shader = shader;
        description.storageBuffers = BONE_PASS_BINDINGS;
        description.pushConstantBytes = pushBytes;
        description.sharedSets = {textures, scene};
        return std::make_unique<ComputePipeline>(device, description);
    }

    template <typename Record>
    void uploadRecords(VmaAllocator allocator, VulkanBuffer& buffer, const std::vector<Record>& records) {
        const Record none{};
        uploadBytes(allocator, buffer, records.empty() ? &none : records.data(), std::max<std::size_t>(records.size(), 1) * sizeof(Record));
    }
}

VulkanBonePosing::VulkanBonePosing(const VulkanDevice& device, VkDescriptorSetLayout textures, VkDescriptorSetLayout scene, std::uint32_t framesInFlight):
    device(device),
    claimPass(passPipeline(device, "assets/shaders/bones_claim.comp.spv", textures, scene)),
    posePass(passPipeline(device, "assets/shaders/bones_pose.comp.spv", textures, scene))
{
    setFramesInFlight(framesInFlight);
}

void VulkanBonePosing::setFramesInFlight(std::uint32_t count) {
    frames.clear();
    frames.resize(count);
}

bool VulkanBonePosing::prepare(const PlacedTable& table, std::span<const DrawItem> plantedParts, std::uint32_t plants, const WindBoneMotion& motion,
                               std::uint32_t frameIndex) {
    if (table.skeletons.empty()) {
        return false;
    }

    const VmaAllocator allocator = device.getAllocator();
    PerFrame& frame = frames.at(frameIndex);
    ensureBuffer(allocator, frame.readback, headerBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback);
    groups = boneGroups(table);
    const std::vector<std::uint32_t> leanParts = skeletonLeanParts(table, plantedParts);

    // The pool starts at the setting and grows as frames ask (collect); changing either setting starts it again.
    if (motion.poolBones != poolSetting || motion.poolMostBones != poolMostSetting) {
        poolSetting = motion.poolBones;
        poolMostSetting = motion.poolMostBones;
        grownBones = static_cast<std::uint32_t>(motion.poolBones);
    }

    const std::uint32_t pool = grownBones;
    const VkDeviceSize plantTreeBytes = VkDeviceSize{std::max(plants, 1u)} * 2 * sizeof(std::uint32_t);
    const bool tablesChanged = table.skeletons != madeFrom || leanParts != madeLeanParts;
    const bool poolChanged = pool != poolBones || plantTrees.getSize() < plantTreeBytes;

    if (!tablesChanged && !poolChanged) {
        return true;
    }

    // Every frame in flight reads the tables and the shared buffers: none may be reading them while they are remade.
    if (header.get() != VK_NULL_HANDLE) {
        device.waitIdle();
    }

    if (tablesChanged) {
        tables = packSwaySkeletons(table.skeletons);

        for (std::size_t skeleton = 0; skeleton < tables.skeletons.size(); ++skeleton) {
            tables.skeletons[skeleton].leanPart = leanParts[skeleton];
        }

        uploadRecords(allocator, bones, tables.bones);
        uploadRecords(allocator, skeletons, tables.skeletons);
        uploadRecords(allocator, chains, tables.chains);
        uploadRecords(allocator, levelStarts, tables.levelStarts);
        madeFrom = table.skeletons;
        madeLeanParts = leanParts;
    }

    if (poolChanged) {
        header = VulkanBuffer{allocator, headerBytes, storage | copied | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, BufferMemory::Gpu};
        trees = VulkanBuffer{allocator, VkDeviceSize{boneMostTrees} * sizeof(GpuBoneTree), storage | copied, BufferMemory::Gpu};
        plantTrees = VulkanBuffer{allocator, plantTreeBytes, storage | copied, BufferMemory::Gpu};
        poses = VulkanBuffer{allocator, VkDeviceSize{pool} * 2 * sizeof(GpuBonePose), storage | copied, BufferMemory::Gpu};
        plantTreesCleared = false;
        poolBones = pool;
    }

    return true;
}

/*
* The work list uploaded; the shared buffers waited on (every earlier frame's use), the plant table cleared once after
* it is made, the header's counts cleared and its dispatch set to no trees; then the claim pass, the pose pass
* (indirect: the claims raised its dispatch), and the poses made ready for the shaders and the copy back.
*/
void VulkanBonePosing::record(VkCommandBuffer commands, FrameDescriptors& descriptors, const Inputs& inputs, std::uint32_t frameIndex) {
    if (inputs.swayingViews.empty() || groups.empty()) {
        return;
    }

    PerFrame& frame = frames.at(frameIndex);
    const VmaAllocator allocator = device.getAllocator();
    std::vector<std::uint32_t> work{static_cast<std::uint32_t>(inputs.swayingViews.size()), static_cast<std::uint32_t>(groups.size()), inputs.groupsAView, 0};
    work.insert(work.end(), inputs.swayingViews.begin(), inputs.swayingViews.end());
    work.insert(work.end(), groups.begin(), groups.end());
    uploadBytes(allocator, frame.work, work.data(), work.size() * sizeof(std::uint32_t));

    for (const VulkanBuffer* shared : {&header, &trees, &plantTrees, &poses}) {
        bufferBarrier(commands, shared->get(), anyEarlierUse, clearedOrPosed);
    }

    if (!plantTreesCleared) {
        vkCmdFillBuffer(commands, plantTrees.get(), 0, VK_WHOLE_SIZE, 0);
        bufferBarrier(commands, plantTrees.get(), transferWrite, computeReadWrite);
        plantTreesCleared = true;
    }

    std::uint32_t words[BONE_HEADER_WORDS] = {};
    words[BONE_HEADER_DISPATCH + 1] = 1;
    words[BONE_HEADER_DISPATCH + 2] = 1;
    vkCmdUpdateBuffer(commands, header.get(), 0, headerBytes, words);
    bufferBarrier(commands, header.get(), transferWrite, computeReadWrite);

    // A frame's stamp is never 0, the plant table's cleared value; the draws were given it (getNextStamp).
    stamp = boneNextStamp(stamp);
    BonePush push{.stamp = stamp, .poolBones = poolBones, .fadeEnd = inputs.fadeEnd, .eye = {inputs.eye.x, inputs.eye.y, inputs.eye.z, 0.0f}};

    std::vector<ComputeBinding> bindings{
        storageBufferBinding(inputs.commands), storageBufferBinding(inputs.entries), storageBufferBinding(inputs.parts),
        storageBufferBinding(bones.get()),     storageBufferBinding(skeletons.get()), storageBufferBinding(chains.get()),
        storageBufferBinding(levelStarts.get()), storageBufferBinding(frame.work.get()), storageBufferBinding(header.get()),
        storageBufferBinding(trees.get()),     storageBufferBinding(plantTrees.get()), storageBufferBinding(poses.get()),
        storageBufferBinding(inputs.plants),   storageBufferBinding(inputs.partInstances.buffer),
    };
    bindings[BONE_PART_INSTANCES_BINDING].buffer = inputs.partInstances;
    const std::array<VkDescriptorSet, 2> shared{inputs.textures, inputs.scene};

    bindComputeSet(commands, device.getDevice(), descriptors, *claimPass, bindings);
    bindSharedSets(commands, *claimPass, shared);
    vkCmdPushConstants(commands, claimPass->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes, &push);
    vkCmdDispatch(commands, static_cast<std::uint32_t>(groups.size()), static_cast<std::uint32_t>(inputs.swayingViews.size()), 1);

    bufferBarrier(commands, header.get(), computeReadWrite, indirectAndCompute);
    bufferBarrier(commands, trees.get(), computeWrite, computeReadWrite);

    bindComputeSet(commands, device.getDevice(), descriptors, *posePass, bindings);
    bindSharedSets(commands, *posePass, shared);
    vkCmdPushConstants(commands, posePass->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes, &push);
    vkCmdDispatchIndirect(commands, header.get(), BONE_HEADER_DISPATCH * sizeof(std::uint32_t));

    const BufferUse then = posesRead(device);

    for (const VulkanBuffer* written : {&trees, &plantTrees, &poses}) {
        bufferBarrier(commands, written->get(), computeReadWrite, then);
    }

    bufferBarrier(commands, header.get(), indirectAndCompute, BufferUse{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT});
    const VkBufferCopy counts{0, 0, headerBytes};
    vkCmdCopyBuffer(commands, header.get(), frame.readback.get(), 1, &counts);
    frame.pending = true;
    frame.posesPending = inputs.readBackPoses;

    if (inputs.readBackPoses) {
        ensureBuffer(allocator, frame.treesReadback, trees.getSize(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback);
        ensureBuffer(allocator, frame.posesReadback, poses.getSize(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback);
        const VkBufferCopy treeCopy{0, 0, trees.getSize()};
        const VkBufferCopy poseCopy{0, 0, poses.getSize()};
        vkCmdCopyBuffer(commands, trees.get(), frame.treesReadback.get(), 1, &treeCopy);
        vkCmdCopyBuffer(commands, poses.get(), frame.posesReadback.get(), 1, &poseCopy);
    }
}

std::optional<VulkanBonePosing::Collected> VulkanBonePosing::collect(std::uint32_t frameIndex) {
    PerFrame& frame = frames.at(frameIndex);

    if (!frame.pending) {
        return std::nullopt;
    }

    frame.pending = false;
    frame.readback.invalidate();
    std::uint32_t words[BONE_HEADER_WORDS] = {};
    std::memcpy(words, frame.readback.getMapped(), headerBytes);
    Collected collected;
    collected.trees = std::min(words[BONE_HEADER_TREES], boneMostTrees);
    collected.still = words[BONE_HEADER_TREES_STILL];
    collected.bonesAsked = words[BONE_HEADER_BONES_ASKED];
    collected.pool = poolBones;

    // A frame that asked for more than the pool holds grows it for the next prepare (the trees past it stood still).
    grownBones = boneGrownPool(grownBones, collected.bonesAsked, static_cast<std::uint32_t>(poolMostSetting));

    if (frame.posesPending) {
        frame.posesPending = false;
        frame.treesReadback.invalidate();
        frame.posesReadback.invalidate();
        const auto* readTrees = static_cast<const GpuBoneTree*>(frame.treesReadback.getMapped());
        const auto* readPoses = static_cast<const GpuBonePose*>(frame.posesReadback.getMapped());
        collected.posedTrees.assign(readTrees, readTrees + collected.trees);
        collected.poses.assign(readPoses, readPoses + std::min<std::size_t>(std::size_t{poolBones} * 2, frame.posesReadback.getSize() / sizeof(GpuBonePose)));
    }

    return collected;
}

const SwaySkeletonTables& VulkanBonePosing::getTables() const {
    return tables;
}

std::uint32_t VulkanBonePosing::getNextStamp() const {
    return boneNextStamp(stamp);
}

VkBuffer VulkanBonePosing::getTrees() const {
    return trees.get();
}

VkBuffer VulkanBonePosing::getPlantTrees() const {
    return plantTrees.get();
}

VkBuffer VulkanBonePosing::getPoses() const {
    return poses.get();
}
