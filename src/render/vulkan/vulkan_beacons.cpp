#include "render/vulkan/vulkan_beacons.h"
#include "render/vulkan/descriptor_helpers.h"
#include "render/vulkan/vulkan_device.h"

/*
* Set 2: the records, read by the vertex shader; no push constants (the frame's view is in its uniform block). Additive,
* the depth tested at the beacon's own depth (GREATER_OR_EQUAL, reversed-Z: anything nearer hides it) and not written.
*/
VulkanBeacons::VulkanBeacons(const VulkanDevice& device, DescriptorLayouts& layouts, VkDescriptorSetLayout bindlessLayout, VkDescriptorSetLayout sceneLayout,
                             VkFormat colourFormat, VkFormat depthFormat):
    device(device)
{
    setLayout = createUniformLayout(device.getDevice(), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, "Beacon set layout creation failed");
    const std::array<VkDescriptorSetLayout, 3> sets{bindlessLayout, sceneLayout, setLayout.get()};
    layout = createPipelineLayout(device.getDevice(), sets, VK_SHADER_STAGE_VERTEX_BIT, "Beacon layout creation failed", 0);

    PipelineDescription description;
    description.vertexShader = "assets/shaders/beacon_splat.vert.spv";
    description.fragmentShader = "assets/shaders/beacon_splat.frag.spv";
    description.sharedLayout = layout.get();
    description.colourFormat = colourFormat;
    description.blend = BlendMode::Additive;
    description.depthFormat = depthFormat;
    description.depthTest = true;
    description.depthWrite = false;
    description.depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;
    pipeline = std::make_unique<VulkanPipeline>(device, layouts, description);
}

/*
* This frame in flight's earlier use of its buffer finished before the frame began (its fence), so it is rewritten in
* place every frame.
*/
void VulkanBeacons::draw(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, std::span<const PlacedBeacon> beacons,
                         const Vec3& cameraOrigin, VkDescriptorSet bindlessSet, VkDescriptorSet sceneSet) {
    if (beacons.empty()) {
        return;
    }

    std::optional<UploadArray>& frameRecords = records.at(frameIndex);

    if (!frameRecords) {
        frameRecords.emplace(device.getAllocator(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }

    frameRecords->write(packBeacons(beacons, cameraOrigin));

    const VkDescriptorSet set = descriptors.allocate(setLayout.get());
    const VkDescriptorBufferInfo described = frameRecords->describe();
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &described;
    vkUpdateDescriptorSets(device.getDevice(), 1, &write, 0, nullptr);

    const std::array<VkDescriptorSet, 3> sets{bindlessSet, sceneSet, set};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->get());
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.get(), 0, static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdDraw(commands, 6, static_cast<std::uint32_t>(beacons.size()), 0, 0);
}
