#include "render/vulkan/vulkan_fog.h"
#include "render/gpu/compute_dispatch.h"
#include "render/gpu/taa.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/scene_layout.h"
#include "render/vulkan/vulkan_device.h"

#include <algorithm>
#include <array>

namespace {
    // Light and transmittance in half floats: the fog's light spans the lamps' brightness, its extinction 1e-4 to 1 per
    // metre, both well inside a half float's range and digits after the frames' averaging.
    constexpr VkFormat fogFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    VulkanImage fogVolume(const VulkanDevice& device, std::uint32_t width, std::uint32_t height, std::uint32_t depth, VkImageUsageFlags usage,
                          const char* name) {
        ImageDescription description;
        description.format = fogFormat;
        description.width = width;
        description.height = height;
        description.depth = depth;
        description.volume = true;
        description.usage = usage;
        description.name = name;
        return VulkanImage{device, description};
    }
}

/*
* Each pass reads the scene's two sets and then its own set 2: the lighting pass last frame's grid (filtered) and this
* frame's to write; the integration this frame's lighting grid (exact texels) and the integrated grid to write; the
* picture pass the depth and the scene image, with its camera and size as push constants. The stand-in is cleared to no
* light and everything let through, and left readable.
*/
VulkanFog::VulkanFog(const VulkanDevice& device, ImmediateCommands& uploads, VkSampler linear, VkSampler nearest, VkDescriptorSetLayout texturesLayout,
                     VkDescriptorSetLayout frameLayout):
    device(device),
    linear(linear),
    nearest(nearest)
{
    ComputePipelineDescription light;
    light.shader = "assets/shaders/fog_light.comp.spv";
    light.sharedSets = {texturesLayout, frameLayout};
    light.sampledImages = 1;
    light.storageImages = 1;
    lightPipeline = std::make_unique<ComputePipeline>(device, light);

    ComputePipelineDescription integrate = light;
    integrate.shader = "assets/shaders/fog_integrate.comp.spv";
    integratePipeline = std::make_unique<ComputePipeline>(device, integrate);

    ComputePipelineDescription apply = light;
    apply.shader = "assets/shaders/fog_apply.comp.spv";
    apply.pushConstantBytes = sizeof(GpuFogPicturePass);
    applyPipeline = std::make_unique<ComputePipeline>(device, apply);

    clear = fogVolume(device, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "Fog stand-in");

    uploads.run([&](VkCommandBuffer commands) {
        clear.transition(commands, ImageState::TransferDestination, ImageContents::Discard);
        const VkClearColorValue nothing{{0.0f, 0.0f, 0.0f, 1.0f}};
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        vkCmdClearColorImage(commands, clear.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &nothing, 1, &range);
        clear.transition(commands, ImageState::SurfaceRead);
    });
}

// The frames in flight may still read the old grid, so the GPU is waited for first (only when the size changes).
void VulkanFog::ensureGrid(const FogSettings& settings) {
    if (integrated && width == settings.gridWidth && height == settings.gridHeight && slices == settings.slices) {
        return;
    }

    device.waitIdle();
    const auto across = static_cast<std::uint32_t>(settings.gridWidth);
    const auto down = static_cast<std::uint32_t>(settings.gridHeight);
    const auto deep = static_cast<std::uint32_t>(settings.slices);
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    lighting[0] = fogVolume(device, across, down, deep, usage, "Fog lighting");
    lighting[1] = fogVolume(device, across, down, deep, usage, "Fog lighting");
    integrated = fogVolume(device, across, down, deep, usage, "Fog integrated");
    width = settings.gridWidth;
    height = settings.gridHeight;
    slices = settings.slices;
    previousKnown = false;
}

GpuFog VulkanFog::prepare(const Fog* fog, const FogSettings& settings, const Mat4& viewProjection, const Vec3& cameraOrigin, std::uint64_t frame,
                          float cameraFar) {
    validateFog(settings);
    fogThisFrame = fog != nullptr && settings.enabled;

    if (!fogThisFrame) {
        previousKnown = false;
        return packFog(nullptr, settings, viewProjection, viewProjection, false, frame, cameraFar);
    }

    ensureGrid(settings);
    const float range = std::min(settings.range, cameraFar);
    const bool historyUsable = previousKnown && previousRange == range;
    const Mat4 previous = historyUsable ? previousRelativeViewProjection(previousViewProjection, previousOrigin, cameraOrigin) : viewProjection;
    const GpuFog packed = packFog(fog, settings, viewProjection, previous, historyUsable, frame, cameraFar);

    previousViewProjection = viewProjection;
    previousOrigin = cameraOrigin;
    previousRange = range;
    previousKnown = true;
    return packed;
}

