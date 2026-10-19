#include "render/vulkan/bindless_textures.h"
#include "render/vulkan/vulkan_check.h"
#include "render/vulkan/vulkan_device.h"

#include <algorithm>
#include <stdexcept>

BindlessTextures::BindlessTextures(const VulkanDevice& device, std::uint32_t requestedCapacity):
    device(device.getDevice()),
    capacity(std::min(requestedCapacity, device.getBindlessTextureLimit()))
{
    const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                           VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags{};
    bindingFlags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    bindingFlags.bindingCount = 1;
    bindingFlags.pBindingFlags = &flags;

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = capacity;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT; // compute: the light probes' rays

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.pNext = &bindingFlags;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    VkDescriptorSetLayout createdLayout = VK_NULL_HANDLE;
    vulkanCheck(vkCreateDescriptorSetLayout(this->device, &layoutInfo, nullptr, &createdLayout), "Bindless texture layout creation failed");
    layout = OwnedHandle<VkDescriptorSetLayout>{this->device, createdLayout, vkDestroyDescriptorSetLayout};

    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, capacity};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;

    VkDescriptorPool createdPool = VK_NULL_HANDLE;
    vulkanCheck(vkCreateDescriptorPool(this->device, &poolInfo, nullptr, &createdPool), "Bindless texture pool creation failed");
    pool = OwnedHandle<VkDescriptorPool>{this->device, createdPool, vkDestroyDescriptorPool};

    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = createdPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &createdLayout;
    vulkanCheck(vkAllocateDescriptorSets(this->device, &allocateInfo, &set), "Bindless texture set allocation failed");
}

VkDescriptorSetLayout BindlessTextures::getLayout() const {
    return layout.get();
}

VkDescriptorSet BindlessTextures::getSet() const {
    return set;
}

std::uint32_t BindlessTextures::getCapacity() const {
    return capacity;
}

void BindlessTextures::write(std::uint32_t slot, VkImageView view, VkSampler sampler) {
    if (slot >= capacity) {
        throw std::out_of_range("More textures than the bindless texture array holds");
    }

    const VkDescriptorImageInfo image{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}
