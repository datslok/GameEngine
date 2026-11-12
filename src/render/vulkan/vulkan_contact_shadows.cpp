#include "render/vulkan/vulkan_contact_shadows.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/vulkan_device.h"

#include <array>
#include <cstring>

namespace {
    // One float a pixel: r32f is a storage format every Vulkan GPU writes (8- and 16-bit single channels need the
    // extended storage formats).
    constexpr VkFormat maskFormat = VK_FORMAT_R32_SFLOAT;
}

VulkanContactShadows::VulkanContactShadows(const VulkanDevice& device, VkSampler nearest, std::uint32_t framesInFlight):
    device(device),
    nearest(nearest)
{
    ComputePipelineDescription description;
    description.shader = "assets/shaders/contact_shadows.comp.spv";
    description.storageBuffers = 1;
    description.sampledImages = 2;
    description.storageImages = 1;
    pipeline = std::make_unique<ComputePipeline>(device, description);
    setFramesInFlight(framesInFlight);
}

void VulkanContactShadows::setFramesInFlight(std::uint32_t count) {
    passRecords.clear();

    for (std::uint32_t frame = 0; frame < count; ++frame) {
        passRecords.emplace_back(device.getAllocator(), sizeof(GpuContactShadowPass), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload);
    }
}

void VulkanContactShadows::ensureSize(RenderSize render) {
    if (size == render && mask) {
        return;
    }

    ImageDescription description;
    description.format = maskFormat;
    description.width = render.width;
    description.height = render.height;
    description.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    description.name = "Contact shadows";
    mask = VulkanImage{device, description};
    size = render;
}

bool VulkanContactShadows::hasImages() const {
    return static_cast<bool>(mask);
}

SampledImage VulkanContactShadows::sampledShadow() const {
    return SampledImage{mask.getView(), nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

/*
* The barriers: the depth from being drawn into by the prepass (or read by the ambient occlusion before) to being read by
* compute (the main pass turns it back into a depth target, keeping it); the mask from being read by last frame's main
* pass (or nothing yet) to being written, its old contents discarded since every pixel is written, then to being read by
* fragment shaders.
*/
void VulkanContactShadows::shade(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& depth,
                                 const SampledImage& blueNoise, const GpuContactShadowPass& pass) {
    VulkanBuffer& record = passRecords.at(frameIndex);
    std::memcpy(record.getMapped(), &pass, sizeof(pass));
    record.flush();

    depth.transition(commands, ImageState::DepthComputeRead);
    mask.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    const std::array<ComputeBinding, 4> bindings{
        storageBufferBinding(record.get()),
        sampledBinding(nearest, depth.getView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL),
        sampledBinding(blueNoise.sampler, blueNoise.view, blueNoise.layout),
        storageImageBinding(mask.getView()),
    };

    bindComputeSet(commands, device.getDevice(), descriptors, *pipeline, bindings);
    vkCmdDispatch(commands, workgroupCount(size.width, CONTACT_GROUP_SIDE), workgroupCount(size.height, CONTACT_GROUP_SIDE), 1);
    mask.transition(commands, ImageState::FragmentRead);
}
