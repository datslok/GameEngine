#include "render/vulkan/vulkan_depth_of_field.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/vulkan_device.h"

#include <array>
#include <cstring>

namespace {
    // The picture's light and the blur grid's layers: rgba16f, a storage format every Vulkan GPU writes.
    constexpr VkFormat pictureFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    // The tiles' near radii: r32f, a storage format every Vulkan GPU writes.
    constexpr VkFormat tileFormat = VK_FORMAT_R32_SFLOAT;

    // The focus buffer: one vec4, its x the inverse distance.
    constexpr VkDeviceSize focusBytes = 4 * sizeof(float);

    ComputePipelineDescription pass(const char* shader, std::uint32_t storageBuffers, std::uint32_t sampledImages, std::uint32_t storageImages) {
        ComputePipelineDescription description;
        description.shader = shader;
        description.storageBuffers = storageBuffers;
        description.sampledImages = sampledImages;
        description.storageImages = storageImages;
        return description;
    }
}

VulkanDepthOfField::VulkanDepthOfField(const VulkanDevice& device, VkSampler nearest, VkSampler linear, std::uint32_t framesInFlight):
    device(device),
    nearest(nearest),
    linear(linear),
    focus(device.getAllocator(), focusBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Gpu)
{
    focusPipeline = std::make_unique<ComputePipeline>(device, pass("assets/shaders/depth_of_field_focus.comp.spv", 2, 1, 0));
    preparePipeline = std::make_unique<ComputePipeline>(device, pass("assets/shaders/depth_of_field_prepare.comp.spv", 2, 2, 2));
    neighboursPipeline = std::make_unique<ComputePipeline>(device, pass("assets/shaders/depth_of_field_neighbours.comp.spv", 0, 1, 1));
    gatherPipeline = std::make_unique<ComputePipeline>(device, pass("assets/shaders/depth_of_field.comp.spv", 0, 2, 2));
    medianPipeline = std::make_unique<ComputePipeline>(device, pass("assets/shaders/depth_of_field_median.comp.spv", 0, 2, 2));
    compositePipeline = std::make_unique<ComputePipeline>(device, pass("assets/shaders/depth_of_field_composite.comp.spv", 2, 4, 1));
    setFramesInFlight(framesInFlight);
}

void VulkanDepthOfField::setFramesInFlight(std::uint32_t count) {
    passRecords.clear();

    for (std::uint32_t frame = 0; frame < count; ++frame) {
        passRecords.emplace_back(device.getAllocator(), sizeof(GpuDepthOfFieldPass), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload);
    }
}

void VulkanDepthOfField::ensureSize(RenderSize output) {
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

    const RenderSize grid = depthOfFieldGrid(output);
    const RenderSize tileGrid = depthOfFieldTiles(output);
    prepared = image("Depth of field, prepared", pictureFormat, grid);
    tiles = image("Depth of field tiles", tileFormat, tileGrid);
    neighbours = image("Depth of field tile neighbourhoods", tileFormat, tileGrid);
    farLayer = image("Depth of field, far layer", pictureFormat, grid);
    nearLayer = image("Depth of field, near layer", pictureFormat, grid);
    farFiltered = image("Depth of field, far layer's median", pictureFormat, grid);
    nearFiltered = image("Depth of field, near layer's median", pictureFormat, grid);
    result = image("Depth of field", pictureFormat, output);
    size = output;
}

bool VulkanDepthOfField::fits(RenderSize output) const {
    return static_cast<bool>(result) && size == output;
}

