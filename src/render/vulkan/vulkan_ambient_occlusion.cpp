#include "render/vulkan/vulkan_ambient_occlusion.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/vulkan_device.h"

#include <array>
#include <cstring>

namespace {
    // The visibility, 0 to 1: 256 levels are plenty, since temporal anti-aliasing blends the frames into the picture.
    constexpr VkFormat occlusionFormat = VK_FORMAT_R8_UNORM;
}

VulkanAmbientOcclusion::VulkanAmbientOcclusion(const VulkanDevice& device, VkSampler nearest, std::uint32_t framesInFlight):
    device(device),
    nearest(nearest)
{
    ComputePipelineDescription occlusion;
    occlusion.shader = "assets/shaders/ambient_occlusion.comp.spv";
    occlusion.storageBuffers = 1;
    occlusion.sampledImages = 1;
    occlusion.storageImages = 1;
    occlusionPipeline = std::make_unique<ComputePipeline>(device, occlusion);

    ComputePipelineDescription blur;
    blur.shader = "assets/shaders/ambient_occlusion_blur.comp.spv";
    blur.storageBuffers = 1;
    blur.sampledImages = 2;
    blur.storageImages = 1;
    blurPipeline = std::make_unique<ComputePipeline>(device, blur);

    setFramesInFlight(framesInFlight);
}

void VulkanAmbientOcclusion::setFramesInFlight(std::uint32_t count) {
    passRecords.clear();

    for (std::uint32_t frame = 0; frame < count; ++frame) {
        passRecords.emplace_back(device.getAllocator(), sizeof(GpuAmbientOcclusionPass), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload);
    }
}

void VulkanAmbientOcclusion::ensureSize(RenderSize render) {
    if (size == render && noisy) {
        return;
    }

    const auto image = [&](const char* name) {
        ImageDescription description;
        description.format = occlusionFormat;
        description.width = render.width;
        description.height = render.height;
        description.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        description.name = name;
        return VulkanImage{device, description};
    };

    noisy = image("Ambient occlusion");
    blurred = image("Ambient occlusion, blurred");
    size = render;
}

bool VulkanAmbientOcclusion::hasImages() const {
    return static_cast<bool>(noisy);
}

SampledImage VulkanAmbientOcclusion::sampledOcclusion(bool isBlurred) const {
    return SampledImage{isBlurred ? blurred.getView() : noisy.getView(), nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

/*
* The barriers: the depth from being drawn into by the prepass to being read by compute (the main pass turns it back
* into a depth target, keeping it); the images from being read by last frame's main pass (or nothing yet) to being
* written, their old contents discarded, every pixel being written. With the blur, the pass's image then goes to being
* read by compute and the blur's is written; whichever the main pass reads ends readable by fragment shaders.
*/
void VulkanAmbientOcclusion::occlude(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& depth,
                                     const GpuAmbientOcclusionPass& pass, bool blur) {
    VulkanBuffer& record = passRecords.at(frameIndex);
    std::memcpy(record.getMapped(), &pass, sizeof(pass));
    record.flush();

    depth.transition(commands, ImageState::DepthComputeRead);
    noisy.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    const SampledImage depthSample{depth.getView(), nearest, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    const std::array<ComputeBinding, 3> occlusionBindings{
        storageBufferBinding(record.get()),
        sampledBinding(depthSample.sampler, depthSample.view, depthSample.layout),
        storageImageBinding(noisy.getView()),
    };

    bindComputeSet(commands, device.getDevice(), descriptors, *occlusionPipeline, occlusionBindings);
    const std::uint32_t groupsAcross = workgroupCount(size.width, AO_GROUP_SIDE);
    const std::uint32_t groupsDown = workgroupCount(size.height, AO_GROUP_SIDE);
    vkCmdDispatch(commands, groupsAcross, groupsDown, 1);

    if (!blur) {
        noisy.transition(commands, ImageState::FragmentRead);
        return;
    }

    noisy.transition(commands, ImageState::ComputeRead);
    blurred.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    const std::array<ComputeBinding, 4> blurBindings{
        storageBufferBinding(record.get()),
        sampledBinding(depthSample.sampler, depthSample.view, depthSample.layout),
        sampledBinding(nearest, noisy.getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        storageImageBinding(blurred.getView()),
    };

    bindComputeSet(commands, device.getDevice(), descriptors, *blurPipeline, blurBindings);
    vkCmdDispatch(commands, groupsAcross, groupsDown, 1);
    blurred.transition(commands, ImageState::FragmentRead);
}
