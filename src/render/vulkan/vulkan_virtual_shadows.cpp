#include "render/vulkan/vulkan_virtual_shadows.h"

#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/vulkan_device.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace {
    // The largest table any settings make: every level at 64 pages a side.
    constexpr std::size_t largestTable = static_cast<std::size_t>(maxVirtualShadowLevels) * 64u * 64u;

    constexpr VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    // Each pass reads and writes what the one before wrote: every compute write made visible to the next pass.
    void computeToCompute(VkCommandBuffer commands) {
        memoryBarrier(commands, {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
                      {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
    }

    std::unique_ptr<ComputePipeline> pagesPipeline(const VulkanDevice& device, const char* shader, std::uint32_t sampledImages) {
        ComputePipelineDescription description;
        description.shader = shader;
        description.storageBuffers = 7;
        description.sampledImages = sampledImages;
        return std::make_unique<ComputePipeline>(device, description);
    }

    // What changes the GPU's state: other settings (the debug view, the depth slack) only move levels or show them.
    bool sameLayout(const VirtualShadowSettings& a, const VirtualShadowSettings& b) {
        return a.levelCount == b.levelCount && a.resolution == b.resolution && a.poolPages == b.poolPages;
    }
}

VulkanVirtualShadows::VulkanVirtualShadows(const VulkanDevice& device, VkSampler nearest, std::uint32_t framesInFlight):
    device(device),
    nearest(nearest),
    table(device.getAllocator(), largestTable * sizeof(std::uint32_t), storage, BufferMemory::Gpu),
    keys(device.getAllocator(), largestTable * 4u * sizeof(std::uint32_t), storage, BufferMemory::Gpu),
    counters(device.getAllocator(), VIRTUAL_SHADOW_COUNTERS * sizeof(std::uint32_t), storage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Gpu),
    freePages(device.getAllocator(), static_cast<std::size_t>(maxVirtualShadowPoolPages) * sizeof(std::uint32_t), storage, BufferMemory::Gpu),
    neededEntries(device.getAllocator(), largestTable * sizeof(std::uint32_t), storage, BufferMemory::Gpu),
    cachedEntries(device.getAllocator(), largestTable * sizeof(std::uint32_t), storage, BufferMemory::Gpu)
{
    clearPipeline = pagesPipeline(device, "assets/shaders/virtual_shadow_clear.comp.spv", 0);
    markPipeline = pagesPipeline(device, "assets/shaders/virtual_shadow_mark.comp.spv", 1);
    resolvePipeline = pagesPipeline(device, "assets/shaders/virtual_shadow_resolve.comp.spv", 0);
    allocatePipeline = pagesPipeline(device, "assets/shaders/virtual_shadow_allocate.comp.spv", 0);
    setFramesInFlight(framesInFlight);
}

void VulkanVirtualShadows::setFramesInFlight(std::uint32_t count) {
    frameRecords.clear();
    readbacks.clear();

    for (std::uint32_t frame = 0; frame < count; ++frame) {
        frameRecords.emplace_back(device.getAllocator(), sizeof(GpuVirtualShadowFrame), storage, BufferMemory::Upload);
        std::memset(frameRecords.back().getMapped(), 0, sizeof(GpuVirtualShadowFrame));
        frameRecords.back().flush();
        readbacks.push_back(Readback{
            VulkanBuffer{device.getAllocator(), VIRTUAL_SHADOW_COUNTERS * sizeof(std::uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback},
            false});
    }
}

/*
* The clipmap follows the camera; each level it dropped (a new sun, settings, a depth range re-centred) gets a new
* generation, so its entries' keys no longer match and resolving frees them. Off or without a sun, the record holds no
* levels: the main pass's debug view shows nothing, and the GPU's pages wait, unchanged, for the next frame that marks.
*/
void VulkanVirtualShadows::prepare(std::uint32_t frameIndex, const VirtualShadowSettings& settings, const Vec3& camera, const Vec3& sunDirection,
                                   float casterReach, const Mat4& inverseViewProjection, float width, float height) {
    VulkanBuffer& record = frameRecords.at(frameIndex);
    running = settings.enabled && sunDirection.lengthSquared() > 0.0f;
    GpuVirtualShadowFrame packed{};

    if (running) {
        if (!builtFor || !sameLayout(*builtFor, settings)) {
            startAgain = true;
            clipmap.reset();
        }

        builtFor = settings;
        const VirtualShadowClipmapUpdate update = updateVirtualShadowClipmap(clipmap, camera, sunDirection, settings, casterReach);
        clipmap = update.clipmap;

        for (int level = 0; level < maxVirtualShadowLevels; ++level) {
            if ((update.droppedLevels & (1u << level)) != 0) {
                ++generations[static_cast<std::size_t>(level)];
            }
        }

        ++frameNumber;
        packed = packVirtualShadowFrame(*clipmap, camera, generations, inverseViewProjection, width, height, settings.showPages, frameNumber);
        packed.options[2] = startAgain ? 1.0f : 0.0f;
    }

    std::memcpy(record.getMapped(), &packed, sizeof(packed));
    record.flush();
}

bool VulkanVirtualShadows::runs() const {
    return running;
}

/*
* The barriers: the depth from the prepass to compute reads (the main pass makes it a depth target again, keeping it);
* between the passes, every compute write to the next pass (memory barriers, since each touches most of the buffers);
* after them, the table to the main pass's fragment reads and the counters to the copy, the copy to the CPU. Before
* clearing, last frame's main pass may still read the table and last frame's copy the counters: a write after reads
* needs only their order, which the first barrier gives.
*/
void VulkanVirtualShadows::mark(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& depth) {
    const VulkanBuffer& record = frameRecords.at(frameIndex);
    Readback& readback = readbacks.at(frameIndex);
    const VirtualShadowSettings& settings = *builtFor;
    const std::uint32_t pagesPerSide = static_cast<std::uint32_t>(virtualShadowPagesPerSide(settings.resolution));
    const std::uint32_t entries = static_cast<std::uint32_t>(settings.levelCount) * pagesPerSide * pagesPerSide;
    const std::uint32_t cleared = std::max(entries, static_cast<std::uint32_t>(settings.poolPages));

    memoryBarrier(commands, {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_NONE},
                  {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
    depth.transition(commands, ImageState::DepthComputeRead);

    const std::array<ComputeBinding, 7> buffers{
        storageBufferBinding(record.get()),    storageBufferBinding(table.get()),         storageBufferBinding(keys.get()),
        storageBufferBinding(counters.get()),  storageBufferBinding(freePages.get()),     storageBufferBinding(neededEntries.get()),
        storageBufferBinding(cachedEntries.get()),
    };
    std::array<ComputeBinding, 8> withDepth{};
    std::copy(buffers.begin(), buffers.end(), withDepth.begin());
    withDepth[7] = sampledBinding(nearest, depth.getView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

    bindComputeSet(commands, device.getDevice(), descriptors, *clearPipeline, buffers);
    vkCmdDispatch(commands, workgroupCount(cleared, VIRTUAL_SHADOW_RESOLVE_GROUP), 1, 1);
    computeToCompute(commands);

    const ImageDescription& size = depth.getDescription();
    bindComputeSet(commands, device.getDevice(), descriptors, *markPipeline, withDepth);
    vkCmdDispatch(commands, workgroupCount(size.width, VIRTUAL_SHADOW_MARK_GROUP_SIDE), workgroupCount(size.height, VIRTUAL_SHADOW_MARK_GROUP_SIDE), 1);
    computeToCompute(commands);

    bindComputeSet(commands, device.getDevice(), descriptors, *resolvePipeline, buffers);
    vkCmdDispatch(commands, workgroupCount(entries, VIRTUAL_SHADOW_RESOLVE_GROUP), 1, 1);
    computeToCompute(commands);

    bindComputeSet(commands, device.getDevice(), descriptors, *allocatePipeline, buffers);
    vkCmdDispatch(commands, 1, 1, 1);

    memoryBarrier(commands, {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
                  {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT});

    VkBufferCopy copy{};
    copy.size = VIRTUAL_SHADOW_COUNTERS * sizeof(std::uint32_t);
    vkCmdCopyBuffer(commands, counters.get(), readback.counters.get(), 1, &copy);
    bufferBarrier(commands, readback.counters.get(), {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
                  {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT});
    readback.pending = true;
    startAgain = false;
}

std::optional<VirtualShadowCounts> VulkanVirtualShadows::collect(std::uint32_t frameIndex) {
    Readback& readback = readbacks.at(frameIndex);

    if (!readback.pending) {
        return std::nullopt;
    }

    readback.pending = false;
    readback.counters.invalidate();
    std::array<std::uint32_t, VIRTUAL_SHADOW_COUNTERS> values{};
    std::memcpy(values.data(), readback.counters.getMapped(), sizeof(values));
    return virtualShadowCountsFrom(values);
}

VkDescriptorBufferInfo VulkanVirtualShadows::frameRecord(std::uint32_t frameIndex) const {
    return VkDescriptorBufferInfo{frameRecords.at(frameIndex).get(), 0, VK_WHOLE_SIZE};
}

VkDescriptorBufferInfo VulkanVirtualShadows::pageTable() const {
    return VkDescriptorBufferInfo{table.get(), 0, VK_WHOLE_SIZE};
}
