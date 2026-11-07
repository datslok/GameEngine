#include "render/vulkan/vulkan_ambient_occlusion.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/vulkan_device.h"

#include <array>
#include <cstring>

namespace {
    // The light gathered, which is as bright as the picture it came from, and the visibility beside it.
    constexpr VkFormat occlusionFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
}

VulkanAmbientOcclusion::VulkanAmbientOcclusion(const VulkanDevice& device, VkSampler nearest, std::uint32_t framesInFlight):
    device(device),
    nearest(nearest)
{
    ComputePipelineDescription occlusion;
    occlusion.shader = "assets/shaders/ambient_occlusion.comp.spv";
    occlusion.storageBuffers = 1;
    occlusion.sampledImages = 2;
    occlusion.storageImages = 2;
    occlusionPipeline = std::make_unique<ComputePipeline>(device, occlusion);

    ComputePipelineDescription blur;
    blur.shader = "assets/shaders/ambient_occlusion_blur.comp.spv";
    blur.storageBuffers = 1;
    blur.sampledImages = 2;
    blur.storageImages = 1;
    blurPipeline = std::make_unique<ComputePipeline>(device, blur);

    ComputePipelineDescription fillBlur = blur;
    fillBlur.shader = "assets/shaders/ambient_fill_blur.comp.spv";
    fillBlur.sampledImages = 1;
    fillBlur.pushConstantBytes = sizeof(std::int32_t) * 4;
    fillBlurPipeline = std::make_unique<ComputePipeline>(device, fillBlur);

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
    fill = image("Ambient occlusion fill");
    fillAcross = image("Ambient occlusion fill, blurred across");
    size = render;
}

bool VulkanAmbientOcclusion::hasImages() const {
    return static_cast<bool>(noisy);
}

SampledImage VulkanAmbientOcclusion::sampledOcclusion(bool isBlurred) const {
    return SampledImage{isBlurred ? blurred.getView() : noisy.getView(), nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

/*
* The fill blurred across into fillAcross, then down back into the fill (whose noisy contents the first pass has read
* by then; the view distances it weighs by ride in its alpha), each read by compute after being written, the fill left readable by fragment shaders for the main pass.
*/
void VulkanAmbientOcclusion::blurFill(VkCommandBuffer commands, FrameDescriptors& descriptors, const VulkanBuffer& record, std::uint32_t groupsAcross,
                                      std::uint32_t groupsDown) {
    const auto pass = [&](VulkanImage& from, VulkanImage& to, std::int32_t x, std::int32_t y) {
        from.transition(commands, ImageState::ComputeRead);
        to.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
        const std::array<ComputeBinding, 3> bindings{
            storageBufferBinding(record.get()),
            sampledBinding(nearest, from.getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            storageImageBinding(to.getView()),
        };
        const std::array<std::int32_t, 4> direction{x, y, 0, 0};
        bindComputeSet(commands, device.getDevice(), descriptors, *fillBlurPipeline, bindings);
        vkCmdPushConstants(commands, fillBlurPipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(direction), direction.data());
        vkCmdDispatch(commands, groupsAcross, groupsDown, 1);
    };

    pass(fill, fillAcross, 1, 0);
    pass(fillAcross, fill, 0, 1);
    fill.transition(commands, ImageState::FragmentRead);
}

SampledImage VulkanAmbientOcclusion::sampledFill() const {
    return SampledImage{fill.getView(), nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

/*
* The barriers: the depth from being drawn into by the prepass to being read by compute (the main pass turns it back
* into a depth target, keeping it); last frame's picture, sampled here before the reflections do; the images from being read by last frame's main pass (or nothing yet) to being
* written, their old contents discarded, every pixel being written. With the blur, the pass's image then goes to being
* read by compute and the blur's is written; whichever the main pass reads ends readable by fragment shaders.
*/
void VulkanAmbientOcclusion::occlude(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& depth,
                                     VulkanImage* previousPicture, const SampledImage& standIn, const GpuAmbientOcclusionPass& pass, bool blur) {
    VulkanBuffer& record = passRecords.at(frameIndex);
    std::memcpy(record.getMapped(), &pass, sizeof(pass));
    record.flush();

    depth.transition(commands, ImageState::DepthComputeRead);
    noisy.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    fill.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    if (previousPicture != nullptr) {
        previousPicture->transition(commands, ImageState::ComputeRead);
    }

    // Nearest is plenty: each sample is one noisy guess of light the frames and the blur average.
    const SampledImage picture = previousPicture != nullptr ? SampledImage{previousPicture->getView(), nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL} : standIn;
    const SampledImage depthSample{depth.getView(), nearest, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    const std::array<ComputeBinding, 5> occlusionBindings{
        storageBufferBinding(record.get()),
        sampledBinding(depthSample.sampler, depthSample.view, depthSample.layout),
        sampledBinding(picture.sampler, picture.view, picture.layout),
        storageImageBinding(noisy.getView()),
        storageImageBinding(fill.getView()),
    };

    bindComputeSet(commands, device.getDevice(), descriptors, *occlusionPipeline, occlusionBindings);
    const std::uint32_t groupsAcross = workgroupCount(size.width, AO_GROUP_SIDE);
    const std::uint32_t groupsDown = workgroupCount(size.height, AO_GROUP_SIDE);
    vkCmdDispatch(commands, groupsAcross, groupsDown, 1);

    // The fill is empty while nothing is gathered: nothing to blur.
    if (pass.gather[1] > 0.5f) {
        blurFill(commands, descriptors, record, groupsAcross, groupsDown);
    } else {
        fill.transition(commands, ImageState::FragmentRead);
    }

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
