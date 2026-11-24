#include "render/vulkan/vulkan_depth_pyramid.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/vulkan_device.h"

#include <array>

namespace {
    // The depth image's own 32-bit floats.
    constexpr VkFormat pyramidFormat = VK_FORMAT_R32_SFLOAT;
}

VulkanDepthPyramid::VulkanDepthPyramid(const VulkanDevice& device, VkSampler nearest, DepthReduction reduction, const char* name):
    device(device),
    nearest(nearest),
    reduction(reduction),
    name(name)
{
    ComputePipelineDescription description;
    description.shader = "assets/shaders/depth_pyramid.comp.spv";
    description.sampledImages = 1;
    description.storageImages = 2;
    description.pushConstantBytes = sizeof(GpuPyramidPass);
    pipeline = std::make_unique<ComputePipeline>(device, description);
}

void VulkanDepthPyramid::ensureSize(RenderSize render) {
    if (size == render && image) {
        return;
    }

    ImageDescription description;
    description.format = pyramidFormat;
    description.width = render.width;
    description.height = render.height;
    description.levels = depthPyramidLevels(render);
    description.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    description.name = name;
    image = VulkanImage{device, description};
    levels = imageLevelViews(device.getDevice(), image);
    size = render;
}

/*
* Level by level, each dispatch reading the level below as a storage image (level 0 reads the depth image), with a
* barrier after each so the next level sees it written.
*/
void VulkanDepthPyramid::build(VkCommandBuffer commands, FrameDescriptors& descriptors, VulkanImage& depth, ImageState readState) {
    depth.transition(commands, ImageState::DepthComputeRead);
    image.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);

    for (std::uint32_t level = 0; level < levels.size(); ++level) {
        const RenderSize target = pyramidLevelSize(size, level);
        const RenderSize source = pyramidLevelSize(size, level == 0 ? 0 : level - 1);
        const std::array<ComputeBinding, 3> bindings{
            sampledBinding(nearest, depth.getView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL),
            storageImageBinding(levels[level == 0 ? 0 : level - 1].get()),
            storageImageBinding(levels[level].get()),
        };

        bindComputeSet(commands, device.getDevice(), descriptors, *pipeline, bindings);
        const GpuPyramidPass pass = packPyramidPass(target, level, source, reduction);
        vkCmdPushConstants(commands, pipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pass), &pass);
        vkCmdDispatch(commands, workgroupCount(target.width, SSR_GROUP_SIDE), workgroupCount(target.height, SSR_GROUP_SIDE), 1);
        computeLevelWritten(commands, image, level);
    }

    image.transition(commands, readState);
}

VulkanImage& VulkanDepthPyramid::getImage() {
    return image;
}

std::uint32_t VulkanDepthPyramid::getLevels() const {
    return static_cast<std::uint32_t>(levels.size());
}
