#include "render/vulkan/vulkan_motion_blur.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/vulkan_device.h"

#include <array>
#include <cstring>

namespace {
    // The picture's light and the prepared half-blur and view depth: rgba16f, a storage format every Vulkan GPU writes.
    constexpr VkFormat pictureFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    // The tiles' half-blurs: rg32f, a storage format every Vulkan GPU writes (rg16f needs the extended storage formats).
    constexpr VkFormat tileFormat = VK_FORMAT_R32G32_SFLOAT;
}

VulkanMotionBlur::VulkanMotionBlur(const VulkanDevice& device, VkSampler nearest, std::uint32_t framesInFlight):
    device(device),
    nearest(nearest)
{
    ComputePipelineDescription tilesPass;
    tilesPass.shader = "assets/shaders/motion_blur_tiles.comp.spv";
    tilesPass.storageBuffers = 1;
    tilesPass.sampledImages = 2;
    tilesPass.storageImages = 2;
    tilesPipeline = std::make_unique<ComputePipeline>(device, tilesPass);

    ComputePipelineDescription neighboursPass;
    neighboursPass.shader = "assets/shaders/motion_blur_neighbours.comp.spv";
    neighboursPass.sampledImages = 1;
    neighboursPass.storageImages = 1;
    neighboursPipeline = std::make_unique<ComputePipeline>(device, neighboursPass);

    ComputePipelineDescription gatherPass;
    gatherPass.shader = "assets/shaders/motion_blur.comp.spv";
    gatherPass.sampledImages = 3;
    gatherPass.storageImages = 1;
    gatherPipeline = std::make_unique<ComputePipeline>(device, gatherPass);

    setFramesInFlight(framesInFlight);
}

void VulkanMotionBlur::setFramesInFlight(std::uint32_t count) {
    passRecords.clear();

    for (std::uint32_t frame = 0; frame < count; ++frame) {
        passRecords.emplace_back(device.getAllocator(), sizeof(GpuMotionBlurPass), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload);
    }
}

void VulkanMotionBlur::ensureSize(RenderSize output) {
    if (fits(output)) {
        return;
    }

    const auto image = [&](const char* name, VkFormat format, RenderSize imageSize) {
        ImageDescription description;
        description.format = format;
        description.width = imageSize.width;
        description.height = imageSize.height;
        description.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        description.name = name;
        return VulkanImage{device, description};
    };

    const RenderSize tileGrid = motionBlurTiles(output);
    prepared = image("Motion blur, prepared", pictureFormat, output);
    tiles = image("Motion blur tiles", tileFormat, tileGrid);
    neighbours = image("Motion blur tile neighbourhoods", tileFormat, tileGrid);
    blurred = image("Motion blur", pictureFormat, output);
    size = output;
}

bool VulkanMotionBlur::fits(RenderSize output) const {
    return static_cast<bool>(blurred) && size == output;
}

/*
* The barriers: the velocity image from being read by the anti-aliasing (or written by the camera motion pass) to being
* read by compute; the depth still readable by compute; the picture from being written by the anti-aliasing to being read
* by compute; each pass's images from being read last frame to being written (old contents discarded, every texel
* written), then read by the next pass; the result left readable by fragment and compute shaders.
*/
VulkanImage& VulkanMotionBlur::blur(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& picture, VulkanImage& velocity,
                                    VulkanImage& depth, const GpuMotionBlurPass& pass) {
    VulkanBuffer& record = passRecords.at(frameIndex);
    std::memcpy(record.getMapped(), &pass, sizeof(pass));
    record.flush();

    velocity.transition(commands, ImageState::ComputeRead);
    depth.transition(commands, ImageState::DepthComputeRead);
    picture.transition(commands, ImageState::ComputeRead);
    prepared.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    tiles.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    const RenderSize tileGrid = motionBlurTiles(size);

    const std::array<ComputeBinding, 5> tilesBindings{
        storageBufferBinding(record.get()),
        sampledBinding(nearest, velocity.getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        sampledBinding(nearest, depth.getView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL),
        storageImageBinding(prepared.getView()),
        storageImageBinding(tiles.getView()),
    };
    bindComputeSet(commands, device.getDevice(), descriptors, *tilesPipeline, tilesBindings);
    vkCmdDispatch(commands, tileGrid.width, tileGrid.height, 1);

    tiles.transition(commands, ImageState::ComputeRead);
    neighbours.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    const std::array<ComputeBinding, 2> neighboursBindings{
        sampledBinding(nearest, tiles.getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        storageImageBinding(neighbours.getView()),
    };
    bindComputeSet(commands, device.getDevice(), descriptors, *neighboursPipeline, neighboursBindings);
    vkCmdDispatch(commands, workgroupCount(tileGrid.width, MOTION_BLUR_GROUP), workgroupCount(tileGrid.height, MOTION_BLUR_GROUP), 1);

    prepared.transition(commands, ImageState::ComputeRead);
    neighbours.transition(commands, ImageState::ComputeRead);
    blurred.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    const std::array<ComputeBinding, 4> gatherBindings{
        sampledBinding(nearest, picture.getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        sampledBinding(nearest, prepared.getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        sampledBinding(nearest, neighbours.getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        storageImageBinding(blurred.getView()),
    };
    bindComputeSet(commands, device.getDevice(), descriptors, *gatherPipeline, gatherBindings);
    vkCmdDispatch(commands, workgroupCount(size.width, MOTION_BLUR_GROUP), workgroupCount(size.height, MOTION_BLUR_GROUP), 1);

    blurred.transition(commands, ImageState::SurfaceRead);
    picture.transition(commands, ImageState::SurfaceRead);
    return blurred;
}
