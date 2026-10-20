#include "render/vulkan/vulkan_eye_adaptation.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/vulkan_device.h"

#include <array>
#include <cstring>

namespace {
    constexpr VkDeviceSize histogramBytes = sizeof(std::uint32_t) * eyeHistogramBins;
}

VulkanEyeAdaptation::VulkanEyeAdaptation(const VulkanDevice& device, VkSampler sampler, std::uint32_t framesInFlight):
    device(device),
    sampler(sampler),
    // Copied from by the GPU tests, which read it back.
    histogram(device.getAllocator(), histogramBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
              BufferMemory::Gpu),
    adaptation(device.getAllocator(), sizeof(GpuEyeAdaptation),
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Gpu)
{
    ComputePipelineDescription histogramDescription;
    histogramDescription.shader = "assets/shaders/eye_histogram.comp.spv";
    histogramDescription.storageBuffers = 1;
    histogramDescription.sampledImages = 1;
    histogramPipeline = std::make_unique<ComputePipeline>(device, histogramDescription);

    ComputePipelineDescription adaptationDescription;
    adaptationDescription.shader = "assets/shaders/eye_adaptation.comp.spv";
    adaptationDescription.storageBuffers = 2;
    adaptationDescription.pushConstantBytes = sizeof(GpuEyeAdaptationStep);
    adaptationPipeline = std::make_unique<ComputePipeline>(device, adaptationDescription);

    setFramesInFlight(framesInFlight);
}

void VulkanEyeAdaptation::setFramesInFlight(std::uint32_t count) {
    readings.clear();

    for (std::uint32_t frame = 0; frame < count; ++frame) {
        readings.push_back(Reading{
            VulkanBuffer{device.getAllocator(), sizeof(GpuEyeAdaptation), VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback}, false});
    }
}

/*
* Both buffers in GPU memory are shared by the frames in flight, so barriers order their uses, across frames too (a
* barrier waits for everything submitted before it):
* - the histogram: last frame's adaptation pass read it, so its reset waits for that; the reset's zeros are made
*   visible to the histogram pass's atomics, and their counts to the adaptation pass.
* - the adapted exposure: last frame's tone mapping and copy read it, so this frame's adaptation pass waits for them; its
*   write is made visible to this frame's tone mapping and to the copy for the readout, and the copy's to the host.
* The scene image is already readable by compute shaders (the main pass leaves it so).
*/
void VulkanEyeAdaptation::adapt(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& scene,
                                const ToneMappingSettings& settings, float seconds, bool snap) {
    Reading& reading = readings.at(frameIndex);
    const ImageDescription& size = scene.getDescription();
    scene.transition(commands, ImageState::SurfaceRead);

    bufferBarrier(commands, histogram.get(), {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE},
                  {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT});
    vkCmdFillBuffer(commands, histogram.get(), 0, histogramBytes, 0);
    bufferBarrier(commands, histogram.get(), {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
                  {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});

    const VkDescriptorBufferInfo histogramInfo{histogram.get(), 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo adaptationInfo = describeAdaptation();

    {
        const VkDescriptorSet set = descriptors.allocate(histogramPipeline->getResourceLayout());
        const VkDescriptorImageInfo imageInfo{sampler, scene.getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        std::array<VkWriteDescriptorSet, 2> writes{};

        for (std::size_t index = 0; index < writes.size(); ++index) {
            writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index].dstSet = set;
            writes[index].dstBinding = static_cast<std::uint32_t>(index);
            writes[index].descriptorCount = 1;
        }

        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &histogramInfo;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device.getDevice(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, histogramPipeline->get());
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, histogramPipeline->getLayout(), 0, 1, &set, 0, nullptr);
        vkCmdDispatch(commands, workgroupCount(size.width, eyeHistogramGroupSide), workgroupCount(size.height, eyeHistogramGroupSide), 1);
    }

    bufferBarrier(commands, histogram.get(), {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
                  {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT});
    bufferBarrier(commands, adaptation.get(), {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_NONE},
                  {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});

    {
        const VkDescriptorSet set = descriptors.allocate(adaptationPipeline->getResourceLayout());
        std::array<VkWriteDescriptorSet, 2> writes{};
        const std::array<const VkDescriptorBufferInfo*, 2> buffers{&histogramInfo, &adaptationInfo};

        for (std::size_t index = 0; index < writes.size(); ++index) {
            writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index].dstSet = set;
            writes[index].dstBinding = static_cast<std::uint32_t>(index);
            writes[index].descriptorCount = 1;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[index].pBufferInfo = buffers[index];
        }

        vkUpdateDescriptorSets(device.getDevice(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

        const GpuEyeAdaptationStep step = packEyeAdaptationStep(settings, seconds, snap);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, adaptationPipeline->get());
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, adaptationPipeline->getLayout(), 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(commands, adaptationPipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(step), &step);
        vkCmdDispatch(commands, 1, 1, 1);
    }

    bufferBarrier(commands, adaptation.get(), {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
                  {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT});

    VkBufferCopy copy{};
    copy.size = sizeof(GpuEyeAdaptation);
    vkCmdCopyBuffer(commands, adaptation.get(), reading.readback.get(), 1, &copy);
    bufferBarrier(commands, reading.readback.get(), {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
                  {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT});
    reading.pending = true;
}

VkDescriptorBufferInfo VulkanEyeAdaptation::describeAdaptation() const {
    return VkDescriptorBufferInfo{adaptation.get(), 0, VK_WHOLE_SIZE};
}

std::optional<GpuEyeAdaptation> VulkanEyeAdaptation::collect(std::uint32_t frameIndex) {
    Reading& reading = readings.at(frameIndex);

    if (!reading.pending) {
        return std::nullopt;
    }

    reading.pending = false;
    reading.readback.invalidate();
    GpuEyeAdaptation adapted;
    std::memcpy(&adapted, reading.readback.getMapped(), sizeof(adapted));
    return adapted;
}

const VulkanBuffer& VulkanEyeAdaptation::getHistogram() const {
    return histogram;
}
