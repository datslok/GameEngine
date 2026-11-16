#include "render/vulkan/vulkan_wind.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/vulkan_device.h"

namespace {
    // Displacements and velocities of a few tenths at most: well inside a half float's range and digits.
    constexpr VkFormat springFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    constexpr std::uint32_t springLayers = static_cast<std::uint32_t>(WIND_CASCADES * WIND_CATEGORIES);

    VulkanImage springImage(const VulkanDevice& device) {
        ImageDescription description;
        description.format = springFormat;
        description.width = WIND_CELLS;
        description.height = WIND_CELLS;
        description.layers = springLayers;
        description.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        description.name = "Wind springs";
        return VulkanImage{device, description};
    }
}

/*
* The pass reads its own set: the record, last frame's springs (exact texels through texelFetch) and this frame's to
* write. Both images start cleared to rest and readable, so frames without wind bind springs at rest.
*/
VulkanWind::VulkanWind(const VulkanDevice& device, ImmediateCommands& uploads, VkSampler wrapping):
    device(device),
    wrapping(wrapping)
{
    ComputePipelineDescription description;
    description.shader = "assets/shaders/wind_field.comp.spv";
    description.storageBuffers = 1;
    description.sampledImages = 1;
    description.storageImages = 1;
    pipeline = std::make_unique<ComputePipeline>(device, description);

    springs[0] = springImage(device);
    springs[1] = springImage(device);

    uploads.run([&](VkCommandBuffer commands) {
        const VkClearColorValue rest{{0.0f, 0.0f, 0.0f, 0.0f}};
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = springLayers;

        for (VulkanImage& image : springs) {
            image.transition(commands, ImageState::TransferDestination, ImageContents::Discard);
            vkCmdClearColorImage(commands, image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &rest, 1, &range);
            image.transition(commands, ImageState::VertexRead);
        }
    });
}

GpuWindField VulkanWind::prepare(const Wind* wind, const WindSettings& settings, const Vec3& cameraOrigin, float frameSeconds) {
    validateWind(settings);
    runsThisFrame = wind != nullptr && settings.enabled;
    const Vec2 place{cameraOrigin.x, cameraOrigin.z};
    const std::array<WindWindow, WIND_CASCADES> windows{windWindowAround(place, settings.nearCellMetres), windWindowAround(place, settings.farCellMetres)};

    if (!runsThisFrame) {
        previousKnown = false;
        return packWindField(wind, settings, windows, windows, false, seconds, 0.0f);
    }

    // This frame's springs go into the other image, turned now: the scene set, written before the pass runs, binds
    // them as this frame's and the one written last frame as last frame's.
    current = 1 - current;
    seconds += frameSeconds;
    const GpuWindField field = packWindField(wind, settings, windows, previousWindows, previousKnown, seconds, frameSeconds);
    previousWindows = windows;
    previousKnown = true;
    return field;
}

bool VulkanWind::runs() const {
    return runsThisFrame;
}

/*
* The barriers: this frame's image from being read last frame (by the draws, as last frame's springs) to being written,
* its old contents discarded (every texel is written); last frame's is already readable by compute (VertexRead covers
* the compute stage). After the pass, this frame's is readable by the vertex shaders and the next frame's pass.
*/
void VulkanWind::step(VkCommandBuffer commands, FrameDescriptors& descriptors, VkBuffer record) {
    VulkanImage& written = springs[static_cast<std::size_t>(current)];
    const VulkanImage& history = springs[static_cast<std::size_t>(1 - current)];

    written.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    const std::array<ComputeBinding, 3> bindings{
        storageBufferBinding(record),
        sampledBinding(wrapping, history.getView()),
        storageImageBinding(written.getView()),
    };

    bindComputeSet(commands, device.getDevice(), descriptors, *pipeline, bindings);
    vkCmdDispatch(commands, workgroupCount(WIND_CELLS, WIND_GROUP_SIDE), workgroupCount(WIND_CELLS, WIND_GROUP_SIDE), springLayers);

    written.transition(commands, ImageState::VertexRead);
}

SampledImage VulkanWind::sampledSprings() const {
    return SampledImage{springs[static_cast<std::size_t>(current)].getView(), wrapping, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

SampledImage VulkanWind::sampledPreviousSprings() const {
    return SampledImage{springs[static_cast<std::size_t>(1 - current)].getView(), wrapping, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}
