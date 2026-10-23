#include "render/vulkan/vulkan_reflection_probe.h"
#include "render/gpu/compute_dispatch.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/mip_levels.h"
#include "render/vulkan/vulkan_check.h"
#include "render/vulkan/vulkan_device.h"

#include <algorithm>
#include <array>

namespace {
    // The capture keeps the scene image's 16-bit floats, so bright light (the sun's disc) survives the prefilter.
    constexpr VkFormat lightFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    constexpr VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    constexpr VkFormat distanceFormat = VK_FORMAT_R32_SFLOAT;

    // The distance pass's push constants: x the face's size, y the near plane, z the far plane.
    struct GpuProbeDistancePass {
        float lens[4];
    };
}

VulkanReflectionProbe::VulkanReflectionProbe(const VulkanDevice& device, VkSampler nearest, VkSampler trilinear):
    device(device),
    nearest(nearest),
    trilinear(trilinear)
{
    ComputePipelineDescription distance;
    distance.shader = "assets/shaders/probe_distance.comp.spv";
    distance.sampledImages = 1;
    distance.storageImages = 1;
    distance.pushConstantBytes = sizeof(GpuProbeDistancePass);
    distancePipeline = std::make_unique<ComputePipeline>(device, distance);

    ComputePipelineDescription distanceMin;
    distanceMin.shader = "assets/shaders/probe_distance_min.comp.spv";
    distanceMin.storageImages = 2;
    distanceMin.pushConstantBytes = sizeof(std::int32_t) * 4;
    distanceMinPipeline = std::make_unique<ComputePipeline>(device, distanceMin);

    ComputePipelineDescription prefilter;
    prefilter.shader = "assets/shaders/probe_prefilter.comp.spv";
    prefilter.sampledImages = 1;
    prefilter.storageImages = 1;
    prefilter.pushConstantBytes = sizeof(GpuPrefilterPass);
    prefilterPipeline = std::make_unique<ComputePipeline>(device, prefilter);
}

OwnedHandle<VkImageView> VulkanReflectionProbe::makeView(const VulkanImage& image, VkImageViewType type, std::uint32_t level,
                                                         std::uint32_t firstLayer, std::uint32_t layers) const {
    const ImageDescription& description = image.getDescription();
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image.get();
    info.viewType = type;
    info.format = description.format;
    info.subresourceRange.aspectMask = description.aspect;
    info.subresourceRange.baseMipLevel = level;
    info.subresourceRange.levelCount = 1;
    info.subresourceRange.baseArrayLayer = firstLayer;
    info.subresourceRange.layerCount = layers;

    VkImageView view = VK_NULL_HANDLE;
    vulkanCheck(vkCreateImageView(device.getDevice(), &info, nullptr, &view), "Reflection probe view creation failed");
    return OwnedHandle<VkImageView>{device.getDevice(), view, vkDestroyImageView};
}

void VulkanReflectionProbe::ensureSize(std::uint32_t newSize) {
    if (newSize == size) {
        return;
    }

    const auto cube = [&](VkFormat format, std::uint32_t levels, VkImageUsageFlags usage, const char* name) {
        ImageDescription description;
        description.format = format;
        description.width = newSize;
        description.height = newSize;
        description.levels = levels;
        description.layers = cubeFaceCount;
        description.cube = true;
        description.usage = usage;
        description.name = name;
        return VulkanImage{device, description};
    };

    capture = cube(lightFormat, mipLevelCount(newSize, newSize),
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   "Reflection probe capture");
    distances = cube(distanceFormat, mipLevelCount(newSize, newSize), VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "Reflection probe distances");
    light = cube(lightFormat, probeLevels(newSize), VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "Reflection probe light");

    ImageDescription depthDescription;
    depthDescription.format = depthFormat;
    depthDescription.width = newSize;
    depthDescription.height = newSize;
    depthDescription.layers = cubeFaceCount;
    depthDescription.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthDescription.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    depthDescription.name = "Reflection probe depth";
    depth = VulkanImage{device, depthDescription};

    captureFaces.clear();
    depthFaces.clear();
    lightLevels.clear();

    for (std::uint32_t face = 0; face < cubeFaceCount; ++face) {
        captureFaces.push_back(makeView(capture, VK_IMAGE_VIEW_TYPE_2D, 0, face, 1));
        depthFaces.push_back(makeView(depth, VK_IMAGE_VIEW_TYPE_2D, 0, face, 1));
    }

    distanceLevels.clear();

    for (std::uint32_t level = 0; level < distances.getDescription().levels; ++level) {
        distanceLevels.push_back(makeView(distances, VK_IMAGE_VIEW_TYPE_2D_ARRAY, level, 0, cubeFaceCount));
    }

    for (std::uint32_t level = 0; level < light.getDescription().levels; ++level) {
        lightLevels.push_back(makeView(light, VK_IMAGE_VIEW_TYPE_2D_ARRAY, level, 0, cubeFaceCount));
    }

    size = newSize;
    captured = false;
}

std::uint32_t VulkanReflectionProbe::getSize() const {
    return size;
}

