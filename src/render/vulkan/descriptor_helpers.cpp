#include "render/vulkan/descriptor_helpers.h"
#include "render/vulkan/vulkan_check.h"

#include <cstddef>
#include <vector>

OwnedHandle<VkPipelineLayout> createPipelineLayout(VkDevice device, std::span<const VkDescriptorSetLayout> sets, VkShaderStageFlags pushStages,
                                                   const char* failure, std::uint32_t pushBytes) {
    const VkPushConstantRange push{pushStages, 0, pushBytes};

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<std::uint32_t>(sets.size());
    layoutInfo.pSetLayouts = sets.data();
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;

    VkPipelineLayout created = VK_NULL_HANDLE;
    vulkanCheck(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &created), failure);
    return OwnedHandle<VkPipelineLayout>{device, created, vkDestroyPipelineLayout};
}

OwnedHandle<VkDescriptorSetLayout> createUniformLayout(VkDevice device, VkDescriptorType type, std::uint32_t count, VkShaderStageFlags stages,
                                                       const char* failure) {
    std::vector<VkDescriptorSetLayoutBinding> bindings(count);

    for (std::uint32_t index = 0; index < count; ++index) {
        bindings[index].binding = index;
        bindings[index].descriptorType = type;
        bindings[index].descriptorCount = 1;
        bindings[index].stageFlags = stages;
    }

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = count;
    info.pBindings = bindings.data();

    VkDescriptorSetLayout created = VK_NULL_HANDLE;
    vulkanCheck(vkCreateDescriptorSetLayout(device, &info, nullptr, &created), failure);
    return OwnedHandle<VkDescriptorSetLayout>{device, created, vkDestroyDescriptorSetLayout};
}

void writeSampledImages(VkDevice device, VkDescriptorSet set, std::uint32_t first, std::span<const SampledImage> images) {
    std::vector<VkDescriptorImageInfo> infos;
    std::vector<VkWriteDescriptorSet> writes(images.size());

    for (const SampledImage& image : images) {
        infos.push_back(VkDescriptorImageInfo{image.sampler, image.view, image.layout});
    }

    for (std::size_t index = 0; index < images.size(); ++index) {
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = set;
        writes[index].dstBinding = first + static_cast<std::uint32_t>(index);
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[index].pImageInfo = &infos[index];
    }

    vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
}
