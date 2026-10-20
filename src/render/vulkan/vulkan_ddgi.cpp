#include "render/vulkan/vulkan_ddgi.h"
#include "render/frames_in_flight.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/vulkan_check.h"
#include "render/vulkan/vulkan_device.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
    constexpr VkFormat irradianceFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    constexpr VkFormat distanceFormat = VK_FORMAT_R16G16_SFLOAT;

    // Each ray's result: the light it brought back from still lights and its distance (a vec4 in std430 and here alike),
    // and from fast lights, as three half floats and one unused in two words (a uvec2); and each probe's flag, an int.
    constexpr VkDeviceSize rayResultBytes = 4 * sizeof(float);
    constexpr VkDeviceSize fastRayResultBytes = 2 * sizeof(std::uint32_t);
    constexpr VkDeviceSize probeFlagBytes = sizeof(std::int32_t);

    // What the relocation pass reads, as push constants (must match RelocationData in ddgi_relocate.comp).
    struct GpuProbeRelocate {
        float origin[4];        // xyz the first probe's grid point
        float spacing[4];       // xyz the spacing between probes
        std::int32_t counts[4]; // xyz probes along each axis, w the most steps a probe takes (0: classify only)
    };

    bool sameVolume(const ProbeVolume& a, const ProbeVolume& b) {
        return a.origin.x == b.origin.x && a.origin.y == b.origin.y && a.origin.z == b.origin.z && a.spacing.x == b.spacing.x &&
               a.spacing.y == b.spacing.y && a.spacing.z == b.spacing.z && a.counts.x == b.counts.x && a.counts.y == b.counts.y &&
               a.counts.z == b.counts.z;
    }

    OwnedHandle<VkPipelineLayout> createPipelineLayout(VkDevice device, std::span<const VkDescriptorSetLayout> sets, VkShaderStageFlags pushStages,
                                                       const char* failure, std::uint32_t pushBytes = sizeof(GpuProbeVolume)) {
        const VkPushConstantRange push{pushStages, 0, pushBytes};

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = static_cast<std::uint32_t>(sets.size());
        layoutInfo.pSetLayouts = sets.data();
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &push;

        VkPipelineLayout created = VK_NULL_HANDLE;
        vulkanCheck(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &created), failure);
        return OwnedHandle<VkPipelineLayout>{device, created, vkDestroyPipelineLayout};
    }

    // A set of count bindings (0..count - 1) all of one type.
    OwnedHandle<VkDescriptorSetLayout> createUniformLayout(VkDevice device, VkDescriptorType type, std::uint32_t count, VkShaderStageFlags stages,
                                                           const char* failure) {
        std::vector<VkDescriptorSetLayoutBinding> bindings(count);

        for (std::uint32_t index = 0; index < count; ++index) {
            bindings[index].binding = index;
            bindings[index].descriptorType = type;
            bindings[index].descriptorCount = 1;
            bindings[index].stageFlags = stages;
        }

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = count;
        info.pBindings = bindings.data();

        VkDescriptorSetLayout created = VK_NULL_HANDLE;
        vulkanCheck(vkCreateDescriptorSetLayout(device, &info, nullptr, &created), failure);
        return OwnedHandle<VkDescriptorSetLayout>{device, created, vkDestroyDescriptorSetLayout};
    }

    // A write of whole buffers into bindings first.. of a set, one each.
    void writeBuffers(VkDevice device, VkDescriptorSet set, std::uint32_t first, std::span<const VulkanBuffer* const> buffers) {
        std::vector<VkDescriptorBufferInfo> infos;
        std::vector<VkWriteDescriptorSet> writes(buffers.size());

        for (const VulkanBuffer* buffer : buffers) {
            infos.push_back(VkDescriptorBufferInfo{buffer->get(), 0, VK_WHOLE_SIZE});
        }

        for (std::size_t index = 0; index < buffers.size(); ++index) {
            writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index].dstSet = set;
            writes[index].dstBinding = first + static_cast<std::uint32_t>(index);
            writes[index].descriptorCount = 1;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[index].pBufferInfo = &infos[index];
        }

        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

