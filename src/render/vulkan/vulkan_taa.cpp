#include "render/vulkan/vulkan_taa.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/vulkan_device.h"

namespace {
    // Motion in NDC per pixel: two 16-bit floats are plenty (and hold TAA_NO_MOTION, under the half-float limit).
    constexpr VkFormat velocityFormat = VK_FORMAT_R16G16_SFLOAT;

    // The history keeps the scene's own format, so smoothing loses none of its range.
    constexpr VkFormat historyFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
}

VulkanTaa::VulkanTaa(const VulkanDevice& device, VkSampler nearest, VkSampler filtered):
    device(device),
    nearest(nearest),
    filtered(filtered)
{
    ComputePipelineDescription description;
    description.shader = "assets/shaders/taa.comp.spv";
    description.sampledImages = 5;
    description.storageImages = 1;
    description.pushConstantBytes = sizeof(GpuTaaPass);
    pipeline = std::make_unique<ComputePipeline>(device, description);

    ComputePipelineDescription fill;
    fill.shader = "assets/shaders/motion_fill.comp.spv";
    fill.sampledImages = 1;
    fill.storageImages = 1;
    fill.pushConstantBytes = sizeof(GpuMotionFillPass);
    fillPipeline = std::make_unique<ComputePipeline>(device, fill);
}

/*
* The old images may still be read by a frame in flight; the renderer waits for the GPU before calling this on a
* resize, as it does for its other frame images.
*/
void VulkanTaa::ensureSize(std::uint32_t newWidth, std::uint32_t newHeight, RenderSize render) {
    if (velocitySize != render) {
        ImageDescription velocityDescription;
        velocityDescription.format = velocityFormat;
        velocityDescription.width = render.width;
        velocityDescription.height = render.height;
        // Storage: the camera motion pass fills it in place.
        velocityDescription.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        velocityDescription.name = "TAA velocity";
        velocity = VulkanImage{device, velocityDescription};
        velocitySize = render;
    }

    if (width == newWidth && height == newHeight) {
        return;
    }

    ImageDescription historyDescription;
    historyDescription.format = historyFormat;
    historyDescription.width = newWidth;
    historyDescription.height = newHeight;
    historyDescription.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    historyDescription.name = "TAA history";

    for (VulkanImage& image : history) {
        image = VulkanImage{device, historyDescription};
    }

    width = newWidth;
    height = newHeight;
    historyValid = false;
}

VulkanImage& VulkanTaa::getVelocity() {
    return velocity;
}

// The resolve turned written over to the other image, so the one it wrote is the other one now.
VulkanImage& VulkanTaa::getResult() {
    return history[1 - written];
}

bool VulkanTaa::hasHistory() const {
    return historyValid;
}

void VulkanTaa::forgetHistory() {
    historyValid = false;
}

/*
* The barriers: the velocity image from the velocity pass's colour target to a storage image read and written in place,
* and the depth to being read by compute.
*/
void VulkanTaa::fillCameraMotion(VkCommandBuffer commands, FrameDescriptors& descriptors, VulkanImage& depth, const GpuMotionFillPass& pass) {
    velocity.transition(commands, ImageState::ComputeStorage);
    depth.transition(commands, ImageState::DepthComputeRead);

    const VkDescriptorSet set = descriptors.allocate(fillPipeline->getResourceLayout());
    const std::array<VkDescriptorImageInfo, 2> images{
        VkDescriptorImageInfo{nearest, depth.getView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
        VkDescriptorImageInfo{VK_NULL_HANDLE, velocity.getView(), VK_IMAGE_LAYOUT_GENERAL},
    };

    std::array<VkWriteDescriptorSet, 2> writes{};

    for (std::uint32_t binding = 0; binding < writes.size(); ++binding) {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = set;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType = binding == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[binding].pImageInfo = &images[binding];
    }

    vkUpdateDescriptorSets(device.getDevice(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, fillPipeline->get());
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, fillPipeline->getLayout(), 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(commands, fillPipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pass), &pass);
    vkCmdDispatch(commands, workgroupCount(velocitySize.width, TAA_GROUP_SIDE), workgroupCount(velocitySize.height, TAA_GROUP_SIDE), 1);
}

/*
* The barriers, each for one use:
* - the velocity image from the velocity pass's colour target to being read by compute (the reactive mask already is);
* - the depth from the depth target (or the depth measurement's read) to being read by compute;
* - last frame's history from being read (by last frame's eye adaptation, bloom and tone mapping, or copied by a
*   capture) to being read by compute; this frame's from anything (its contents are all overwritten, so discarded) to
*   being written as a storage image;
* - after the dispatch, this frame's history to being read by eye adaptation, bloom and tone mapping.
*/
void VulkanTaa::resolve(VkCommandBuffer commands, FrameDescriptors& descriptors, VulkanImage& scene, VulkanImage& depth, VulkanImage& reactive,
                        const GpuTaaPass& pass) {
    VulkanImage& previous = history[1 - written];
    VulkanImage& current = history[written];

    velocity.transition(commands, ImageState::ComputeRead);
    reactive.transition(commands, ImageState::ComputeRead);
    depth.transition(commands, ImageState::DepthComputeRead);
    previous.transition(commands, ImageState::ComputeRead);
    current.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    const VkDescriptorSet set = descriptors.allocate(pipeline->getResourceLayout());
    const std::array<VkDescriptorImageInfo, 6> images{
        VkDescriptorImageInfo{nearest, scene.getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        VkDescriptorImageInfo{nearest, depth.getView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
        VkDescriptorImageInfo{nearest, velocity.getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        VkDescriptorImageInfo{filtered, previous.getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        VkDescriptorImageInfo{nearest, reactive.getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        VkDescriptorImageInfo{VK_NULL_HANDLE, current.getView(), VK_IMAGE_LAYOUT_GENERAL},
    };

    std::array<VkWriteDescriptorSet, 6> writes{};

    for (std::uint32_t binding = 0; binding < writes.size(); ++binding) {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = set;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType = binding < 5 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[binding].pImageInfo = &images[binding];
    }

    vkUpdateDescriptorSets(device.getDevice(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    // The history read last frame's result only when there is one.
    GpuTaaPass recorded = pass;
    recorded.options[2] = historyValid && pass.options[2] > 0.5f ? 1.0f : 0.0f;

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->get());
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->getLayout(), 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(commands, pipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(recorded), &recorded);
    vkCmdDispatch(commands, workgroupCount(width, TAA_GROUP_SIDE), workgroupCount(height, TAA_GROUP_SIDE), 1);

    current.transition(commands, ImageState::SurfaceRead);

    written = 1 - written;
    historyValid = true;
}
