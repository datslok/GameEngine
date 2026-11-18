#include "render/vulkan/vulkan_occlusion_culling.h"
#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/vulkan_device.h"

#include <algorithm>
#include <array>
#include <cstring>

VulkanOcclusionCulling::VulkanOcclusionCulling(const VulkanDevice& device, VkSampler nearest, VkDescriptorSetLayout texturesLayout,
                                               VkDescriptorSetLayout sceneLayout, std::uint32_t framesInFlight):
    device(device),
    nearest(nearest),
    counts(device.getAllocator(), sizeof(GpuOcclusionCounts),
           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Gpu)
{
    pyramid = std::make_unique<VulkanDepthPyramid>(device, nearest, DepthReduction::Farthest, "Occlusion pyramid");

    ComputePipelineDescription description;
    description.shader = "assets/shaders/occlusion_count.comp.spv";
    description.sharedSets = {texturesLayout, sceneLayout};
    description.storageBuffers = 2;
    description.sampledImages = 1;
    description.pushConstantBytes = sizeof(GpuOcclusionPass);
    pipeline = std::make_unique<ComputePipeline>(device, description);

    setFramesInFlight(framesInFlight);
}

void VulkanOcclusionCulling::setFramesInFlight(std::uint32_t count) {
    frames.clear();

    for (std::uint32_t frame = 0; frame < count; ++frame) {
        frames.push_back(PerFrame{
            VulkanBuffer{},
            VulkanBuffer{device.getAllocator(), sizeof(GpuOcclusionCounts), VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback},
            false});
    }
}

void VulkanOcclusionCulling::ensureSize(RenderSize render) {
    pyramid->ensureSize(render);
}

/*
* The pyramid, then the counts: cleared, added into by every workgroup, copied for the CPU. The counts buffer in GPU
* memory is shared by the frames in flight, so barriers order its uses across frames too, as the depth range does:
* last frame's copy before the clear, the clear before the atomics, the atomics before the copy, the copy before the
* host reads it. The batch table is written by the CPU into this frame's upload buffer before the commands are submitted.
*/
void VulkanOcclusionCulling::count(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& depth,
                                   VkDescriptorSet textures, VkDescriptorSet scene, const std::vector<GpuOcclusionBatch>& batches,
                                   const OcclusionView& view, const Vec3& cameraOrigin) {
    PerFrame& frame = frames.at(frameIndex);
    const VkDeviceSize batchBytes = std::max<VkDeviceSize>(batches.size(), 1) * sizeof(GpuOcclusionBatch);

    if (frame.batches.getSize() < batchBytes) {
        frame.batches = VulkanBuffer{device.getAllocator(), std::max(batchBytes, 2 * frame.batches.getSize()), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                     BufferMemory::Upload};
    }

    if (!batches.empty()) {
        std::memcpy(frame.batches.getMapped(), batches.data(), batches.size() * sizeof(GpuOcclusionBatch));
        frame.batches.flush();
    }

    pyramid->build(commands, descriptors, depth);

    bufferBarrier(commands, counts.get(), {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_NONE},
                  {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT});
    const GpuOcclusionCounts empty;
    vkCmdUpdateBuffer(commands, counts.get(), 0, sizeof(empty), &empty);
    bufferBarrier(commands, counts.get(), {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
                  {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});

    const std::array<VkDescriptorSet, 2> sharedSets{textures, scene};
    bindSharedSets(commands, *pipeline, sharedSets);
    const std::array<ComputeBinding, 3> bindings{
        storageBufferBinding(frame.batches.get()),
        storageBufferBinding(counts.get()),
        sampledBinding(nearest, pyramid->getImage().getView()),
    };
    bindComputeSet(commands, device.getDevice(), descriptors, *pipeline, bindings);

    const GpuOcclusionPass pass = packOcclusionPass(view, cameraOrigin, pyramid->getLevels(), static_cast<std::uint32_t>(batches.size()));
    vkCmdPushConstants(commands, pipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pass), &pass);

    if (!batches.empty()) {
        vkCmdDispatch(commands, std::min(static_cast<std::uint32_t>(batches.size()), occlusionMaxGroups), 1, 1);
    }

    bufferBarrier(commands, counts.get(), {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
                  {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT});

    VkBufferCopy copy{};
    copy.size = sizeof(GpuOcclusionCounts);
    vkCmdCopyBuffer(commands, counts.get(), frame.readback.get(), 1, &copy);

    bufferBarrier(commands, frame.readback.get(), {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
                  {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT});
    frame.pending = true;
}

std::optional<OcclusionCounts> VulkanOcclusionCulling::collect(std::uint32_t frameIndex) {
    PerFrame& frame = frames.at(frameIndex);

    if (!frame.pending) {
        return std::nullopt;
    }

    frame.pending = false;
    frame.readback.invalidate();
    GpuOcclusionCounts measured;
    std::memcpy(&measured, frame.readback.getMapped(), sizeof(measured));
    return unpackOcclusionCounts(measured);
}
