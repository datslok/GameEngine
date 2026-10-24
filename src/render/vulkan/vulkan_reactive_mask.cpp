#include "render/vulkan/vulkan_reactive_mask.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/vulkan_device.h"

#include <array>

namespace {
    // A share, 0 to 1, as a half float: Streamline's Vulkan format table (DLSS's bias hint) has no 8-bit unsigned
    // normalised single channel, and refuses an image in a format it cannot name.
    constexpr VkFormat maskFormat = VK_FORMAT_R16_SFLOAT;
}

VulkanReactiveMask::VulkanReactiveMask(const VulkanDevice& device, VkSampler nearest):
    device(device),
    nearest(nearest)
{
    ComputePipelineDescription description;
    description.shader = "assets/shaders/reactive_mask.comp.spv";
    description.sampledImages = 2;
    description.storageImages = 1;
    description.pushConstantBytes = sizeof(GpuReactivePass);
    pipeline = std::make_unique<ComputePipeline>(device, description);
}

void VulkanReactiveMask::ensureSize(RenderSize render) {
    if (size == render && mask) {
        return;
    }

    ImageDescription reactive;
    reactive.format = maskFormat;
    reactive.width = render.width;
    reactive.height = render.height;
    // Transfer destination: cleared in frames without glass; sampled: the resolve, FSR and DLSS read it.
    reactive.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    reactive.name = "Reactive mask";
    mask = VulkanImage{device, reactive};

    size = render;
    cleared = false;
}

/*
* The barriers: the scene image from the glass pass's colour target (left readable for fragment and compute shaders) to
* being read by compute (the picture before the glass already is), and the mask from being read by last frame's anti-aliasing to being written, its old contents
* discarded, every pixel being written; after it, the mask to being read by compute.
*/
void VulkanReactiveMask::measure(VkCommandBuffer commands, FrameDescriptors& descriptors, VulkanImage& opaque, VulkanImage& scene,
                                 const GpuReactivePass& pass) {
    scene.transition(commands, ImageState::SurfaceRead);
    opaque.transition(commands, ImageState::SurfaceRead);
    mask.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    const std::array<ComputeBinding, 3> bindings{
        sampledBinding(nearest, opaque.getView()),
        sampledBinding(nearest, scene.getView()),
        storageImageBinding(mask.getView()),
    };

    bindComputeSet(commands, device.getDevice(), descriptors, *pipeline, bindings);
    vkCmdPushConstants(commands, pipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pass), &pass);
    vkCmdDispatch(commands, workgroupCount(size.width, TRANSPARENCY_REACTIVE_GROUP_SIDE), workgroupCount(size.height, TRANSPARENCY_REACTIVE_GROUP_SIDE), 1);

    mask.transition(commands, ImageState::ComputeRead);
    cleared = false;
}

// The barriers: the mask from being read by last frame's anti-aliasing to being cleared, then to being read by compute.
void VulkanReactiveMask::clear(VkCommandBuffer commands) {
    if (cleared) {
        return;
    }

    mask.transition(commands, ImageState::TransferDestination, ImageContents::Discard);
    const VkClearColorValue zero{};
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(commands, mask.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    mask.transition(commands, ImageState::ComputeRead);
    cleared = true;
}

VulkanImage& VulkanReactiveMask::getMask() {
    return mask;
}
