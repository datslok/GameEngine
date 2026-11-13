#include "render/vulkan/vulkan_ambient_occlusion.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/vulkan_device.h"

#include <array>
#include <cstring>

namespace {
    // The light gathered, which is as bright as the picture it came from, and the visibility beside it.
    constexpr VkFormat occlusionFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    // The history's view depth, age and anti-lag's fast mean and second moment: four 32-bit floats, a format every
    // Vulkan GPU writes as a storage image (halves would need the extended storage formats, and the moment their range).
    constexpr VkFormat depthAgeFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

    // The half-size depths: the depth image's own 32-bit floats, copied exactly (a storage image every GPU writes).
    constexpr VkFormat halfDepthFormat = VK_FORMAT_R32_SFLOAT;
}

VulkanAmbientOcclusion::VulkanAmbientOcclusion(const VulkanDevice& device, VkSampler nearest, std::uint32_t framesInFlight,
                                               VkDescriptorSetLayout texturesLayout, VkDescriptorSetLayout frameLayout):
    device(device),
    nearest(nearest)
{
    ComputePipelineDescription occlusion;
    occlusion.shader = "assets/shaders/ambient_occlusion.comp.spv";
    occlusion.sharedSets = {texturesLayout, frameLayout};
    occlusion.storageBuffers = 1;
    occlusion.sampledImages = 3;
    occlusion.storageImages = 2;
    occlusionPipeline = std::make_unique<ComputePipeline>(device, occlusion);

    ComputePipelineDescription temporal;
    temporal.shader = "assets/shaders/ambient_occlusion_temporal.comp.spv";
    temporal.storageBuffers = 1;
    temporal.sampledImages = 4;
    temporal.storageImages = 2;
    temporalPipeline = std::make_unique<ComputePipeline>(device, temporal);

    ComputePipelineDescription denoise;
    denoise.shader = "assets/shaders/ambient_occlusion_denoise.comp.spv";
    denoise.storageBuffers = 1;
    denoise.sampledImages = 2;
    denoise.storageImages = 1;
    denoise.pushConstantBytes = sizeof(std::int32_t) * 4;
    denoisePipeline = std::make_unique<ComputePipeline>(device, denoise);

    ComputePipelineDescription downsample;
    downsample.shader = "assets/shaders/ambient_occlusion_downsample.comp.spv";
    downsample.sampledImages = 1;
    downsample.storageImages = 1;
    downsample.pushConstantBytes = sizeof(std::int32_t) * 4;
    downsamplePipeline = std::make_unique<ComputePipeline>(device, downsample);

    ComputePipelineDescription upsampling;
    upsampling.shader = "assets/shaders/ambient_occlusion_upsample.comp.spv";
    upsampling.storageBuffers = 1;
    upsampling.sampledImages = 3;
    upsampling.storageImages = 1;
    upsampling.pushConstantBytes = sizeof(std::int32_t) * 4;
    upsamplePipeline = std::make_unique<ComputePipeline>(device, upsampling);

    setFramesInFlight(framesInFlight);
}

void VulkanAmbientOcclusion::setFramesInFlight(std::uint32_t count) {
    passRecords.clear();

    for (std::uint32_t frame = 0; frame < count; ++frame) {
        passRecords.emplace_back(device.getAllocator(), sizeof(GpuAmbientOcclusionPass), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload);
    }
}

void VulkanAmbientOcclusion::ensureSize(RenderSize render, bool half) {
    if (fits(render, half)) {
        return;
    }

    const RenderSize grid = half ? aoHalfSize(render) : render;
    const auto image = [&](const char* name, VkFormat format = occlusionFormat, RenderSize imageSize = RenderSize{}) {
        const RenderSize made = imageSize.width > 0 ? imageSize : grid;
        ImageDescription description;
        description.format = format;
        description.width = made.width;
        description.height = made.height;
        description.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        description.name = name;
        return VulkanImage{device, description};
    };

    noisy = image("Ambient occlusion");
    blurred = image("Ambient occlusion, denoised");
    denoiseSpare = image("Ambient occlusion, denoised halfway");
    geometry = image("Ambient occlusion geometry");
    historyLight = {image("Ambient occlusion history 0"), image("Ambient occlusion history 1")};
    historyDepthAge = {image("Ambient occlusion history depth and age 0", depthAgeFormat), image("Ambient occlusion history depth and age 1", depthAgeFormat)};
    halfDepth = half ? image("Ambient occlusion half-size depths", halfDepthFormat) : VulkanImage{};
    upsampled = half ? image("Ambient occlusion, upsampled", occlusionFormat, render) : VulkanImage{};
    lastHistory.reset();
    renderSize = render;
    size = grid;
    halfResolution = half;
}

