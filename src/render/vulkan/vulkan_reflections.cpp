#include "render/vulkan/vulkan_reflections.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/vulkan_check.h"
#include "render/vulkan/vulkan_device.h"

#include <cstring>
#include <span>

namespace {
    // The shaded normal folded onto a square: two 16-bit floats keep it to about a twentieth of a degree.
    constexpr VkFormat normalFormat = VK_FORMAT_R16G16_SFLOAT;

    // The specular albedo (0..1) and the roughness, 8 bits each.
    constexpr VkFormat propertiesFormat = VK_FORMAT_R8G8B8A8_UNORM;

    // 11- and 10-bit floats with no alpha for the stand-in (light, never negative), where the GPU can draw into it.
    constexpr VkFormat compactStandInFormat = VK_FORMAT_B10G11R11_UFLOAT_PACK32;

    // The depth pyramid keeps the depth image's 32-bit floats; the colour pyramid the scene's 16-bit floats.
    constexpr VkFormat depthPyramidFormat = VK_FORMAT_R32_SFLOAT;
    constexpr VkFormat colourPyramidFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
}

VulkanReflections::VulkanReflections(const VulkanDevice& device, VkSampler nearest, VkSampler linear, VkSampler pyramid, VkSampler nearestLevels,
                                     std::uint32_t framesInFlight):
    device(device),
    nearest(nearest),
    linear(linear),
    pyramidSampler(pyramid),
    nearestLevels(nearestLevels)
{
    if (device.supportsFormat(compactStandInFormat, VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT)) {
        standInFormat = compactStandInFormat;
    }

    ComputePipelineDescription depthLevels;
    depthLevels.shader = "assets/shaders/depth_pyramid.comp.spv";
    depthLevels.sampledImages = 1;
    depthLevels.storageImages = 2;
    depthLevels.pushConstantBytes = sizeof(GpuPyramidPass);
    depthPyramidPipeline = std::make_unique<ComputePipeline>(device, depthLevels);

    ComputePipelineDescription colourLevels;
    colourLevels.shader = "assets/shaders/colour_pyramid.comp.spv";
    colourLevels.sampledImages = 1;
    colourLevels.storageImages = 1;
    colourLevels.pushConstantBytes = sizeof(GpuPyramidPass);
    colourPyramidPipeline = std::make_unique<ComputePipeline>(device, colourLevels);

    ComputePipelineDescription reflections;
    reflections.shader = "assets/shaders/reflections.comp.spv";
    reflections.storageBuffers = 1;
    reflections.sampledImages = 8;
    reflections.storageImages = 1;
    reflectionPipeline = std::make_unique<ComputePipeline>(device, reflections);

    setFramesInFlight(framesInFlight);
}

void VulkanReflections::setFramesInFlight(std::uint32_t count) {
    passRecords.clear();

    for (std::uint32_t frame = 0; frame < count; ++frame) {
        passRecords.emplace_back(device.getAllocator(), sizeof(GpuReflectionPass), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload);
    }
}

std::vector<OwnedHandle<VkImageView>> VulkanReflections::levelViews(const VulkanImage& image) const {
    std::vector<OwnedHandle<VkImageView>> views;
    const ImageDescription& description = image.getDescription();

    for (std::uint32_t level = 0; level < description.levels; ++level) {
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = image.get();
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = description.format;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.baseMipLevel = level;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.layerCount = 1;

        VkImageView view = VK_NULL_HANDLE;
        vulkanCheck(vkCreateImageView(device.getDevice(), &info, nullptr, &view), "Pyramid level view creation failed");
        views.emplace_back(device.getDevice(), view, vkDestroyImageView);
    }

    return views;
}

void VulkanReflections::ensureSize(RenderSize render) {
    if (size == render && normals) {
        return;
    }

    const auto surface = [&](VkFormat format, const char* name) {
        ImageDescription description;
        description.format = format;
        description.width = render.width;
        description.height = render.height;
        description.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        description.name = name;
        return VulkanImage{device, description};
    };

    normals = surface(normalFormat, "Reflection normals");
    properties = surface(propertiesFormat, "Reflection properties");
    standIns = surface(standInFormat, "Reflection stand-ins");

    const auto pyramid = [&](VkFormat format, std::uint32_t levels, const char* name) {
        ImageDescription description;
        description.format = format;
        description.width = render.width;
        description.height = render.height;
        description.levels = levels;
        description.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        description.name = name;
        return VulkanImage{device, description};
    };

    depthPyramid = pyramid(depthPyramidFormat, depthPyramidLevels(render), "Depth pyramid");
    colourPyramid = pyramid(colourPyramidFormat, depthPyramidLevels(render), "Colour pyramid");
    depthLevels = levelViews(depthPyramid);
    colourLevels = levelViews(colourPyramid);
    size = render;
}

std::vector<VkFormat> VulkanReflections::surfaceFormats() const {
    return {normalFormat, propertiesFormat, standInFormat};
}

std::uint32_t VulkanReflections::getPyramidLevels() const {
    return depthPyramid.getDescription().levels;
}

/*
* The barriers: each surface from being read by last frame's reflection pass (or nothing yet) to being drawn into, its
* old contents discarded.
*/
std::array<ColourAttachment, 3> VulkanReflections::beginSurfaces(VkCommandBuffer commands) {
    normals.transition(commands, ImageState::ColourTarget, ImageContents::Discard);
    properties.transition(commands, ImageState::ColourTarget, ImageContents::Discard);
    standIns.transition(commands, ImageState::ColourTarget, ImageContents::Discard);
    return {
        ColourAttachment{normals.getView(), VK_ATTACHMENT_LOAD_OP_DONT_CARE, {}},
        ColourAttachment{properties.getView(), VK_ATTACHMENT_LOAD_OP_DONT_CARE, {}},
        ColourAttachment{standIns.getView(), VK_ATTACHMENT_LOAD_OP_DONT_CARE, {}},
    };
}

