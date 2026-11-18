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

    // The reflection pass's own set: its record, nine images it samples and the scene image it changes.
    ComputePipelineDescription reflectionPass(const char* shader) {
        ComputePipelineDescription reflections;
        reflections.shader = shader;
        reflections.storageBuffers = 1;
        reflections.sampledImages = 10;
        reflections.storageImages = 1;
        return reflections;
    }
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

    depthPyramid = std::make_unique<VulkanDepthPyramid>(device, nearest, DepthReduction::Nearest, "Depth pyramid");

    colourPyramid = std::make_unique<VulkanColourPyramid>(device, linear, "Colour pyramid");
    reflectionPipeline = std::make_unique<ComputePipeline>(device, reflectionPass("assets/shaders/reflections.comp.spv"));

    setFramesInFlight(framesInFlight);
}

void VulkanReflections::enableRayTracing(VkDescriptorSetLayout textures, VkDescriptorSetLayout scene, VkDescriptorSetLayout rayScene) {
    ComputePipelineDescription rays = reflectionPass("assets/shaders/reflections_rays.comp.spv");
    rays.sharedSets = {textures, scene, rayScene};
    rayReflectionPipeline = std::make_unique<ComputePipeline>(device, rays);
}

bool VulkanReflections::tracesRays() const {
    return rayReflectionPipeline != nullptr;
}

void VulkanReflections::setFramesInFlight(std::uint32_t count) {
    passRecords.clear();

    for (std::uint32_t frame = 0; frame < count; ++frame) {
        passRecords.emplace_back(device.getAllocator(), sizeof(GpuReflectionPass), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload);
    }
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

    depthPyramid->ensureSize(render);
    colourPyramid->ensureSize(render);
    size = render;
}

std::vector<VkFormat> VulkanReflections::surfaceFormats() const {
    return {normalFormat, propertiesFormat, standInFormat};
}

void VulkanReflections::prepareColourPyramid(VkCommandBuffer commands, FrameDescriptors& descriptors, VulkanImage& previousPicture) {
    colourPyramid->build(commands, descriptors, &previousPicture, ImageState::ComputeRead);
    colourPyramidPrepared = true;
}

void VulkanReflections::forgetColourPyramid() {
    colourPyramidPrepared = false;
}

SampledImage VulkanReflections::sampledColourPyramid() const {
    return SampledImage{colourPyramid->getImage().getView(), pyramidSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

std::uint32_t VulkanReflections::getPyramidLevels() const {
    return depthPyramid->getLevels();
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
* The barriers, besides the pyramids': the surfaces and the velocity image from being drawn into to being read by
* compute, and the scene image from being read (it was left readable by the main pass) to being read and written in
* place, and back to being readable by fragment and compute shaders afterwards. Without last frame's picture the colour
* pyramid is not built, only made bindable (its contents discarded: the pass does not read it then).
*/
void VulkanReflections::reflect(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& scene,
                                VulkanImage& depth, VulkanImage& velocity, VulkanImage* previousPicture, VulkanReflectionProbe& probe,
                                VulkanImage& planarPicture, bool planarDrawn, const GpuReflectionPass& pass, const ReflectionRaySets* rays) {
    VulkanBuffer& record = passRecords.at(frameIndex);
    std::memcpy(record.getMapped(), &pass, sizeof(pass));
    record.flush();

    depthPyramid->build(commands, descriptors, depth);
    const bool prepared = colourPyramidPrepared;
    colourPyramidPrepared = false;

    // Last frame's picture with its blurred levels, readable by the reflection pass at the end (built already when the
    // ambient occlusion pass gathered from it).
    if (previousPicture != nullptr && prepared) {
        colourPyramid->getImage().transition(commands, ImageState::ComputeRead);
    } else if (previousPicture != nullptr) {
        colourPyramid->build(commands, descriptors, previousPicture, ImageState::ComputeRead);
    } else {
        colourPyramid->getImage().transition(commands, ImageState::ComputeRead, ImageContents::Discard);
    }

    // The mirror's picture is bound even while unused, its contents discarded as the colour pyramid's are.
    if (!planarDrawn) {
        planarPicture.transition(commands, ImageState::ComputeRead, ImageContents::Discard);
    }

    const VkImageView probeLight = probe.readableLight(commands);
    const VkImageView probeDistances = probe.readableDistances(commands);
    normals.transition(commands, ImageState::ComputeRead);
    properties.transition(commands, ImageState::ComputeRead);
    standIns.transition(commands, ImageState::ComputeRead);
    velocity.transition(commands, ImageState::ComputeRead);
    scene.transition(commands, ImageState::ComputeStorage);

    const std::array<ComputeBinding, 12> bindings{
        storageBufferBinding(record.get()),
        sampledBinding(nearest, depthPyramid->getImage().getView()),
        sampledBinding(pyramidSampler, colourPyramid->getImage().getView()),
        sampledBinding(nearest, normals.getView()),
        sampledBinding(nearest, properties.getView()),
        sampledBinding(nearest, standIns.getView()),
        sampledBinding(nearest, velocity.getView()),
        sampledBinding(pyramidSampler, probeLight),
        sampledBinding(nearestLevels, probeDistances),
        sampledBinding(pyramidSampler, probeDistances),
        sampledBinding(pyramidSampler, planarPicture.getView()),
        storageImageBinding(scene.getView()),
    };

    const bool traced = rays != nullptr && rayReflectionPipeline;
    const ComputePipeline& pipeline = traced ? *rayReflectionPipeline : *reflectionPipeline;
    bindComputeSet(commands, device.getDevice(), descriptors, pipeline, bindings);

    if (traced) {
        const std::array<VkDescriptorSet, 3> sets{rays->textures, rays->scene, rays->rayScene};
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getLayout(), 0, static_cast<std::uint32_t>(sets.size()), sets.data(), 0,
                                nullptr);
    }

    vkCmdDispatch(commands, workgroupCount(size.width, SSR_GROUP_SIDE), workgroupCount(size.height, SSR_GROUP_SIDE), 1);

    scene.transition(commands, ImageState::SurfaceRead);
}