// The barriers: from being read by last frame's passes (or nothing yet) to being drawn into, the old contents discarded.
void VulkanReflectionProbe::beginCapture(VkCommandBuffer commands) {
    capture.transition(commands, ImageState::ColourTarget, ImageContents::Discard);
    depth.transition(commands, ImageState::DepthTarget, ImageContents::Discard);
}

ColourAttachment VulkanReflectionProbe::faceColour(int face, const Vec3& background) const {
    return ColourAttachment{captureFaces.at(static_cast<std::size_t>(face)).get(), VK_ATTACHMENT_LOAD_OP_CLEAR,
                            {background.x, background.y, background.z, 1.0f}};
}

DepthAttachment VulkanReflectionProbe::faceDepth(int face) const {
    return DepthAttachment{depthFaces.at(static_cast<std::size_t>(face)).get(), VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, gpuFarDepth};
}

/*
* The barriers, each for one use:
* - the capture's mips are blitted from its drawn top level (generateMipmaps), and every level left for compute to read;
* - the depths from being drawn into to being read by compute;
* - the distances and the prefiltered light from being read (last frame) to being written as storage images, their
*   old contents discarded, each distance level made visible to the next one's pass, and after their dispatches to
*   being read by compute (the reflection pass).
* The prefiltered levels all read the capture alone, so they need nothing between them.
*/
void VulkanReflectionProbe::finishCapture(VkCommandBuffer commands, FrameDescriptors& descriptors, float nearPlane, float farPlane,
                                          const Vec3& captureOrigin) {
    generateMipmaps(commands, capture, ImageState::ColourTarget, ImageState::ComputeRead);
    depth.transition(commands, ImageState::DepthComputeRead);
    distances.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    light.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    const std::array<ComputeBinding, 2> distanceBindings{
        sampledBinding(nearest, depth.getView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL),
        storageImageBinding(distanceLevels[0].get()),
    };
    bindComputeSet(commands, device.getDevice(), descriptors, *distancePipeline, distanceBindings);
    const GpuProbeDistancePass lens{{static_cast<float>(size), nearPlane, farPlane, 0.0f}};
    vkCmdPushConstants(commands, distancePipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(lens), &lens);
    vkCmdDispatch(commands, workgroupCount(size, PROBE_GROUP_SIDE), workgroupCount(size, PROBE_GROUP_SIDE), cubeFaceCount);

    // The distances' pyramid: each level the nearest of the four texels below it.
    for (std::uint32_t level = 1; level < distanceLevels.size(); ++level) {
        computeLevelWritten(commands, distances, level - 1);
        const std::uint32_t levelSize = std::max(size >> level, 1u);
        const std::array<ComputeBinding, 2> levelBindings{
            storageImageBinding(distanceLevels[level - 1].get()),
            storageImageBinding(distanceLevels[level].get()),
        };
        bindComputeSet(commands, device.getDevice(), descriptors, *distanceMinPipeline, levelBindings);
        const std::array<std::int32_t, 4> levelPass{static_cast<std::int32_t>(levelSize), 0, 0, 0};
        vkCmdPushConstants(commands, distanceMinPipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(levelPass), levelPass.data());
        vkCmdDispatch(commands, workgroupCount(levelSize, PROBE_GROUP_SIDE), workgroupCount(levelSize, PROBE_GROUP_SIDE), cubeFaceCount);
    }

    const std::uint32_t levels = light.getDescription().levels;
    const std::uint32_t sourceLevels = capture.getDescription().levels;

    for (std::uint32_t level = 0; level < levels; ++level) {
        const std::uint32_t levelSize = std::max(size >> level, 1u);
        const std::array<ComputeBinding, 2> bindings{
            sampledBinding(trilinear, capture.getView()),
            storageImageBinding(lightLevels[level].get()),
        };
        bindComputeSet(commands, device.getDevice(), descriptors, *prefilterPipeline, bindings);
        const GpuPrefilterPass pass = packPrefilterPass(levelSize, level, levels, size, sourceLevels);
        vkCmdPushConstants(commands, prefilterPipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pass), &pass);
        vkCmdDispatch(commands, workgroupCount(levelSize, PROBE_GROUP_SIDE), workgroupCount(levelSize, PROBE_GROUP_SIDE), cubeFaceCount);
    }

    distances.transition(commands, ImageState::ComputeRead);
    light.transition(commands, ImageState::ComputeRead);
    captured = true;
    origin = captureOrigin;
}

bool VulkanReflectionProbe::hasCapture() const {
    return captured;
}

const Vec3& VulkanReflectionProbe::getOrigin() const {
    return origin;
}

std::uint32_t VulkanReflectionProbe::getLevels() const {
    return light.getDescription().levels;
}

std::uint32_t VulkanReflectionProbe::getDistanceLevels() const {
    return distances.getDescription().levels;
}

// Never captured into: readable with nothing in it (the reflection pass reads it only once a capture is there).
VkImageView VulkanReflectionProbe::readableLight(VkCommandBuffer commands) {
    light.transition(commands, ImageState::ComputeRead, captured ? ImageContents::Keep : ImageContents::Discard);
    return light.getView();
}

VkImageView VulkanReflectionProbe::readableDistances(VkCommandBuffer commands) {
    distances.transition(commands, ImageState::ComputeRead, captured ? ImageContents::Keep : ImageContents::Discard);
    return distances.getView();
}