/*
* Level 0 from the depth image (read by compute), each level above from the one below. The pyramid starts with its old
* contents discarded, all of it in the general layout, and each level is made visible to the next dispatch; at the end
* the whole pyramid becomes readable by the reflection pass.
*/
void VulkanReflections::buildDepthPyramid(VkCommandBuffer commands, FrameDescriptors& descriptors, VulkanImage& depth) {
    depth.transition(commands, ImageState::DepthComputeRead);
    depthPyramid.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    for (std::uint32_t level = 0; level < depthLevels.size(); ++level) {
        const RenderSize target = pyramidLevelSize(size, level);
        const RenderSize source = pyramidLevelSize(size, level == 0 ? 0 : level - 1);
        const std::array<ComputeBinding, 3> bindings{
            sampledBinding(nearest, depth.getView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL),
            storageImageBinding(depthLevels[level == 0 ? 0 : level - 1].get()),
            storageImageBinding(depthLevels[level].get()),
        };

        bindComputeSet(commands, device.getDevice(), descriptors, *depthPyramidPipeline, bindings);
        const GpuPyramidPass pass = packPyramidPass(target, level, source);
        vkCmdPushConstants(commands, depthPyramidPipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pass), &pass);
        vkCmdDispatch(commands, workgroupCount(target.width, SSR_GROUP_SIDE), workgroupCount(target.height, SSR_GROUP_SIDE), 1);
        computeLevelWritten(commands, depthPyramid, level);
    }

    depthPyramid.transition(commands, ImageState::ComputeRead);
}

/*
* Level 0 from last frame's picture (sampled by compute, any size), each level above from the one below, sampled in the
* general layout; as the depth pyramid, readable by the reflection pass at the end.
*/
void VulkanReflections::buildColourPyramid(VkCommandBuffer commands, FrameDescriptors& descriptors, VulkanImage& previousPicture) {
    previousPicture.transition(commands, ImageState::ComputeRead);
    colourPyramid.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    for (std::uint32_t level = 0; level < colourLevels.size(); ++level) {
        const RenderSize target = pyramidLevelSize(size, level);
        const ImageDescription& picture = previousPicture.getDescription();
        const RenderSize source = level == 0 ? RenderSize{picture.width, picture.height} : pyramidLevelSize(size, level - 1);
        const std::array<ComputeBinding, 2> bindings{
            level == 0 ? sampledBinding(linear, previousPicture.getView()) : sampledBinding(linear, colourLevels[level - 1].get(), VK_IMAGE_LAYOUT_GENERAL),
            storageImageBinding(colourLevels[level].get()),
        };

        bindComputeSet(commands, device.getDevice(), descriptors, *colourPyramidPipeline, bindings);
        const GpuPyramidPass pass = packPyramidPass(target, level, source);
        vkCmdPushConstants(commands, colourPyramidPipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pass), &pass);
        vkCmdDispatch(commands, workgroupCount(target.width, SSR_GROUP_SIDE), workgroupCount(target.height, SSR_GROUP_SIDE), 1);
        computeLevelWritten(commands, colourPyramid, level);
    }

    colourPyramid.transition(commands, ImageState::ComputeRead);
}

/*
* The barriers, besides the pyramids': the surfaces and the velocity image from being drawn into to being read by
* compute, and the scene image from being read (it was left readable by the main pass) to being read and written in
* place, and back to being readable by fragment and compute shaders afterwards. Without last frame's picture the colour
* pyramid is not built, only made bindable (its contents discarded: the pass does not read it then).
*/
void VulkanReflections::reflect(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& scene,
                                VulkanImage& depth, VulkanImage& velocity, VulkanImage* previousPicture, VulkanReflectionProbe& probe,
                                const GpuReflectionPass& pass) {
    VulkanBuffer& record = passRecords.at(frameIndex);
    std::memcpy(record.getMapped(), &pass, sizeof(pass));
    record.flush();

    buildDepthPyramid(commands, descriptors, depth);

    if (previousPicture != nullptr) {
        buildColourPyramid(commands, descriptors, *previousPicture);
    } else {
        colourPyramid.transition(commands, ImageState::ComputeRead, ImageContents::Discard);
    }

    const VkImageView probeLight = probe.readableLight(commands);
    const VkImageView probeDistances = probe.readableDistances(commands);
    normals.transition(commands, ImageState::ComputeRead);
    properties.transition(commands, ImageState::ComputeRead);
    standIns.transition(commands, ImageState::ComputeRead);
    velocity.transition(commands, ImageState::ComputeRead);
    scene.transition(commands, ImageState::ComputeStorage);

    const std::array<ComputeBinding, 10> bindings{
        storageBufferBinding(record.get()),
        sampledBinding(nearest, depthPyramid.getView()),
        sampledBinding(pyramidSampler, colourPyramid.getView()),
        sampledBinding(nearest, normals.getView()),
        sampledBinding(nearest, properties.getView()),
        sampledBinding(nearest, standIns.getView()),
        sampledBinding(nearest, velocity.getView()),
        sampledBinding(pyramidSampler, probeLight),
        sampledBinding(nearestLevels, probeDistances),
        storageImageBinding(scene.getView()),
    };

    bindComputeSet(commands, device.getDevice(), descriptors, *reflectionPipeline, bindings);
    vkCmdDispatch(commands, workgroupCount(size.width, SSR_GROUP_SIDE), workgroupCount(size.height, SSR_GROUP_SIDE), 1);

    scene.transition(commands, ImageState::SurfaceRead);
}
