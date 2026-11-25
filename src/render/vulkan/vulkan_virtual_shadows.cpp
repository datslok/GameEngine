#include "render/vulkan/vulkan_virtual_shadows.h"

#include "render/gpu/compute_dispatch.h"
#include "render/gpu/mesh_cluster_culling_shared.h"
#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/vulkan_check.h"
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

    constexpr VkDeviceSize headerBytes = VIRTUAL_SHADOW_HEADER_WORDS * sizeof(std::uint32_t);

    // The expanding pass pushes the cluster passes' list and bones stamp, then the style (MESH_CLUSTER_PASS_STYLE).
    constexpr std::uint32_t expandPushBytes = 3 * sizeof(std::uint32_t);

    std::unique_ptr<ComputePipeline> pagesPipeline(const VulkanDevice& device, const char* shader, std::uint32_t sampledImages) {
        ComputePipelineDescription description;
        description.shader = shader;
        description.storageBuffers = 7;
        description.sampledImages = sampledImages;
        return std::make_unique<ComputePipeline>(device, description);
    }

    // A 2D view of a colour image in another format of the same size (the image made with mutableFormat).
    OwnedHandle<VkImageView> viewAs(const VulkanDevice& device, const VulkanImage& image, VkFormat format) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image.get();
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        VkImageView created = VK_NULL_HANDLE;
        vulkanCheck(vkCreateImageView(device.getDevice(), &viewInfo, nullptr, &created), "Virtual shadow pool view creation failed");
        return OwnedHandle<VkImageView>{device.getDevice(), created, vkDestroyImageView};
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
    ImageDescription standIn;
    standIn.format = VK_FORMAT_R32_UINT;
    standIn.width = 1;
    standIn.height = 1;
    standIn.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    standIn.name = "Virtual shadow atomics stand-in";
    atomicStandIn = VulkanImage{device, standIn};
    clearPipeline = pagesPipeline(device, "assets/shaders/virtual_shadow_clear.comp.spv", 0);
    markPipeline = pagesPipeline(device, "assets/shaders/virtual_shadow_mark.comp.spv", 1);
    resolvePipeline = pagesPipeline(device, "assets/shaders/virtual_shadow_resolve.comp.spv", 0);
    allocatePipeline = pagesPipeline(device, "assets/shaders/virtual_shadow_allocate.comp.spv", 0);

    // Expanding: the cluster passes' buffers, the list and totals, then the record, the table and the pairs.
    ComputePipelineDescription expand;
    expand.shader = "assets/shaders/virtual_shadow_expand.comp.spv";
    expand.storageBuffers = MESH_CLUSTER_PASS_BINDINGS + 5;
    expand.pushConstantBytes = expandPushBytes;
    expandPipeline = std::make_unique<ComputePipeline>(device, expand);
    ComputePipelineDescription tasks;
    tasks.shader = "assets/shaders/virtual_shadow_tasks.comp.spv";
    tasks.storageBuffers = 2;
    tasksPipeline = std::make_unique<ComputePipeline>(device, tasks);
    setFramesInFlight(framesInFlight);
}

