#include "render/vulkan/vulkan_colour_pyramid.h"
#include "render/gpu/compute_dispatch.h"
#include "render/gpu/screen_reflections.h"
#include "render/vulkan/vulkan_device.h"

#include <array>

namespace {
    // The scene's own 16-bit floats.
    constexpr VkFormat pyramidFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
}

VulkanColourPyramid::VulkanColourPyramid(const VulkanDevice& device, VkSampler linear, const char* name):
    device(device),
    linear(linear),
    name(name)
{
    ComputePipelineDescription description;
    description.shader = "assets/shaders/colour_pyramid.comp.spv";
    description.sampledImages = 1;
    description.storageImages = 1;
    description.pushConstantBytes = sizeof(GpuPyramidPass);
    pipeline = std::make_unique<ComputePipeline>(device, description);
}

void VulkanColourPyramid::ensureSize(RenderSize render) {
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
* Level 0 from the picture (sampled by compute, any size), each level above from the one below, sampled in the general
* layout. The pyramid starts with its old contents discarded, all of it in the general layout (unless level 0 was just
* written there), and each level is made visible to the next dispatch.
*/
void VulkanColourPyramid::build(VkCommandBuffer commands, FrameDescriptors& descriptors, VulkanImage* picture, ImageState finished) {
    std::uint32_t first = 1;

    if (picture != nullptr) {
        picture->transition(commands, ImageState::ComputeRead);
        image.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
        first = 0;
    }

    for (std::uint32_t level = first; level < levels.size(); ++level) {
        const RenderSize target = pyramidLevelSize(size, level);
        const RenderSize source = level == 0 ? RenderSize{picture->getDescription().width, picture->getDescription().height}
                                             : pyramidLevelSize(size, level - 1);
        const std::array<ComputeBinding, 2> bindings{
            level == 0 ? sampledBinding(linear, picture->getView()) : sampledBinding(linear, levels[level - 1].get(), VK_IMAGE_LAYOUT_GENERAL),
            storageImageBinding(levels[level].get()),
        };

        bindComputeSet(commands, device.getDevice(), descriptors, *pipeline, bindings);
        const GpuPyramidPass pass = packPyramidPass(target, level, source);
        vkCmdPushConstants(commands, pipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pass), &pass);
        vkCmdDispatch(commands, workgroupCount(target.width, SSR_GROUP_SIDE), workgroupCount(target.height, SSR_GROUP_SIDE), 1);
        computeLevelWritten(commands, image, level);
    }

    image.transition(commands, finished);
}

VulkanImage& VulkanColourPyramid::getImage() {
    return image;
}

VkImageView VulkanColourPyramid::getLevelView(std::uint32_t level) const {
    return levels.at(level).get();
}

std::uint32_t VulkanColourPyramid::getLevels() const {
    return static_cast<std::uint32_t>(levels.size());
}
