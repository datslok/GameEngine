#include "render/vulkan/vulkan_ambient_occlusion.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/vulkan_device.h"

#include <array>
#include <cstring>

namespace {
    // The light gathered, which is as bright as the picture it came from, and the visibility beside it.
    constexpr VkFormat occlusionFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    // The history's view depth and age: two 32-bit floats, a format every Vulkan GPU writes as a storage image (two
    // halves would need the extended storage formats).
    constexpr VkFormat depthAgeFormat = VK_FORMAT_R32G32_SFLOAT;
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

    ComputePipelineDescription temporal;
    temporal.shader = "assets/shaders/ambient_occlusion_temporal.comp.spv";
    temporal.storageBuffers = 1;
    temporal.sampledImages = 6;
    temporal.storageImages = 3;
    temporalPipeline = std::make_unique<ComputePipeline>(device, temporal);

    ComputePipelineDescription blur;
    blur.shader = "assets/shaders/ambient_occlusion_blur.comp.spv";
    blur.storageBuffers = 1;
    blur.sampledImages = 2;
    blur.storageImages = 1;
    blurPipeline = std::make_unique<ComputePipeline>(device, blur);

    ComputePipelineDescription fillBlur = blur;
    fillBlur.shader = "assets/shaders/ambient_fill_blur.comp.spv";
    fillBlur.sampledImages = 2;
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