bool VulkanFog::drawsFog() const {
    return fogThisFrame;
}

SampledImage VulkanFog::sampledVolume() const {
    const VulkanImage& volume = fogThisFrame ? integrated : clear;
    return SampledImage{volume.getView(), linear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

void VulkanFog::bindScene(VkCommandBuffer commands, const ComputePipeline& pipeline, VkDescriptorSet textures, VkDescriptorSet frameSet) const {
    const std::array<VkDescriptorSet, 2> sets{textures, frameSet};
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getLayout(), bindlessTextureSet, static_cast<std::uint32_t>(sets.size()),
                            sets.data(), 0, nullptr);
}

/*
* The barriers: this frame's lighting grid from being read last frame (as the history, or by the integration) to being
* written, its old contents discarded (every froxel is written); last frame's, already readable by compute, read as the
* history (a grid never written yet is undefined, but the first frame keeps none of it: its weight is 0, and it is
* moved to readable so the descriptor's layout holds). Then the lighting grid to readable and the integrated grid from
* being read last frame (by the picture pass and glass) to being written, and after it readable by both.
*/
void VulkanFog::light(VkCommandBuffer commands, FrameDescriptors& descriptors, VkDescriptorSet textures, VkDescriptorSet frameSet) {
    current = 1 - current;
    VulkanImage& written = lighting[static_cast<std::size_t>(current)];
    VulkanImage& history = lighting[static_cast<std::size_t>(1 - current)];

    if (history.getState() == ImageState::Undefined) {
        history.transition(commands, ImageState::ComputeRead, ImageContents::Discard);
    }

    written.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    const std::array<ComputeBinding, 2> lightBindings{
        sampledBinding(linear, history.getView()),
        storageImageBinding(written.getView()),
    };

    bindScene(commands, *lightPipeline, textures, frameSet);
    bindComputeSet(commands, device.getDevice(), descriptors, *lightPipeline, lightBindings);
    const auto across = static_cast<std::uint32_t>(width);
    const auto down = static_cast<std::uint32_t>(height);
    const auto deep = static_cast<std::uint32_t>(slices);
    vkCmdDispatch(commands, workgroupCount(across, FOG_LIGHT_GROUP_SIDE), workgroupCount(down, FOG_LIGHT_GROUP_SIDE),
                  workgroupCount(deep, FOG_LIGHT_GROUP_SIDE));

    written.transition(commands, ImageState::ComputeRead);
    integrated.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    const std::array<ComputeBinding, 2> integrateBindings{
        sampledBinding(nearest, written.getView()),
        storageImageBinding(integrated.getView()),
    };

    bindScene(commands, *integratePipeline, textures, frameSet);
    bindComputeSet(commands, device.getDevice(), descriptors, *integratePipeline, integrateBindings);
    vkCmdDispatch(commands, workgroupCount(across, FOG_COLUMN_GROUP_SIDE), workgroupCount(down, FOG_COLUMN_GROUP_SIDE), 1);

    integrated.transition(commands, ImageState::SurfaceRead);
}

/*
* The barriers: the depth to being read by compute (kept), and the scene image from wherever it was left (drawn into by
* the main pass, or changed in place by the reflections) to being read and written in place, kept; the glass and the
* anti-aliasing after it move it on themselves.
*/
void VulkanFog::apply(VkCommandBuffer commands, FrameDescriptors& descriptors, VkDescriptorSet textures, VkDescriptorSet frameSet, VulkanImage& picture,
                      VulkanImage& depth, const GpuFogPicturePass& pass) {
    depth.transition(commands, ImageState::DepthComputeRead);
    picture.transition(commands, ImageState::ComputeStorage);

    const std::array<ComputeBinding, 2> bindings{
        sampledBinding(nearest, depth.getView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL),
        storageImageBinding(picture.getView()),
    };

    bindScene(commands, *applyPipeline, textures, frameSet);
    bindComputeSet(commands, device.getDevice(), descriptors, *applyPipeline, bindings);
    vkCmdPushConstants(commands, applyPipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pass), &pass);
    vkCmdDispatch(commands, workgroupCount(static_cast<std::uint32_t>(pass.size[0]), FOG_APPLY_GROUP_SIDE),
                  workgroupCount(static_cast<std::uint32_t>(pass.size[1]), FOG_APPLY_GROUP_SIDE), 1);
}
