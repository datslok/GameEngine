#include "render/vulkan/vulkan_planar_reflection.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/vulkan_device.h"

VulkanPlanarReflection::VulkanPlanarReflection(const VulkanDevice& device, VkSampler linear, VkFormat colourFormat, VkFormat depthFormat):
    device(device),
    colourFormat(colourFormat),
    depthFormat(depthFormat),
    pyramid(device, linear, "Planar reflection levels")
{
}

void VulkanPlanarReflection::ensureSize(RenderSize render) {
    if (size == render && colour) {
        return;
    }

    ImageDescription colourDescription;
    colourDescription.format = colourFormat;
    colourDescription.width = render.width;
    colourDescription.height = render.height;
    colourDescription.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    colourDescription.name = "Planar reflection";
    colour = VulkanImage{device, colourDescription};

    ImageDescription depthDescription;
    depthDescription.format = depthFormat;
    depthDescription.width = render.width;
    depthDescription.height = render.height;
    depthDescription.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthDescription.name = "Planar reflection depth";
    depth = VulkanImage{device, depthDescription};

    pyramid.ensureSize(render);
    size = render;
}

RenderSize VulkanPlanarReflection::getSize() const {
    return size;
}

void VulkanPlanarReflection::beginDraw(VkCommandBuffer commands) {
    colour.transition(commands, ImageState::ColourTarget, ImageContents::Discard);
    depth.transition(commands, ImageState::DepthTarget, ImageContents::Discard);
}

ColourAttachment VulkanPlanarReflection::colourTarget(const Vec3& background) const {
    return ColourAttachment{colour.getView(), VK_ATTACHMENT_LOAD_OP_CLEAR, {background.x, background.y, background.z, 1.0f}};
}

DepthAttachment VulkanPlanarReflection::depthTarget() const {
    return DepthAttachment{depth.getView(), VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, gpuFarDepth};
}

/*
* The barrier: the picture from being drawn into to being read by compute, for the levels; the pyramid's own passes leave
* it all readable by compute too, for the reflection pass.
*/
void VulkanPlanarReflection::finishDraw(VkCommandBuffer commands, FrameDescriptors& descriptors) {
    colour.transition(commands, ImageState::ComputeRead);
    pyramid.build(commands, descriptors, &colour, ImageState::ComputeRead);
}

VulkanImage& VulkanPlanarReflection::getPicture() {
    return pyramid.getImage();
}

std::uint32_t VulkanPlanarReflection::getLevels() const {
    return pyramid.getLevels();
}