    const auto image = [&](const char* name, VkFormat format = occlusionFormat) {
        ImageDescription description;
        description.format = format;
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
    historyLight = {image("Ambient occlusion history 0"), image("Ambient occlusion history 1")};
    historyDepthAge = {image("Ambient occlusion history depth and age 0", depthAgeFormat), image("Ambient occlusion history depth and age 1", depthAgeFormat)};
    historyFill = {image("Ambient occlusion fill history 0"), image("Ambient occlusion fill history 1")};
    lastHistory.reset();
    size = render;
}

bool VulkanAmbientOcclusion::hasImages() const {
    return static_cast<bool>(noisy);
}

SampledImage VulkanAmbientOcclusion::sampledOcclusion(bool isBlurred, bool accumulated) const {
    const VkImageView view = isBlurred ? blurred.getView() : accumulated ? historyLight[static_cast<std::size_t>(1 - latest)].getView() : noisy.getView();
    return SampledImage{view, nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

void VulkanAmbientOcclusion::forgetHistory() {
    lastHistory.reset();
}

/*
* The accumulation, from the pass's image and last frame's history pair into the other pair, which becomes the latest.
* The barriers: the pass's image from being written to being read by compute; last frame's pair from whatever read it
* last (the blur, the main pass, or nothing yet) to being read by compute, its contents discarded when it is not used
* (an image never written has nothing to keep); this frame's pair to being written, its old contents discarded, every
* pixel being written.
*/
void VulkanAmbientOcclusion::accumulate(VkCommandBuffer commands, FrameDescriptors& descriptors, const VulkanBuffer& record, const SampledImage& depthSample,
                                        bool usable, std::uint32_t groupsAcross, std::uint32_t groupsDown) {
    const std::size_t previous = static_cast<std::size_t>(latest);
    const std::size_t next = 1 - previous;
    const ImageContents keep = usable ? ImageContents::Keep : ImageContents::Discard;
    noisy.transition(commands, ImageState::ComputeRead);
    historyLight[previous].transition(commands, ImageState::ComputeRead, keep);
    historyDepthAge[previous].transition(commands, ImageState::ComputeRead, keep);
    historyFill[previous].transition(commands, ImageState::ComputeRead, keep);
    fill.transition(commands, ImageState::ComputeRead);
    historyLight[next].transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    historyDepthAge[next].transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    historyFill[next].transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    const std::array<ComputeBinding, 10> bindings{
        storageBufferBinding(record.get()),
        sampledBinding(depthSample.sampler, depthSample.view, depthSample.layout),
        sampledBinding(nearest, noisy.getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        sampledBinding(nearest, historyLight[previous].getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        sampledBinding(nearest, historyDepthAge[previous].getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        sampledBinding(nearest, fill.getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        sampledBinding(nearest, historyFill[previous].getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        storageImageBinding(historyLight[next].getView()),
        storageImageBinding(historyDepthAge[next].getView()),
        storageImageBinding(historyFill[next].getView()),
    };

    bindComputeSet(commands, device.getDevice(), descriptors, *temporalPipeline, bindings);
    vkCmdDispatch(commands, groupsAcross, groupsDown, 1);
    latest = static_cast<int>(next);
}

/*
* The fill's parts blurred across from source (the fill's average, or the pass's own fill) into fillAcross, then down
* into the fill (whose own contents the accumulation or the first pass has read by then), each read by compute after being
* written, the fill left readable by fragment shaders for the main pass. The view distances come from the depth.
*/
void VulkanAmbientOcclusion::blurFill(VkCommandBuffer commands, FrameDescriptors& descriptors, const VulkanBuffer& record, const SampledImage& depthSample,
                                      VulkanImage& source, std::uint32_t groupsAcross, std::uint32_t groupsDown) {
    const auto pass = [&](VulkanImage& from, VulkanImage& to, std::int32_t x, std::int32_t y) {
        from.transition(commands, ImageState::ComputeRead);
        to.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
        const std::array<ComputeBinding, 4> bindings{
            storageBufferBinding(record.get()),
            sampledBinding(depthSample.sampler, depthSample.view, depthSample.layout),
            sampledBinding(nearest, from.getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            storageImageBinding(to.getView()),
        };
        const std::array<std::int32_t, 4> direction{x, y, 0, 0};
        bindComputeSet(commands, device.getDevice(), descriptors, *fillBlurPipeline, bindings);
        vkCmdPushConstants(commands, fillBlurPipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(direction), direction.data());
        vkCmdDispatch(commands, groupsAcross, groupsDown, 1);
    };

    pass(source, fillAcross, 1, 0);
    pass(fillAcross, fill, 0, 1);
    fill.transition(commands, ImageState::FragmentRead);
}

SampledImage VulkanAmbientOcclusion::sampledFill() const {
    return SampledImage{fill.getView(), nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

/*
* The barriers: the depth from being drawn into by the prepass to being read by compute (the main pass turns it back
* into a depth target, keeping it); last frame's picture, sampled here before the reflections do; the images from being read by last frame's main pass (or nothing yet) to being
* written, their old contents discarded, every pixel being written. The accumulation then reads the pass's image (see
* accumulate). With the blur, the accumulation's image (or the pass's) then goes to being read by compute and the blur's
* is written; whichever the main pass reads ends readable by fragment shaders.
*/
void VulkanAmbientOcclusion::occlude(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& depth,
                                     VulkanImage* previousPicture, const SampledImage& standIn, const GpuAmbientOcclusionPass& pass, bool blur,
                                     bool previousCameraKnown) {
    // Whether last frame's history carries over is known here, where what it was made for is kept.
    const bool accumulates = pass.history[2] > 0.5f;
    const AoHistoryKey key{size, pass.gather[1] > 0.5f};
    const bool usable = accumulates && aoHistoryUsable(lastHistory, key, previousCameraKnown);
    GpuAmbientOcclusionPass recorded = pass;
    recorded.history[3] = usable ? 1.0f : 0.0f;

    VulkanBuffer& record = passRecords.at(frameIndex);
    std::memcpy(record.getMapped(), &recorded, sizeof(recorded));
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

    // The image the blur or the main pass reads: the accumulation's average while it runs, else the pass's own.
    VulkanImage* result = &noisy;

    if (accumulates) {
        accumulate(commands, descriptors, record, depthSample, usable, groupsAcross, groupsDown);
        lastHistory = key;
        result = &historyLight[static_cast<std::size_t>(latest)];
    } else {
        lastHistory.reset();
    }

    // The fill (its average while accumulating) blurred into the image the main pass reads; empty while nothing is
    // gathered: nothing to blur.
    if (pass.gather[1] > 0.5f) {
        blurFill(commands, descriptors, record, depthSample, accumulates ? historyFill[static_cast<std::size_t>(latest)] : fill, groupsAcross, groupsDown);
    } else {
        fill.transition(commands, ImageState::FragmentRead);
    }

    if (!blur) {
        result->transition(commands, ImageState::FragmentRead);
        return;
    }

    result->transition(commands, ImageState::ComputeRead);
    blurred.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    const std::array<ComputeBinding, 4> blurBindings{
        storageBufferBinding(record.get()),
        sampledBinding(depthSample.sampler, depthSample.view, depthSample.layout),
        sampledBinding(nearest, result->getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        storageImageBinding(blurred.getView()),
    };

    bindComputeSet(commands, device.getDevice(), descriptors, *blurPipeline, blurBindings);
    vkCmdDispatch(commands, groupsAcross, groupsDown, 1);
    blurred.transition(commands, ImageState::FragmentRead);
}
