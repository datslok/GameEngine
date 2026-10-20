#include "render/vulkan/vulkan_stars.h"
#include "render/vulkan/vulkan_check.h"
#include "render/vulkan/vulkan_device.h"

/*
* The set: the atmosphere's numbers, its transmittance table and the stars, all read by the vertex shader; the push
* constants: the camera's rays and the sky's turn (GpuStarView). Additive, at the far depth with the depth test equal,
* like the sky, so stars show only where nothing was drawn.
*/
VulkanStars::VulkanStars(const VulkanDevice& device, DescriptorLayouts& layouts, VkFormat colourFormat, VkFormat depthFormat):
    device(device)
{
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    bindings[0] = VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    bindings[1] = VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    bindings[2] = VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    setInfo.pBindings = bindings.data();
    VkDescriptorSetLayout createdSet = VK_NULL_HANDLE;
    vulkanCheck(vkCreateDescriptorSetLayout(device.getDevice(), &setInfo, nullptr, &createdSet), "Star set layout creation failed");
    setLayout = OwnedHandle<VkDescriptorSetLayout>{device.getDevice(), createdSet, vkDestroyDescriptorSetLayout};

    const VkDescriptorSetLayout set = setLayout.get();
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GpuStarView)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &set;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    VkPipelineLayout createdLayout = VK_NULL_HANDLE;
    vulkanCheck(vkCreatePipelineLayout(device.getDevice(), &layoutInfo, nullptr, &createdLayout), "Star layout creation failed");
    layout = OwnedHandle<VkPipelineLayout>{device.getDevice(), createdLayout, vkDestroyPipelineLayout};

    PipelineDescription description;
    description.vertexShader = "assets/shaders/stars.vert.spv";
    description.fragmentShader = "assets/shaders/stars.frag.spv";
    description.sharedLayout = layout.get();
    description.colourFormat = colourFormat;
    description.blend = BlendMode::Additive;
    description.depthFormat = depthFormat;
    description.depthTest = true;
    description.depthWrite = false;
    description.depthCompare = VK_COMPARE_OP_EQUAL;
    pipeline = std::make_unique<VulkanPipeline>(device, layouts, description);
}

void VulkanStars::draw(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, const StarrySky& sky,
                       const VkDescriptorBufferInfo& atmosphere, const SampledImage& transmittance, const GpuRayDebugView& camera) {
    if (!sky.stars || sky.stars->empty()) {
        return;
    }

    FrameStars& stars = frames.at(frameIndex);

    if (!stars.records) {
        stars.records.emplace(device.getAllocator(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }

    // This frame in flight's earlier use of its buffer finished before the frame began (its fence).
    if (stars.writtenFrom != sky.stars) {
        stars.records->write(packStars(*sky.stars));
        stars.writtenFrom = sky.stars;
    }

    const VkDescriptorSet set = descriptors.allocate(setLayout.get());
    const VkDescriptorBufferInfo records = stars.records->describe();
    const VkDescriptorImageInfo table{transmittance.sampler, transmittance.view, transmittance.layout};
    std::array<VkWriteDescriptorSet, 3> writes{};

    for (std::size_t index = 0; index < writes.size(); ++index) {
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = set;
        writes[index].dstBinding = static_cast<std::uint32_t>(index);
        writes[index].descriptorCount = 1;
    }

    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &atmosphere;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &table;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &records;
    vkUpdateDescriptorSets(device.getDevice(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    const GpuStarView view = packStarView(camera, sky);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->get());
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.get(), 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(commands, layout.get(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(view), &view);
    vkCmdDraw(commands, 6, static_cast<std::uint32_t>(sky.stars->size()), 0, 0);
}
