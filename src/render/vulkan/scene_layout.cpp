#include "render/vulkan/scene_layout.h"
#include "render/gpu/gpu_scene.h"
#include "render/vulkan/vulkan_check.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace {
    struct BindingKind {
        VkDescriptorType type;
        VkShaderStageFlags stages;
    };

    // What each binding of set 1 holds and which stages read it, in SceneBinding order.
    constexpr std::array<BindingKind, SceneBindingCount> sceneBindings{{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT}, // frame data
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT},           // instances
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT},         // materials
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT},         // directional lights
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT},         // point lights
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT},         // spotlights
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT},         // shadow tiles
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT}, // shadow atlas, compared
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT}, // shadow atlas, raw depths
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT}, // BRDF table
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT},           // draw list
    }};
}

SceneLayout::SceneLayout(VkDevice device, VkDescriptorSetLayout bindlessLayout) {
    std::array<VkDescriptorSetLayoutBinding, SceneBindingCount> bindings{};

    for (std::uint32_t binding = 0; binding < SceneBindingCount; ++binding) {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = sceneBindings[binding].type;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = sceneBindings[binding].stages;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    VkDescriptorSetLayout createdLayout = VK_NULL_HANDLE;
    vulkanCheck(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &createdLayout), "Scene descriptor layout creation failed");
    frameLayout = OwnedHandle<VkDescriptorSetLayout>{device, createdLayout, vkDestroyDescriptorSetLayout};

    const std::array<VkDescriptorSetLayout, 2> setLayouts{bindlessLayout, createdLayout};

    // Only the shadow pass pushes anything: each tile's view, for its vertex shader.
    const VkPushConstantRange shadowView{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GpuShadowView)};

    VkPipelineLayoutCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineInfo.setLayoutCount = static_cast<std::uint32_t>(setLayouts.size());
    pipelineInfo.pSetLayouts = setLayouts.data();
    pipelineInfo.pushConstantRangeCount = 1;
    pipelineInfo.pPushConstantRanges = &shadowView;

    VkPipelineLayout createdPipelineLayout = VK_NULL_HANDLE;
    vulkanCheck(vkCreatePipelineLayout(device, &pipelineInfo, nullptr, &createdPipelineLayout), "Scene pipeline layout creation failed");
    pipelineLayout = OwnedHandle<VkPipelineLayout>{device, createdPipelineLayout, vkDestroyPipelineLayout};
}

VkDescriptorSetLayout SceneLayout::getFrameLayout() const {
    return frameLayout.get();
}

VkPipelineLayout SceneLayout::getPipelineLayout() const {
    return pipelineLayout.get();
}

UploadArray::UploadArray(VmaAllocator allocator, VkBufferUsageFlags usage):
    allocator(allocator),
    usage(usage),
    buffer(allocator, smallestBytes, usage, BufferMemory::Upload)
{
}

void UploadArray::write(const void* data, std::size_t bytes) {
    if (bytes > buffer.getSize()) {
        const std::size_t grown = std::max(bytes, 2 * static_cast<std::size_t>(buffer.getSize()));
        buffer = VulkanBuffer{allocator, grown, usage, BufferMemory::Upload};
    }

    if (bytes > 0) {
        std::memcpy(buffer.getMapped(), data, bytes);
        buffer.flush();
    }
}

VkDescriptorBufferInfo UploadArray::describe() const {
    return VkDescriptorBufferInfo{buffer.get(), 0, VK_WHOLE_SIZE};
}

SceneBuffers::SceneBuffers(VmaAllocator allocator):
    frameData(allocator, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT),
    instances(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
    materials(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
    directionalLights(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
    pointLights(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
    spotLights(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
    shadowTiles(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
    drawList(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
{
}

VkDescriptorSet writeSceneSet(FrameDescriptors& descriptors, const SceneLayout& layout, const SceneBuffers& buffers, const SceneImages& images) {
    const VkDescriptorSet set = descriptors.allocate(layout.getFrameLayout());

    const std::array<VkDescriptorBufferInfo, SceneBindingCount> bufferInfos{
        buffers.frameData.describe(),
        buffers.instances.describe(),
        buffers.materials.describe(),
        buffers.directionalLights.describe(),
        buffers.pointLights.describe(),
        buffers.spotLights.describe(),
        buffers.shadowTiles.describe(),
        VkDescriptorBufferInfo{},
        VkDescriptorBufferInfo{},
        VkDescriptorBufferInfo{},
        buffers.drawList.describe(),
    };

    std::array<VkDescriptorImageInfo, SceneBindingCount> imageInfos{};
    imageInfos[ShadowAtlasBinding] = {images.shadowAtlas.sampler, images.shadowAtlas.view, images.shadowAtlas.layout};
    imageInfos[ShadowDepthsBinding] = {images.shadowDepths.sampler, images.shadowDepths.view, images.shadowDepths.layout};
    imageInfos[BrdfTableBinding] = {images.brdfTable.sampler, images.brdfTable.view, images.brdfTable.layout};

    std::array<VkWriteDescriptorSet, SceneBindingCount> writes{};

    for (std::uint32_t binding = 0; binding < SceneBindingCount; ++binding) {
        VkWriteDescriptorSet& write = writes[binding];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = sceneBindings[binding].type;

        if (write.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            write.pImageInfo = &imageInfos[binding];
        }
        else {
            write.pBufferInfo = &bufferInfos[binding];
        }
    }

    vkUpdateDescriptorSets(descriptors.getDevice(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return set;
}

void bindScene(VkCommandBuffer commands, const SceneLayout& layout, VkDescriptorSet textures, VkDescriptorSet frameSet) {
    const std::array<VkDescriptorSet, 2> sets{textures, frameSet};
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.getPipelineLayout(), bindlessTextureSet,
                            static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
}