VulkanDdgi::VulkanDdgi(const VulkanDevice& device, DescriptorLayouts& layouts, VkDescriptorSetLayout texturesLayout, VkDescriptorSetLayout frameLayout,
                       VkDescriptorSetLayout rayLayout, VkSampler sampler, VkFormat colourFormat, VkFormat depthFormat):
    device(device),
    sampler(sampler)
{
    createTracePipeline(texturesLayout, frameLayout, rayLayout);
    createRelocatePipeline(texturesLayout, frameLayout, rayLayout);

    restartUploads.reserve(maxFramesInFlight);
    stateUploads.reserve(maxFramesInFlight);

    for (std::uint32_t frame = 0; frame < maxFramesInFlight; ++frame) {
        restartUploads.emplace_back(device.getAllocator(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        stateUploads.emplace_back(device.getAllocator(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }

    // Both read the three ray buffers, the probes' turns, the scene event restarts and the probes' states; the irradiance
    // update writes two atlases and the adaptive memory's change statistics, the distance update one atlas.
    ComputePipelineDescription update;
    update.storageBuffers = 6;
    update.storageImages = 3;
    update.pushConstantBytes = sizeof(GpuProbeVolume);
    update.shader = "assets/shaders/ddgi_update_irradiance.comp.spv";
    irradiancePipeline = std::make_unique<ComputePipeline>(device, update);
    update.storageImages = 1;
    update.shader = "assets/shaders/ddgi_update_distance.comp.spv";
    distancePipeline = std::make_unique<ComputePipeline>(device, update);

    // Sharing reads the scene event restarts, the probes' states, both irradiance atlases and the distances, and writes the
    // two shared atlases.
    ComputePipelineDescription share;
    share.storageBuffers = 2;
    share.storageImages = 5;
    share.pushConstantBytes = sizeof(GpuProbeShare);
    share.shader = "assets/shaders/ddgi_share.comp.spv";
    sharePipeline = std::make_unique<ComputePipeline>(device, share);

    createBallsPipeline(layouts, texturesLayout, frameLayout, colourFormat, depthFormat);
}

/*
* The trace reads the scene the way the ray debug view does (every texture, the frame's instances, materials and lights,
* and the ray tracing set), plus a fourth set of its own: the buffers it writes the rays into.
*/
void VulkanDdgi::createTracePipeline(VkDescriptorSetLayout texturesLayout, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout rayLayout) {
    raysLayout = createUniformLayout(device.getDevice(), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4, VK_SHADER_STAGE_COMPUTE_BIT,
                                     "Probe ray set layout creation failed");

    const std::array<VkDescriptorSetLayout, 4> sets{texturesLayout, frameLayout, rayLayout, raysLayout.get()};
    traceLayout = createPipelineLayout(device.getDevice(), sets, VK_SHADER_STAGE_COMPUTE_BIT, "Probe trace layout creation failed");

    ComputePipelineDescription trace;
    trace.shader = "assets/shaders/ddgi_trace.comp.spv";
    trace.sharedLayout = traceLayout.get();
    tracePipeline = std::make_unique<ComputePipeline>(device, trace);
}

/*
* The relocation traces against the frame's ray tracing set, as the trace does (sets 0 and 1 are in its layout so the
* set keeps its number, though it reads neither), plus a set of its own: the records it writes.
*/
void VulkanDdgi::createRelocatePipeline(VkDescriptorSetLayout texturesLayout, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout rayLayout) {
    relocateSetLayout = createUniformLayout(device.getDevice(), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                            "Probe relocation set layout creation failed");

    const std::array<VkDescriptorSetLayout, 4> sets{texturesLayout, frameLayout, rayLayout, relocateSetLayout.get()};
    relocateLayout = createPipelineLayout(device.getDevice(), sets, VK_SHADER_STAGE_COMPUTE_BIT, "Probe relocation layout creation failed",
                                          sizeof(GpuProbeRelocate));

    ComputePipelineDescription relocation;
    relocation.shader = "assets/shaders/ddgi_relocate.comp.spv";
    relocation.sharedLayout = relocateLayout.get();
    relocatePipeline = std::make_unique<ComputePipeline>(device, relocation);
}

/*
* The balls draw into the scene image over the finished main pass, tested against and writing its depth (reversed-Z:
* nearer is greater). Their layout keeps the scene's two sets in place, so the frame's set 1 gives the camera, and adds
* both irradiance atlases as set 2.
*/
void VulkanDdgi::createBallsPipeline(DescriptorLayouts& layouts, VkDescriptorSetLayout texturesLayout, VkDescriptorSetLayout frameLayout,
                                     VkFormat colourFormat, VkFormat depthFormat) {
    ballsSetLayout = createUniformLayout(device.getDevice(), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, VK_SHADER_STAGE_FRAGMENT_BIT,
                                         "Probe ball set layout creation failed");

    const std::array<VkDescriptorSetLayout, 3> sets{texturesLayout, frameLayout, ballsSetLayout.get()};
    ballsLayout = createPipelineLayout(device.getDevice(), sets, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                       "Probe ball layout creation failed");

    PipelineDescription description;
    description.vertexShader = "assets/shaders/ddgi_probes.vert.spv";
    description.fragmentShader = "assets/shaders/ddgi_probes.frag.spv";
    description.sharedLayout = ballsLayout.get();
    description.colourFormat = colourFormat;
    description.depthFormat = depthFormat;
    description.depthTest = true;
    description.depthWrite = true;
    description.depthCompare = VK_COMPARE_OP_GREATER;
    ballsPipeline = std::make_unique<VulkanPipeline>(device, layouts, description);
}

/*
* A new volume replaces everything sized by it: the atlases and the ray buffers (room for the most rays a probe may
* trace, so changing the ray counts never reallocates). The frames still in flight may be using the old ones, so this
* waits for the GPU first, as a shadow atlas resize does. The atlases start at zero: no light and no distance, which
* the first update replaces outright (hysteresis 0).
*/
void VulkanDdgi::createAtlases(VkCommandBuffer commands, const ProbeVolume& volume) {
    device.waitIdle();

    const auto createAtlas = [&](VkFormat format, int side, const char* name) {
        const ProbeAtlasSize size = probeAtlasSize(volume.counts, side);
        ImageDescription description;
        description.format = format;
        description.width = static_cast<std::uint32_t>(size.width);
        description.height = static_cast<std::uint32_t>(size.height);
        // Copied from by the GPU tests, which read the atlases back.
        description.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        description.name = name;
        VulkanImage atlas{device, description};

        atlas.transition(commands, ImageState::TransferDestination, ImageContents::Discard);
        const VkClearColorValue zero{};
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        vkCmdClearColorImage(commands, atlas.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        return atlas;
    };

    irradiance = createAtlas(irradianceFormat, probeIrradianceTexels, "Probe irradiance atlas");
    fastIrradiance = createAtlas(irradianceFormat, probeIrradianceTexels, "Probe fast irradiance atlas");
    changeStatistics = createAtlas(irradianceFormat, probeIrradianceTexels, "Probe change statistics");
    sharedIrradiance = createAtlas(irradianceFormat, probeIrradianceTexels, "Probe shared irradiance atlas");
    sharedFastIrradiance = createAtlas(irradianceFormat, probeIrradianceTexels, "Probe shared fast irradiance atlas");
    distance = createAtlas(distanceFormat, probeDistanceTexels, "Probe distance atlas");

    const VkDeviceSize probes = static_cast<VkDeviceSize>(probeCount(volume.counts));
    rays = VulkanBuffer{device.getAllocator(), probes * maxRaysPerProbe * rayResultBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Gpu};
    fastRays = VulkanBuffer{device.getAllocator(), probes * maxRaysPerProbe * fastRayResultBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Gpu};
    nearFastLight = VulkanBuffer{device.getAllocator(), probes * probeFlagBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Gpu};
    probeTurns = VulkanBuffer{device.getAllocator(), probes * sizeof(GpuProbeTurn), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload};
    writeProbeTurns(probeCount(volume.counts), turnsIndependent);

    // Written by the GPU straight into memory the CPU reads: a few bytes per probe, once in a while.
    relocationRecords = VulkanBuffer{device.getAllocator(), probes * sizeof(GpuProbeRelocation), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                     BufferMemory::Readback};
    relocations.clear();
    states.clear();
    relocationRequested = true;
    relocationPending = false;
    currentVolume = volume;
}

/*
* The turns are written where the GPU reads them, so a change waits for every frame in flight to finish with the old
* ones: a hitch, but only when sharing is switched on or off.
*/
void VulkanDdgi::prepare(VkCommandBuffer commands, const ProbeVolume& volume, const GlobalIlluminationSettings& settings, std::span<const DrawItem> draws,
                         std::uint32_t frameIndex) {
    validateProbeVolume(volume);
    sharing = settings.probeSharing > 0.0f || settings.fastProbeSharing > 0.0f;

    if (!currentVolume || !sameVolume(*currentVolume, volume)) {
        turnsIndependent = sharing;
        createAtlases(commands, volume);
        irradiance.transition(commands, ImageState::SurfaceRead);
        fastIrradiance.transition(commands, ImageState::SurfaceRead);
        distance.transition(commands, ImageState::SurfaceRead);
        sharedIrradiance.transition(commands, ImageState::SurfaceRead);
        sharedFastIrradiance.transition(commands, ImageState::SurfaceRead);
        freshAtlases = true;
    } else if (sharing != turnsIndependent) {
        device.waitIdle();
        turnsIndependent = sharing;
        writeProbeTurns(probeCount(volume.counts), turnsIndependent);
    }

    if (relocationPending) {
        collectRelocation(volume);
    }

    // Switching relocation on or off relocates again: off, the probes are classified where the grid puts them.
    if (!relocations.empty() && settings.probeRelocation != relocatedWithMoves) {
        relocationRequested = true;
    }

    std::vector<GpuProbeState> next = buildProbeStates(volume, relocations, movingBounds(draws), settings);
    restartedByStates = restartedProbes(states, next);
    states = std::move(next);
    stateUploads.at(frameIndex).write(states);
}

/*
* Recorded on an earlier frame, which may still be in flight: waiting for the whole GPU is a hitch, but only after a
* relocation (start-up, a new volume, a geometry change).
*/
void VulkanDdgi::collectRelocation(const ProbeVolume& volume) {
    device.waitIdle();
    relocationRecords.invalidate();
    relocations.resize(static_cast<std::size_t>(probeCount(volume.counts)));
    std::memcpy(relocations.data(), relocationRecords.getMapped(), relocations.size() * sizeof(GpuProbeRelocation));
    relocationPending = false;
}

void VulkanDdgi::writeProbeTurns(int probes, bool independent) {
    const std::vector<GpuProbeTurn> turns = packProbeTurns(probes, independent);
    std::memcpy(probeTurns.getMapped(), turns.data(), turns.size() * sizeof(GpuProbeTurn));
    probeTurns.flush();
}

void VulkanDdgi::update(VkCommandBuffer commands, FrameDescriptors& descriptors, const ProbeSceneSets& sets, const ProbeVolume& volume,
                        const GlobalIlluminationSettings& settings, const Vec3& sky, float frameSeconds, bool fastLights,
                        std::span<const ProbeEvent> events, std::uint32_t frameIndex) {
    if (!currentVolume || !sameVolume(*currentVolume, volume)) {
        throw std::logic_error("Prepare the light probes for a volume before updating them");
    }

    const bool fresh = std::exchange(freshAtlases, false);
    ProbeFrame frame = scheduleProbeFrame(schedule, settings, frameSeconds, fastLights, fresh);
    frame.sky = sky;
    lastPacked = packProbeVolume(volume, frame);

    // Written to this frame in flight's upload memory, whose last reads finished with its fence.
    advanceProbeRestarts(restarts, volume, events, frame, restartedByStates);
    UploadArray& frameRestarts = restartUploads.at(frameIndex);
    frameRestarts.write(restarts);
    const UploadArray& frameStates = stateUploads.at(frameIndex);

    // A changed level moves what the probes may land in: relocate them again, for the next frame's states.
    const bool geometryChanged =
        std::any_of(events.begin(), events.end(), [](const ProbeEvent& event) { return event.kind == ProbeEventKind::GeometryChange; });

    if (relocationRequested || geometryChanged) {
        relocate(commands, descriptors, sets, volume, settings.probeRelocation ? probeRelocationIterations : 0);
        relocatedWithMoves = settings.probeRelocation;
        relocationRequested = false;
        relocationPending = true;
    }

    const int probes = probeCount(volume.counts);
    trace(commands, descriptors, sets, probes, probeRayStride(frame));

    // The change statistics are only ever read and written by this update, so they stay in storage use between frames
    // (each frame's transition still orders its update after the last one's writes).
    const std::array<VulkanImage*, 3> irradianceAtlases{&irradiance, &fastIrradiance, &changeStatistics};
    const std::array<VulkanImage*, 1> distanceAtlas{&distance};
    blend(commands, descriptors, *irradiancePipeline, irradianceAtlases, probes, frameRestarts, frameStates);
    blend(commands, descriptors, *distancePipeline, distanceAtlas, probes, frameRestarts, frameStates);

    if (sharing) {
        shareProbes(commands, descriptors, volume, settings, frame.irradianceHysteresis, frameRestarts, frameStates);
    }

    // The blends' writes made visible to every later read: this frame's main pass and debug balls, the next one's trace.
    irradiance.transition(commands, ImageState::SurfaceRead);
    fastIrradiance.transition(commands, ImageState::SurfaceRead);
    distance.transition(commands, ImageState::SurfaceRead);
    sharedIrradiance.transition(commands, ImageState::SurfaceRead);
    sharedFastIrradiance.transition(commands, ImageState::SurfaceRead);
}

std::array<const VulkanBuffer*, 3> VulkanDdgi::rayBuffers() const {
    return {&rays, &fastRays, &nearFastLight};
}

std::array<const VulkanBuffer*, 4> VulkanDdgi::probeBuffers() const {
    return {&rays, &fastRays, &nearFastLight, &probeTurns};
}

/*
* One workgroup per probe. The probes' own atlases stay in storage use from the blends, whose writes these reads wait
* for (a transition to the same writable state still records a barrier); the shared atlases go to storage use from being
* sampled (last frame's main pass and balls, this frame's trace), whose reads must finish first.
*/
void VulkanDdgi::shareProbes(VkCommandBuffer commands, FrameDescriptors& descriptors, const ProbeVolume& volume, const GlobalIlluminationSettings& settings,
                             float longKeep, const UploadArray& frameRestarts, const UploadArray& frameStates) {
    const std::array<VulkanImage*, 5> images{&irradiance, &fastIrradiance, &distance, &sharedIrradiance, &sharedFastIrradiance};
    const VkDescriptorSet set = descriptors.allocate(sharePipeline->getResourceLayout());
    writeUpload(set, 0, frameRestarts);
    writeUpload(set, 1, frameStates);
    std::array<VkDescriptorImageInfo, 5> imageInfos{};
    std::array<VkWriteDescriptorSet, 5> writes{};

    for (std::size_t index = 0; index < images.size(); ++index) {
        images[index]->transition(commands, ImageState::ComputeStorage);
        imageInfos[index] = VkDescriptorImageInfo{VK_NULL_HANDLE, images[index]->getView(), VK_IMAGE_LAYOUT_GENERAL};
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = set;
        writes[index].dstBinding = static_cast<std::uint32_t>(index) + 2;
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[index].pImageInfo = &imageInfos[index];
    }

    vkUpdateDescriptorSets(device.getDevice(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    const GpuProbeShare packed = packProbeShare(volume, settings.probeSharing, settings.fastProbeSharing, settings.eventProbeSharing, longKeep);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, sharePipeline->get());
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, sharePipeline->getLayout(), 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(commands, sharePipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(packed), &packed);
    vkCmdDispatch(commands, static_cast<std::uint32_t>(probeCount(volume.counts)), 1, 1);
}

/*
* Every ray of every probe, one invocation each, with room for the most rays any probe traces (the stride): a probe that
* traces fewer leaves the rest of its room unwritten, and its invocations there stop at once. The ray buffers are shared
* by the frames in flight: last frame's update may still be reading them, so the writes wait for those reads (nothing
* to flush after a read), and the update's reads wait for these writes.
*/
void VulkanDdgi::trace(VkCommandBuffer commands, FrameDescriptors& descriptors, const ProbeSceneSets& sets, int probes, int rayStride) {
    for (const VulkanBuffer* buffer : rayBuffers()) {
        bufferBarrier(commands, buffer->get(), {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE},
                      {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
    }

    const VkDescriptorSet raySet = descriptors.allocate(raysLayout.get());
    writeBuffers(device.getDevice(), raySet, 0, probeBuffers());

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, tracePipeline->get());
    const std::array<VkDescriptorSet, 4> boundSets{sets.textures, sets.frame, sets.rayScene, raySet};
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, traceLayout.get(), 0, static_cast<std::uint32_t>(boundSets.size()),
                            boundSets.data(), 0, nullptr);
    vkCmdPushConstants(commands, traceLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(lastPacked), &lastPacked);

    const std::uint32_t rayCount = static_cast<std::uint32_t>(probes) * static_cast<std::uint32_t>(rayStride);
    vkCmdDispatch(commands, workgroupCount(rayCount, DDGI_TRACE_GROUP_SIZE), 1, 1);

    for (const VulkanBuffer* buffer : rayBuffers()) {
        bufferBarrier(commands, buffer->get(), {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
                      {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT});
    }
}

/*
* One workgroup per probe blends the rays into the atlases, in place. Each atlas goes to storage use from being sampled
* (this frame's trace, last frame's main pass and balls), whose reads must finish before it is written.
*/
void VulkanDdgi::blend(VkCommandBuffer commands, FrameDescriptors& descriptors, const ComputePipeline& pipeline, std::span<VulkanImage* const> atlases,
                       int probes, const UploadArray& frameRestarts, const UploadArray& frameStates) {
    const VkDescriptorSet set = descriptors.allocate(pipeline.getResourceLayout());
    const std::array<const VulkanBuffer*, 4> buffers = probeBuffers();
    writeBuffers(device.getDevice(), set, 0, buffers);

    // The restarts and the states after the probe buffers, then the atlases.
    writeUpload(set, static_cast<std::uint32_t>(buffers.size()), frameRestarts);
    writeUpload(set, static_cast<std::uint32_t>(buffers.size()) + 1, frameStates);
    const std::uint32_t firstImage = static_cast<std::uint32_t>(buffers.size()) + 2;

    std::vector<VkDescriptorImageInfo> imageInfos;

    for (VulkanImage* atlas : atlases) {
        atlas->transition(commands, ImageState::ComputeStorage);
        imageInfos.push_back(VkDescriptorImageInfo{VK_NULL_HANDLE, atlas->getView(), VK_IMAGE_LAYOUT_GENERAL});
    }

    std::vector<VkWriteDescriptorSet> writes(atlases.size());

    for (std::size_t index = 0; index < atlases.size(); ++index) {
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = set;
        writes[index].dstBinding = firstImage + static_cast<std::uint32_t>(index);
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[index].pImageInfo = &imageInfos[index];
    }

    vkUpdateDescriptorSets(device.getDevice(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.get());
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getLayout(), 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(commands, pipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(lastPacked), &lastPacked);
    vkCmdDispatch(commands, static_cast<std::uint32_t>(probes), 1, 1);
}

/*
* One workgroup per probe, its records written straight into read-back memory; the barrier makes the writes visible to
* the CPU once the frame's fence has passed. The records were last read by the CPU after waiting for the GPU, so the
* writes need nothing to wait for.
*/
void VulkanDdgi::relocate(VkCommandBuffer commands, FrameDescriptors& descriptors, const ProbeSceneSets& sets, const ProbeVolume& volume, int iterations) {
    const VkDescriptorSet recordSet = descriptors.allocate(relocateSetLayout.get());
    const std::array<const VulkanBuffer*, 1> records{&relocationRecords};
    writeBuffers(device.getDevice(), recordSet, 0, records);

    GpuProbeRelocate packed{};
    packed.origin[0] = volume.origin.x;
    packed.origin[1] = volume.origin.y;
    packed.origin[2] = volume.origin.z;
    packed.spacing[0] = volume.spacing.x;
    packed.spacing[1] = volume.spacing.y;
    packed.spacing[2] = volume.spacing.z;
    packed.counts[0] = volume.counts.x;
    packed.counts[1] = volume.counts.y;
    packed.counts[2] = volume.counts.z;
    packed.counts[3] = iterations;

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, relocatePipeline->get());
    const std::array<VkDescriptorSet, 4> boundSets{sets.textures, sets.frame, sets.rayScene, recordSet};
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, relocateLayout.get(), 0, static_cast<std::uint32_t>(boundSets.size()),
                            boundSets.data(), 0, nullptr);
    vkCmdPushConstants(commands, relocateLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(packed), &packed);
    vkCmdDispatch(commands, static_cast<std::uint32_t>(probeCount(volume.counts)), 1, 1);

    bufferBarrier(commands, relocationRecords.get(), {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
                  {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT});
}

void VulkanDdgi::writeUpload(VkDescriptorSet set, std::uint32_t binding, const UploadArray& upload) const {
    const VkDescriptorBufferInfo info = upload.describe();
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(device.getDevice(), 1, &write, 0, nullptr);
}

// Six corners (two triangles) per ball, one instance per probe.
void VulkanDdgi::drawProbes(VkCommandBuffer commands, FrameDescriptors& descriptors, VkDescriptorSet textures, VkDescriptorSet frame) {
    if (!hasProbes()) {
        return;
    }

    const VkDescriptorSet atlasSet = descriptors.allocate(ballsSetLayout.get());
    const SampledImage still = sampledIrradiance();
    const SampledImage fast = sampledFastIrradiance();
    const std::array<VkDescriptorImageInfo, 2> imageInfos{VkDescriptorImageInfo{still.sampler, still.view, still.layout},
                                                          VkDescriptorImageInfo{fast.sampler, fast.view, fast.layout}};
    std::array<VkWriteDescriptorSet, 2> writes{};

    for (std::size_t index = 0; index < writes.size(); ++index) {
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = atlasSet;
        writes[index].dstBinding = static_cast<std::uint32_t>(index);
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[index].pImageInfo = &imageInfos[index];
    }

    vkUpdateDescriptorSets(device.getDevice(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, ballsPipeline->get());
    const std::array<VkDescriptorSet, 3> sets{textures, frame, atlasSet};
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, ballsLayout.get(), 0, static_cast<std::uint32_t>(sets.size()), sets.data(), 0,
                            nullptr);
    vkCmdPushConstants(commands, ballsLayout.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(lastPacked), &lastPacked);
    vkCmdDraw(commands, 6, static_cast<std::uint32_t>(probeCount(currentVolume->counts)), 0, 0);
}

bool VulkanDdgi::hasProbes() const {
    return currentVolume.has_value();
}

VkDescriptorBufferInfo VulkanDdgi::describeProbeStates(std::uint32_t frameIndex) const {
    return stateUploads.at(frameIndex).describe();
}

const std::vector<GpuProbeState>& VulkanDdgi::getProbeStates() const {
    return states;
}

const std::vector<GpuProbeRelocation>& VulkanDdgi::getRelocations() const {
    return relocations;
}

SampledImage VulkanDdgi::sampledIrradiance() const {
    return SampledImage{(sharing ? sharedIrradiance : irradiance).getView(), sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

SampledImage VulkanDdgi::sampledDistance() const {
    return SampledImage{distance.getView(), sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

SampledImage VulkanDdgi::sampledFastIrradiance() const {
    return SampledImage{(sharing ? sharedFastIrradiance : fastIrradiance).getView(), sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

VulkanImage& VulkanDdgi::getFastIrradianceAtlas() {
    return fastIrradiance;
}

VulkanImage& VulkanDdgi::getSharedIrradianceAtlas() {
    return sharedIrradiance;
}

VulkanImage& VulkanDdgi::getIrradianceAtlas() {
    return irradiance;
}

VulkanImage& VulkanDdgi::getDistanceAtlas() {
    return distance;
}