/*
* The barriers: the focus buffer from being read by last frame's passes to being read and written by the focus pass,
* then its write made visible to this frame's prepare and composite passes; the depth still readable by compute; the
* picture from being written by the anti-aliasing to being read by compute; each pass's images from being read last
* frame to being written (old contents discarded, every texel written), then read by the next pass; the result left
* readable by fragment and compute shaders.
*/
VulkanImage& VulkanDepthOfField::blur(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& picture, VulkanImage& depth,
                                      const GpuDepthOfFieldPass& passRecord) {
    VulkanBuffer& record = passRecords.at(frameIndex);
    std::memcpy(record.getMapped(), &passRecord, sizeof(passRecord));
    record.flush();

    depth.transition(commands, ImageState::DepthComputeRead);
    picture.transition(commands, ImageState::ComputeRead);
    const VkDevice handle = device.getDevice();
    const VkImageLayout readOnly = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const ComputeBinding depthBinding = sampledBinding(nearest, depth.getView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    const ComputeBinding pictureBinding = sampledBinding(nearest, picture.getView(), readOnly);

    bufferBarrier(commands, focus.get(), {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE},
                  {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
    const std::array<ComputeBinding, 3> focusBindings{storageBufferBinding(record.get()), storageBufferBinding(focus.get()), depthBinding};
    bindComputeSet(commands, handle, descriptors, *focusPipeline, focusBindings);
    vkCmdDispatch(commands, 1, 1, 1);
    bufferBarrier(commands, focus.get(), {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
                  {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT});

    prepared.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    tiles.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    const RenderSize grid = depthOfFieldGrid(size);
    const RenderSize tileGrid = depthOfFieldTiles(size);
    const std::array<ComputeBinding, 6> prepareBindings{
        storageBufferBinding(record.get()), storageBufferBinding(focus.get()), depthBinding, pictureBinding,
        storageImageBinding(prepared.getView()), storageImageBinding(tiles.getView()),
    };
    bindComputeSet(commands, handle, descriptors, *preparePipeline, prepareBindings);
    vkCmdDispatch(commands, tileGrid.width, tileGrid.height, 1);

    tiles.transition(commands, ImageState::ComputeRead);
    neighbours.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    const std::array<ComputeBinding, 2> neighboursBindings{sampledBinding(nearest, tiles.getView(), readOnly), storageImageBinding(neighbours.getView())};
    bindComputeSet(commands, handle, descriptors, *neighboursPipeline, neighboursBindings);
    vkCmdDispatch(commands, workgroupCount(tileGrid.width, DEPTH_OF_FIELD_GROUP), workgroupCount(tileGrid.height, DEPTH_OF_FIELD_GROUP), 1);

    prepared.transition(commands, ImageState::ComputeRead);
    neighbours.transition(commands, ImageState::ComputeRead);
    farLayer.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    nearLayer.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    const std::array<ComputeBinding, 4> gatherBindings{
        sampledBinding(nearest, prepared.getView(), readOnly), sampledBinding(nearest, neighbours.getView(), readOnly),
        storageImageBinding(farLayer.getView()), storageImageBinding(nearLayer.getView()),
    };
    bindComputeSet(commands, handle, descriptors, *gatherPipeline, gatherBindings);
    vkCmdDispatch(commands, workgroupCount(grid.width, DEPTH_OF_FIELD_GROUP), workgroupCount(grid.height, DEPTH_OF_FIELD_GROUP), 1);

    farLayer.transition(commands, ImageState::ComputeRead);
    nearLayer.transition(commands, ImageState::ComputeRead);
    farFiltered.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    nearFiltered.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    const std::array<ComputeBinding, 4> medianBindings{
        sampledBinding(nearest, farLayer.getView(), readOnly), sampledBinding(nearest, nearLayer.getView(), readOnly),
        storageImageBinding(farFiltered.getView()), storageImageBinding(nearFiltered.getView()),
    };
    bindComputeSet(commands, handle, descriptors, *medianPipeline, medianBindings);
    vkCmdDispatch(commands, workgroupCount(grid.width, DEPTH_OF_FIELD_GROUP), workgroupCount(grid.height, DEPTH_OF_FIELD_GROUP), 1);

    farFiltered.transition(commands, ImageState::ComputeRead);
    nearFiltered.transition(commands, ImageState::ComputeRead);
    result.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    const std::array<ComputeBinding, 7> compositeBindings{
        storageBufferBinding(record.get()), storageBufferBinding(focus.get()), depthBinding, pictureBinding,
        sampledBinding(linear, farFiltered.getView(), readOnly), sampledBinding(linear, nearFiltered.getView(), readOnly),
        storageImageBinding(result.getView()),
    };
    bindComputeSet(commands, handle, descriptors, *compositePipeline, compositeBindings);
    vkCmdDispatch(commands, workgroupCount(size.width, DEPTH_OF_FIELD_GROUP), workgroupCount(size.height, DEPTH_OF_FIELD_GROUP), 1);

    result.transition(commands, ImageState::SurfaceRead);
    picture.transition(commands, ImageState::SurfaceRead);
    return result;
}
