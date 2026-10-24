#include "render/vulkan/vulkan_opaque_picture.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/vulkan_device.h"

#include <array>

VulkanOpaquePicture::VulkanOpaquePicture(const VulkanDevice& device, VkSampler nearest, VkSampler linear, VkSampler levels):
    device(device),
    nearest(nearest),
    levelsSampler(levels)
{
    ComputePipelineDescription description;
    description.shader = "assets/shaders/opaque_picture.comp.spv";
    description.sampledImages = 2;
    description.storageImages = 1;
    description.pushConstantBytes = sizeof(GpuOpaquePicturePass);
    pipeline = std::make_unique<ComputePipeline>(device, description);
    pyramid = std::make_unique<VulkanColourPyramid>(device, linear, "Opaque picture");
}

void VulkanOpaquePicture::ensureSize(RenderSize render) {
    pyramid->ensureSize(render);
    size = render;
}

/*
* The barriers: the scene image from being drawn into or read to being read by compute, the depth from the depth
* target to being read by compute, and the picture from being read by last frame's glass and mask to being written,
* its old contents discarded (every texel of every level is written); level 0 is made visible to the levels' pass,
* which leaves the whole picture readable by fragment and compute shaders.
*/
void VulkanOpaquePicture::keep(VkCommandBuffer commands, FrameDescriptors& descriptors, VulkanImage& scene, VulkanImage& depth,
                               const GpuOpaquePicturePass& pass) {
    VulkanImage& picture = pyramid->getImage();
    scene.transition(commands, ImageState::ComputeRead);
    depth.transition(commands, ImageState::DepthComputeRead);
    picture.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    const std::array<ComputeBinding, 3> bindings{
        sampledBinding(nearest, scene.getView()),
        sampledBinding(nearest, depth.getView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL),
        storageImageBinding(pyramid->getLevelView(0)),
    };

    bindComputeSet(commands, device.getDevice(), descriptors, *pipeline, bindings);
    vkCmdPushConstants(commands, pipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pass), &pass);
    vkCmdDispatch(commands, workgroupCount(size.width, TRANSPARENCY_PICTURE_GROUP_SIDE), workgroupCount(size.height, TRANSPARENCY_PICTURE_GROUP_SIDE), 1);
    computeLevelWritten(commands, picture, 0);

    pyramid->build(commands, descriptors, nullptr, ImageState::SurfaceRead);
}

SampledImage VulkanOpaquePicture::sampled() const {
    return SampledImage{pyramid->getImage().getView(), levelsSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

VulkanImage& VulkanOpaquePicture::getImage() {
    return pyramid->getImage();
}

std::uint32_t VulkanOpaquePicture::getLevels() const {
    return pyramid->getLevels();
}
