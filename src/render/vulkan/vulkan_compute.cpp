#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/vulkan_check.h"
#include "render/vulkan/vulkan_device.h"

#include <stdexcept>
#include <vector>

/*
* Three objects, each built from the one before: the descriptor set layout (which kinds of resource sit at which
* binding), the pipeline layout (that set plus the push constant range), and the pipeline (the compiled shader against
* that layout). The shader module is only needed while the driver compiles, so it is dropped at the end.
* With a shared layout the first two are made elsewhere (the scene's) and only the pipeline is built here.
*/
ComputePipeline::ComputePipeline(const VulkanDevice& device, const ComputePipelineDescription& description):
    pushConstantBytes(description.pushConstantBytes)
{
    if (description.shader == nullptr) {
        throw std::invalid_argument("A compute pipeline needs a shader");
    }

    const VkDevice handle = device.getDevice();
    VkPipelineLayout pipelineLayout = description.sharedLayout;

    if (pipelineLayout == VK_NULL_HANDLE) {
        pipelineLayout = createLayout(handle, description);
    }

    const OwnedHandle<VkShaderModule> shader = device.loadShader(description.shader);

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shader.get();
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = pipelineLayout;

    VkPipeline createdPipeline = VK_NULL_HANDLE;
    vulkanCheck(vkCreateComputePipelines(handle, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &createdPipeline), description.shader);
    pipeline = OwnedHandle<VkPipeline>{handle, createdPipeline, vkDestroyPipeline};
    sharedLayout = description.sharedLayout;
}

// The set layout of the storage buffers, sampled images and storage images, and the pipeline layout of that set and the
// push constants, kept by this pipeline.
VkPipelineLayout ComputePipeline::createLayout(VkDevice handle, const ComputePipelineDescription& description) {
    const std::uint32_t firstStorageImage = description.storageBuffers + description.sampledImages;
    const std::uint32_t bindingCount = firstStorageImage + description.storageImages;
    std::vector<VkDescriptorSetLayoutBinding> bindings(bindingCount);

    for (std::uint32_t binding = 0; binding < bindingCount; ++binding) {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = binding < description.storageBuffers ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                           : binding < firstStorageImage       ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                               : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    setInfo.pBindings = bindings.data();

    VkDescriptorSetLayout createdSetLayout = VK_NULL_HANDLE;
    vulkanCheck(vkCreateDescriptorSetLayout(handle, &setInfo, nullptr, &createdSetLayout), "Compute descriptor set layout creation failed");
    resourceLayout = OwnedHandle<VkDescriptorSetLayout>{handle, createdSetLayout, vkDestroyDescriptorSetLayout};

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = pushConstantBytes;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &createdSetLayout;
    layoutInfo.pushConstantRangeCount = pushConstantBytes > 0 ? 1 : 0;
    layoutInfo.pPushConstantRanges = &pushRange;

    VkPipelineLayout createdLayout = VK_NULL_HANDLE;
    vulkanCheck(vkCreatePipelineLayout(handle, &layoutInfo, nullptr, &createdLayout), "Compute pipeline layout creation failed");
    layout = OwnedHandle<VkPipelineLayout>{handle, createdLayout, vkDestroyPipelineLayout};
    return createdLayout;
}

VkPipeline ComputePipeline::get() const {
    return pipeline.get();
}

VkPipelineLayout ComputePipeline::getLayout() const {
    return sharedLayout != VK_NULL_HANDLE ? sharedLayout : layout.get();
}

VkDescriptorSetLayout ComputePipeline::getResourceLayout() const {
    return resourceLayout.get();
}

std::uint32_t ComputePipeline::getPushConstantBytes() const {
    return pushConstantBytes;
}

ComputeBinding storageBufferBinding(VkBuffer buffer) {
    return ComputeBinding{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, {}, VkDescriptorBufferInfo{buffer, 0, VK_WHOLE_SIZE}};
}

ComputeBinding sampledBinding(VkSampler sampler, VkImageView view, VkImageLayout layout) {
    return ComputeBinding{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VkDescriptorImageInfo{sampler, view, layout}, {}};
}

ComputeBinding storageImageBinding(VkImageView view) {
    return ComputeBinding{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VkDescriptorImageInfo{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL}, {}};
}

void bindComputeSet(VkCommandBuffer commands, VkDevice device, FrameDescriptors& descriptors, const ComputePipeline& pipeline,
                    std::span<const ComputeBinding> bindings) {
    const VkDescriptorSet set = descriptors.allocate(pipeline.getResourceLayout());
    std::vector<VkWriteDescriptorSet> writes(bindings.size());

    for (std::uint32_t index = 0; index < bindings.size(); ++index) {
        const ComputeBinding& binding = bindings[index];
        VkWriteDescriptorSet& write = writes[index];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = index;
        write.descriptorCount = 1;
        write.descriptorType = binding.type;

        if (binding.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            write.pBufferInfo = &binding.buffer;
        } else {
            write.pImageInfo = &binding.image;
        }
    }

    vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.get());
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getLayout(), 0, 1, &set, 0, nullptr);
}
