#include "render/vulkan/vulkan_mirrored_light.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/vulkan_device.h"

#include <array>
#include <cstring>

namespace {
    // The light (16-bit floats: bright near a focus, dim far away) and its direction folded onto a square.
    constexpr VkFormat lightFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    constexpr VkFormat directionFormat = VK_FORMAT_R16G16_SFLOAT;
}

VulkanMirroredLights::VulkanMirroredLights(const VulkanDevice& device, VkSampler nearest, VkDescriptorSetLayout textures, VkDescriptorSetLayout scene,
                                           VkDescriptorSetLayout rayScene, std::uint32_t framesInFlight):
    device(device),
    nearest(nearest)
{
    ComputePipelineDescription pass;
    pass.shader = "assets/shaders/mirrored_light.comp.spv";
    pass.storageBuffers = 1;
    pass.sampledImages = 1;
    pass.storageImages = 2;
    pass.sharedSets = {textures, scene, rayScene};
    pipeline = std::make_unique<ComputePipeline>(device, pass);

    setFramesInFlight(framesInFlight);
}

void VulkanMirroredLights::setFramesInFlight(std::uint32_t count) {
    passRecords.clear();

    for (std::uint32_t frame = 0; frame < count; ++frame) {
        passRecords.emplace_back(device.getAllocator(), sizeof(GpuMirroredLightPass), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload);
    }
}

void VulkanMirroredLights::ensureSize(RenderSize render) {
    if (size == render && arriving) {
        return;
    }

    const auto image = [&](VkFormat format, const char* name) {
        ImageDescription description;
        description.format = format;
        description.width = render.width;
        description.height = render.height;
        description.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        description.name = name;
        return VulkanImage{device, description};
    };

    arriving = image(lightFormat, "Mirrored light");
    directions = image(directionFormat, "Mirrored light directions");
    size = render;
}

bool VulkanMirroredLights::hasImages() const {
    return static_cast<bool>(arriving);
}

SampledImage VulkanMirroredLights::sampledLight() const {
    return SampledImage{arriving.getView(), nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

SampledImage VulkanMirroredLights::sampledDirection() const {
    return SampledImage{directions.getView(), nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

/*
* The barriers: the depth from being drawn into by the prepass to being read by compute (the main pass turns it back
* into a depth target, keeping it); the images from being read by last frame's main pass (or nothing yet) to being
* written, their old contents discarded, every pixel being written; then to being read by fragment shaders.
*/
void VulkanMirroredLights::light(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& depth,
                                 const GpuMirroredLightPass& pass, const MirroredLightSets& sets) {
    VulkanBuffer& record = passRecords.at(frameIndex);
    std::memcpy(record.getMapped(), &pass, sizeof(pass));
    record.flush();

    depth.transition(commands, ImageState::DepthComputeRead);
    arriving.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    directions.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    const std::array<ComputeBinding, 4> bindings{
        storageBufferBinding(record.get()),
        sampledBinding(nearest, depth.getView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL),
        storageImageBinding(arriving.getView()),
        storageImageBinding(directions.getView()),
    };

    bindComputeSet(commands, device.getDevice(), descriptors, *pipeline, bindings);
    const std::array<VkDescriptorSet, 3> shared{sets.textures, sets.scene, sets.rayScene};
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->getLayout(), 0, static_cast<std::uint32_t>(shared.size()), shared.data(), 0,
                            nullptr);
    vkCmdDispatch(commands, workgroupCount(size.width, MIRROR_GROUP_SIDE), workgroupCount(size.height, MIRROR_GROUP_SIDE), 1);

    arriving.transition(commands, ImageState::FragmentRead);
    directions.transition(commands, ImageState::FragmentRead);
}