bool VulkanAmbientOcclusion::hasImages() const {
    return static_cast<bool>(noisy);
}

bool VulkanAmbientOcclusion::fits(RenderSize render, bool half) const {
    return hasImages() && renderSize == render && halfResolution == half;
}

bool VulkanAmbientOcclusion::isHalfResolution() const {
    return halfResolution;
}

SampledImage VulkanAmbientOcclusion::sampledOcclusion(bool isBlurred, bool accumulated) const {
    const VkImageView view = halfResolution ? upsampled.getView() : isBlurred ? blurred.getView() : accumulated ? historyLight[static_cast<std::size_t>(1 - latest)].getView() : noisy.getView();
    return SampledImage{view, nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

void VulkanAmbientOcclusion::forgetHistory() {
    lastHistory.reset();
}

/*
* The accumulation, from the pass's image and last frame's history pair into the other pair, which becomes the latest.
* The barriers: the pass's image from being written to being read by compute; last frame's pair from whatever read it
* last (the denoiser, the main pass, or nothing yet) to being read by compute, its contents discarded when it is not used
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
    historyLight[next].transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    historyDepthAge[next].transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    const std::array<ComputeBinding, 7> bindings{
        storageBufferBinding(record.get()),
        sampledBinding(depthSample.sampler, depthSample.view, depthSample.layout),
        sampledBinding(nearest, noisy.getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        sampledBinding(nearest, historyLight[previous].getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        sampledBinding(nearest, historyDepthAge[previous].getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        storageImageBinding(historyLight[next].getView()),
        storageImageBinding(historyDepthAge[next].getView()),
    };

    bindComputeSet(commands, device.getDevice(), descriptors, *temporalPipeline, bindings);
    vkCmdDispatch(commands, groupsAcross, groupsDown, 1);
    latest = static_cast<int>(next);
}

/*
* The denoiser's passes from source, taking turns between the spare image and the denoised one so the last pass writes
* the denoised one, which the main pass or the upsample reads. Barriers: the geometry from being written to being read by
* compute; each pass's source to being read by compute and its target to being written (old contents discarded, every
* pixel written).
*/
void VulkanAmbientOcclusion::denoiseInto(VkCommandBuffer commands, FrameDescriptors& descriptors, const VulkanBuffer& record, VulkanImage& source, int passes,
                                         std::uint32_t groupsAcross, std::uint32_t groupsDown) {
    geometry.transition(commands, ImageState::ComputeRead);
    VulkanImage* from = &source;

    for (int step = 0; step < passes; ++step) {
        VulkanImage& to = (passes - 1 - step) % 2 == 0 ? blurred : denoiseSpare;
        from->transition(commands, ImageState::ComputeRead);
        to.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
        const std::array<ComputeBinding, 4> bindings{
            storageBufferBinding(record.get()),
            sampledBinding(nearest, geometry.getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            sampledBinding(nearest, from->getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            storageImageBinding(to.getView()),
        };
        const std::array<std::int32_t, 4> pushed{step, 0, 0, 0};
        bindComputeSet(commands, device.getDevice(), descriptors, *denoisePipeline, bindings);
        vkCmdPushConstants(commands, denoisePipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushed), pushed.data());
        vkCmdDispatch(commands, groupsAcross, groupsDown, 1);
        from = &to;
    }
}
/*
* The barriers: the depth from being drawn into by the prepass to being read by compute (the main pass turns it back
* into a depth target, keeping it); last frame's picture's pyramid, built just before (VulkanReflections::prepareColourPyramid); the images from being read by last frame's main pass (or nothing yet) to being
* written, their old contents discarded, every pixel being written. accumulateHistory then reads the pass's image (see
* accumulate), and denoise sends the accumulation's image (or the pass's) through the denoiser's passes (see
* denoiseInto); whichever the main pass reads ends readable by fragment shaders.
*/
void VulkanAmbientOcclusion::occlude(VkCommandBuffer commands, FrameDescriptors& descriptors, VkDescriptorSet textures, VkDescriptorSet frameSet,
                                     std::uint32_t frameIndex, VulkanImage& depth,
                                     const SampledImage* gatherPicture, const SampledImage& standIn, const SampledImage& blueNoise, const GpuAmbientOcclusionPass& pass,
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
    const SampledImage renderDepth{depth.getView(), nearest, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    SampledImage depthSample = renderDepth;

    // At half resolution, the picked pixels' depths first: the half-size image written (old contents discarded, every
    // texel written), then read by compute.
    if (halfResolution) {
        halfDepth.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
        const std::array<ComputeBinding, 2> downsampleBindings{
            sampledBinding(renderDepth.sampler, renderDepth.view, renderDepth.layout),
            storageImageBinding(halfDepth.getView()),
        };
        const std::array<std::int32_t, 4> pushed{pass.frame[1], pass.frame[2], static_cast<std::int32_t>(renderSize.width), static_cast<std::int32_t>(renderSize.height)};
        bindComputeSet(commands, device.getDevice(), descriptors, *downsamplePipeline, downsampleBindings);
        vkCmdPushConstants(commands, downsamplePipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushed), pushed.data());
        vkCmdDispatch(commands, workgroupCount(size.width, AO_GROUP_SIDE), workgroupCount(size.height, AO_GROUP_SIDE), 1);
        halfDepth.transition(commands, ImageState::ComputeRead);
        depthSample = SampledImage{halfDepth.getView(), nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }

    noisy.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    geometry.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    // The picture's pyramid, read trilinear at each sample's level (aoSampleLevel), already readable by compute.
    const SampledImage picture = gatherPicture != nullptr ? *gatherPicture : standIn;
    const std::array<ComputeBinding, 6> occlusionBindings{
        storageBufferBinding(record.get()),
        sampledBinding(depthSample.sampler, depthSample.view, depthSample.layout),
        sampledBinding(picture.sampler, picture.view, picture.layout),
        sampledBinding(blueNoise.sampler, blueNoise.view, blueNoise.layout),
        storageImageBinding(noisy.getView()),
        storageImageBinding(geometry.getView()),
    };

    bindComputeSet(commands, device.getDevice(), descriptors, *occlusionPipeline, occlusionBindings);
    const std::array<VkDescriptorSet, 2> sceneSets{textures, frameSet};
    bindSharedSets(commands, *occlusionPipeline, sceneSets);
    const std::uint32_t groupsAcross = workgroupCount(size.width, AO_GROUP_SIDE);
    const std::uint32_t groupsDown = workgroupCount(size.height, AO_GROUP_SIDE);
    vkCmdDispatch(commands, groupsAcross, groupsDown, 1);

    // The image the denoiser, the upsample or the main pass reads: the pass's own until the accumulation replaces it.
    steps = FrameSteps{&record, depthSample, &depth, groupsAcross, groupsDown, key, accumulates, usable, static_cast<int>(pass.denoise[0]), &noisy};
}

void VulkanAmbientOcclusion::accumulateHistory(VkCommandBuffer commands, FrameDescriptors& descriptors) {
    if (!steps.accumulates) {
        lastHistory.reset();
        return;
    }

    accumulate(commands, descriptors, *steps.record, steps.depth, steps.usable, steps.groupsAcross, steps.groupsDown);
    lastHistory = steps.key;
    steps.result = &historyLight[static_cast<std::size_t>(latest)];
}

void VulkanAmbientOcclusion::denoise(VkCommandBuffer commands, FrameDescriptors& descriptors, bool blur) {
    if (blur) {
        denoiseInto(commands, descriptors, *steps.record, *steps.result, steps.denoisePasses, steps.groupsAcross, steps.groupsDown);
        steps.result = &blurred;
    }

    // At the render's size the main pass reads the result itself; at half resolution the upsample reads it first.
    if (!halfResolution) {
        steps.result->transition(commands, ImageState::FragmentRead);
    }
}

/*
* The barriers: the result and the geometry from being written (or read already) to being read by compute; the
* upsampled image from being read by last frame's main pass to being written, its old contents discarded, every pixel
* written; then readable by fragment shaders for the main pass. The render's depths are still readable by compute from
* occlude.
*/
void VulkanAmbientOcclusion::upsample(VkCommandBuffer commands, FrameDescriptors& descriptors) {
    if (!halfResolution) {
        return;
    }

    steps.result->transition(commands, ImageState::ComputeRead);
    geometry.transition(commands, ImageState::ComputeRead);
    upsampled.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    const std::array<ComputeBinding, 5> bindings{
        storageBufferBinding(steps.record->get()),
        sampledBinding(nearest, steps.renderDepth->getView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL),
        sampledBinding(nearest, geometry.getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        sampledBinding(nearest, steps.result->getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        storageImageBinding(upsampled.getView()),
    };
    const std::array<std::int32_t, 4> pushed{static_cast<std::int32_t>(renderSize.width), static_cast<std::int32_t>(renderSize.height), 0, 0};
    bindComputeSet(commands, device.getDevice(), descriptors, *upsamplePipeline, bindings);
    vkCmdPushConstants(commands, upsamplePipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushed), pushed.data());
    vkCmdDispatch(commands, workgroupCount(renderSize.width, AO_GROUP_SIDE), workgroupCount(renderSize.height, AO_GROUP_SIDE), 1);
    upsampled.transition(commands, ImageState::FragmentRead);
}
