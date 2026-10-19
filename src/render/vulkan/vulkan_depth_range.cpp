#include "render/vulkan/vulkan_depth_range.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/vulkan_device.h"

#include <array>
#include <cstring>

VulkanDepthRange::VulkanDepthRange(const VulkanDevice& device, VkSampler sampler, std::uint32_t framesInFlight):
    device(device),
    sampler(sampler),
    range(device.getAllocator(), sizeof(ScreenDepthRange),
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Gpu)
{
    ComputePipelineDescription description;
    description.shader = "assets/shaders/depth_range.comp.spv";
    description.storageBuffers = 1;
    description.sampledImages = 1;
    pipeline = std::make_unique<ComputePipeline>(device, description);

    setFramesInFlight(framesInFlight);
}

void VulkanDepthRange::setFramesInFlight(std::uint32_t count) {
    results.clear();

    for (std::uint32_t frame = 0; frame < count; ++frame) {
        results.push_back(Result{
            VulkanBuffer{device.getAllocator(), sizeof(ScreenDepthRange), VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback},
            false});
    }
}

/*
* The range starts empty, is reduced into by every workgroup, and is copied for the CPU. The buffer in GPU memory is
* shared by the frames in flight, so barriers order its uses, including across frames (a barrier waits for everything
* submitted before it):
* - before the reset: last frame's copy may still be reading it. A write after a read needs only that order.
* - reset to pass: the reset's write is made visible to the shader's atomics (which read and write).
* - pass to copy: the atomics' writes are made visible to the copy.
* - copy to CPU: the copy's writes are made visible to the host once the fence says the frame is done.
* The depth image goes from being the depth target to being read by compute shaders; next frame's depth pass discards
* it again, waiting only for this read.
*/
void VulkanDepthRange::measure(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& depth) {
    Result& result = results.at(frameIndex);
    const ImageDescription& size = depth.getDescription();

    bufferBarrier(commands, range.get(), {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_NONE},
                  {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT});
    const ScreenDepthRange empty;
    vkCmdUpdateBuffer(commands, range.get(), 0, sizeof(empty), &empty);
    bufferBarrier(commands, range.get(), {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
                  {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});

    depth.transition(commands, ImageState::DepthComputeRead);

    const VkDescriptorSet set = descriptors.allocate(pipeline->getResourceLayout());
    VkDescriptorBufferInfo bufferInfo{range.get(), 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo imageInfo{sampler, depth.getView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};

    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &bufferInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device.getDevice(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->get());
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->getLayout(), 0, 1, &set, 0, nullptr);

    const std::uint32_t pixelsPerGroup = depthRangeGroupSide * depthRangeTexelsPerSide;
    vkCmdDispatch(commands, workgroupCount(size.width, pixelsPerGroup), workgroupCount(size.height, pixelsPerGroup), 1);

    bufferBarrier(commands, range.get(), {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
                  {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT});

    VkBufferCopy copy{};
    copy.size = sizeof(ScreenDepthRange);
    vkCmdCopyBuffer(commands, range.get(), result.readback.get(), 1, &copy);

    bufferBarrier(commands, result.readback.get(), {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
                  {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT});
    result.pending = true;
}

std::optional<ScreenDepthRange> VulkanDepthRange::collect(std::uint32_t frameIndex) {
    Result& result = results.at(frameIndex);

    if (!result.pending) {
        return std::nullopt;
    }

    result.pending = false;
    result.readback.invalidate();
    ScreenDepthRange measured;
    std::memcpy(&measured, result.readback.getMapped(), sizeof(measured));
    return measured;
}