void VulkanVirtualShadows::setFramesInFlight(std::uint32_t count) {
    frameRecords.clear();
    readbacks.clear();

    for (std::uint32_t frame = 0; frame < count; ++frame) {
        frameRecords.emplace_back(device.getAllocator(), sizeof(GpuVirtualShadowFrame), storage, BufferMemory::Upload);
        std::memset(frameRecords.back().getMapped(), 0, sizeof(GpuVirtualShadowFrame));
        frameRecords.back().flush();
        Readback readback;
        readback.counters =
            VulkanBuffer{device.getAllocator(), VIRTUAL_SHADOW_COUNTERS * sizeof(std::uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback};
        readback.header = VulkanBuffer{device.getAllocator(), headerBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback};
        readbacks.push_back(std::move(readback));
    }
}

/*
* The clipmap follows the camera; each level it dropped (a new sun, settings, a depth range re-centred) gets a new
* generation, so its entries' keys no longer match and resolving frees them. Off or without a sun, the record holds no
* levels: the main pass's debug view shows nothing, and the GPU's pages wait, unchanged, for the next frame that marks.
*/
void VulkanVirtualShadows::prepare(std::uint32_t frameIndex, const VirtualShadowSettings& settings, const Vec3& camera, const Vec3& sunDirection,
                                   float casterReach, float biasTexels, const Mat4& inverseViewProjection, float width, float height) {
    VulkanBuffer& record = frameRecords.at(frameIndex);
    running = settings.enabled && sunDirection.lengthSquared() > 0.0f;
    GpuVirtualShadowFrame packed{};

    if (running) {
        if (!builtFor || !sameLayout(*builtFor, settings)) {
            startAgain = true;
            clipmap.reset();
            levelBias = 0;
        }

        // The pool for these pages: a new one only between frames, the GPU idle (a change of settings, rare).
        const std::uint32_t poolWidth = static_cast<std::uint32_t>(virtualShadowPoolColumns(settings.poolPages) * virtualShadowPageTexels);
        const std::uint32_t poolHeight = static_cast<std::uint32_t>(virtualShadowPoolRows(settings.poolPages) * virtualShadowPageTexels);

        // Drawn by atomics: an R32 float image, its whole-number view written (the same bits), the float view sampled.
        atomics = settings.drawing == VirtualShadowDrawing::Atomics && device.supportsFragmentAtomics();
        const VkFormat poolFormat = atomics ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_D32_SFLOAT;

        if (!pool || pool->getDescription().width != poolWidth || pool->getDescription().height != poolHeight ||
            pool->getDescription().format != poolFormat) {
            device.waitIdle();
            atomicView.reset();
            pool.reset();
            ImageDescription description;
            description.format = poolFormat;
            description.width = poolWidth;
            description.height = poolHeight;
            description.usage = atomics ? VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                        : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            description.aspect = atomics ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
            description.mutableFormat = atomics;
            description.name = "Virtual shadow pool";
            pool = std::make_unique<VulkanImage>(device, description);

            if (atomics) {
                atomicView = viewAs(device, *pool, VK_FORMAT_R32_UINT);
            }
        }

        // This frame's pairs, with the room the frames before asked for.
        Readback& readback = readbacks.at(frameIndex);
        const VkDeviceSize pairBytes = headerBytes + VkDeviceSize{VIRTUAL_SHADOW_PAIR_LISTS} * pairRoom * 2u * sizeof(std::uint32_t);
        ensureBuffer(device.getAllocator(), readback.pairs, pairBytes,
                     storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     BufferMemory::Gpu);
        readback.pairRoom = pairRoom;
        readback.atomics = atomics;
        readback.counted = settings.countCosts;

        builtFor = settings;
        const VirtualShadowClipmapUpdate update = updateVirtualShadowClipmap(clipmap, camera, sunDirection, settings, casterReach);
        clipmap = update.clipmap;

        for (int level = 0; level < maxVirtualShadowLevels; ++level) {
            if ((update.droppedLevels & (1u << level)) != 0) {
                ++generations[static_cast<std::size_t>(level)];
            }
        }

        ++frameNumber;
        packed = packVirtualShadowFrame(*clipmap, camera, generations, inverseViewProjection, width, height, biasTexels, frameNumber,
                                        readback.pairRoom, levelBias);
        packed.options[2] = startAgain ? 1.0f : 0.0f;
    }

    std::memcpy(record.getMapped(), &packed, sizeof(packed));
    record.flush();
}

bool VulkanVirtualShadows::runs() const {
    return running;
}

const std::optional<VirtualShadowClipmap>& VulkanVirtualShadows::getClipmap() const {
    return clipmap;
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
    VirtualShadowCounts counts = virtualShadowCountsFrom(values);

    // The pool's load sets the next frames' coarsening (4d-ii).
    if (builtFor) {
        levelBias = virtualShadowLevelBias(levelBias, counts.requested, builtFor->poolPages, builtFor->poolLoad, builtFor->levelCount);
    }

    counts.coarser = levelBias;
    return counts;
}

int VulkanVirtualShadows::getLevelBias() const {
    return levelBias;
}

VkDescriptorBufferInfo VulkanVirtualShadows::frameRecord(std::uint32_t frameIndex) const {
    return VkDescriptorBufferInfo{frameRecords.at(frameIndex).get(), 0, VK_WHOLE_SIZE};
}

VkDescriptorBufferInfo VulkanVirtualShadows::pageTable() const {
    return VkDescriptorBufferInfo{table.get(), 0, VK_WHOLE_SIZE};
}

VkDeviceAddress VulkanVirtualShadows::pairAddress(std::uint32_t frameIndex) const {
    return readbacks.at(frameIndex).pairs.getDeviceAddress();
}

/*
* The barriers: the casters' list and totals, written by compute while culling and left for the mesh shaders, and the
* table, left by mark, read by the expanding pass; the header cleared first (this frame in flight's last use of the
* pairs, its draw, finished before its fence). Each expanding dispatch only adds to the header, so they run back to back.
*/
void VulkanVirtualShadows::expand(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex,
                                  const std::vector<ComputeBinding>& clusterBindings, VkBuffer clusterTasks, std::uint32_t boneStamp) {
    Readback& readback = readbacks.at(frameIndex);
    const VkBuffer pairs = readback.pairs.get();
    memoryBarrier(commands, {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
                  {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT});
    vkCmdFillBuffer(commands, pairs, 0, headerBytes, 0);
    bufferBarrier(commands, pairs, {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
                  {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});

    std::vector<ComputeBinding> bindings = clusterBindings;
    bindings.push_back(storageBufferBinding(frameRecords.at(frameIndex).get()));
    bindings.push_back(storageBufferBinding(table.get()));
    bindings.push_back(storageBufferBinding(pairs));
    bindComputeSet(commands, device.getDevice(), descriptors, *expandPipeline, bindings);

    for (std::uint32_t style = 0; style < MESH_CLUSTER_STYLES; ++style) {
        const std::uint32_t words[3] = {MESH_CLUSTER_EARLY, boneStamp, style};
        vkCmdPushConstants(commands, expandPipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, expandPushBytes, words);
        vkCmdDispatchIndirect(commands, clusterTasks, (MESH_CLUSTER_EARLY * MESH_CLUSTER_STYLES + style) * 3 * sizeof(std::uint32_t));
    }

    bufferBarrier(commands, pairs, {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
                  {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
    const std::array<ComputeBinding, 2> taskBindings{storageBufferBinding(frameRecords.at(frameIndex).get()), storageBufferBinding(pairs)};
    bindComputeSet(commands, device.getDevice(), descriptors, *tasksPipeline, taskBindings);
    vkCmdDispatch(commands, 1, 1, 1);
    bufferBarrier(commands, pairs, {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
                  {VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT});

    VkBufferCopy copy{};
    copy.size = headerBytes;
    vkCmdCopyBuffer(commands, pairs, readback.header.get(), 1, &copy);
    bufferBarrier(commands, readback.header.get(), {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
                  {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT});
    readback.headerPending = true;
}

void VulkanVirtualShadows::markPoolDrawn(std::uint32_t frameIndex) {
    VulkanBuffer& record = frameRecords.at(frameIndex);
    GpuVirtualShadowFrame* packed = static_cast<GpuVirtualShadowFrame*>(record.getMapped());
    packed->options[3] = 1.0f;
    record.flush();
}

void VulkanVirtualShadows::setCameraFrustum(std::uint32_t frameIndex, const GpuFrustum& frustum) {
    VulkanBuffer& record = frameRecords.at(frameIndex);
    GpuVirtualShadowFrame* packed = static_cast<GpuVirtualShadowFrame*>(record.getMapped());
    std::memcpy(packed->cameraPlanes, frustum.planes, sizeof(packed->cameraPlanes));
    record.flush();
}

VulkanImage* VulkanVirtualShadows::getPool() {
    return pool.get();
}

bool VulkanVirtualShadows::drawsByAtomics() const {
    return atomics && pool;
}

VkImageView VulkanVirtualShadows::getAtomicPoolView() const {
    return drawsByAtomics() ? atomicView.get() : atomicStandIn.getView();
}

VkBuffer VulkanVirtualShadows::getPairs(std::uint32_t frameIndex) const {
    return readbacks.at(frameIndex).pairs.get();
}

std::optional<VirtualShadowDrawn> VulkanVirtualShadows::collectDrawn(std::uint32_t frameIndex) {
    Readback& readback = readbacks.at(frameIndex);

    if (!readback.headerPending) {
        return std::nullopt;
    }

    readback.headerPending = false;
    readback.header.invalidate();
    std::array<std::uint32_t, VIRTUAL_SHADOW_HEADER_WORDS> header{};
    std::memcpy(header.data(), readback.header.getMapped(), sizeof(header));
    VirtualShadowDrawn drawn = virtualShadowDrawnFrom(header, readback.pairRoom, readback.counted);
    drawn.atomics = readback.atomics;
    pairRoom = virtualShadowPairRoomFor(pairRoom, *std::max_element(drawn.pairs.begin(), drawn.pairs.end()));
    return drawn;
}
