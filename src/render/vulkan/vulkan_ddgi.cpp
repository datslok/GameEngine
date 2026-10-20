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
#include <string>
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

    // A seed's texel: its still and fast light, two vec4s (ProbeSeedTexel in ddgi_seed.comp and ddgi_update.glsl).
    constexpr VkDeviceSize seedTexelBytes = 8 * sizeof(float);
    constexpr VkDeviceSize seedBytes = seedTexelBytes * probeIrradianceTexels * probeIrradianceTexels;

    // What the relocation pass reads, as push constants (must match RelocationData in ddgi_relocate.comp).
    struct GpuProbeRelocate {
        std::int32_t counts[4]; // xyz each cascade's probes along each axis, w the most steps a probe takes (0: classify only)
    };

    // Cascades of the same size and spacing share their atlases from frame to frame, however they move.
    bool sameLayout(std::span<const ProbeCascade> a, std::span<const ProbeCascade> b) {
        if (a.size() != b.size()) {
            return false;
        }

        for (std::size_t c = 0; c < a.size(); ++c) {
            const ProbeVolume& first = a[c].volume;
            const ProbeVolume& second = b[c].volume;

            if (first.counts.x != second.counts.x || first.counts.y != second.counts.y || first.counts.z != second.counts.z ||
                first.spacing.x != second.spacing.x || first.spacing.y != second.spacing.y || first.spacing.z != second.spacing.z ||
                a[c].firstProbe != b[c].firstProbe) {
                return false;
            }
        }

        return true;
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

    // Images into bindings first.. of a set as storage images, each moved to storage use first.
    void writeStorageImages(VkCommandBuffer commands, VkDevice device, VkDescriptorSet set, std::uint32_t first, std::span<VulkanImage* const> images) {
        std::vector<VkDescriptorImageInfo> infos;
        std::vector<VkWriteDescriptorSet> writes(images.size());

        for (VulkanImage* image : images) {
            image->transition(commands, ImageState::ComputeStorage);
            infos.push_back(VkDescriptorImageInfo{VK_NULL_HANDLE, image->getView(), VK_IMAGE_LAYOUT_GENERAL});
        }

        for (std::size_t index = 0; index < images.size(); ++index) {
            writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index].dstSet = set;
            writes[index].dstBinding = first + static_cast<std::uint32_t>(index);
            writes[index].descriptorCount = 1;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[index].pImageInfo = &infos[index];
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
    createSeedPipeline(texturesLayout, frameLayout, rayLayout);
    createRelocatePipeline(texturesLayout, frameLayout, rayLayout);

    for (std::uint32_t frame = 0; frame < maxFramesInFlight; ++frame) {
        restartUploads.emplace_back(device.getAllocator(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        stateUploads.emplace_back(device.getAllocator(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        cascadeUploads.emplace_back(device.getAllocator(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        seedUploads.emplace_back(device.getAllocator(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        RelocationFrame relocation;
        relocation.requests = std::make_unique<UploadArray>(device.getAllocator(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        relocationFrames.push_back(std::move(relocation));
    }

    seeds = VulkanBuffer{device.getAllocator(), seedBytes * static_cast<VkDeviceSize>(maxProbeSeeds), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         BufferMemory::Gpu};

    // Both read the three ray buffers, the probes' turns, the scene event restarts, the probes' states, the cascades and
    // the seeds; the irradiance update writes two atlases and the adaptive memory's change statistics, the distance
    // update one atlas.
    ComputePipelineDescription update;
    update.storageBuffers = 8;
    update.storageImages = 3;
    update.pushConstantBytes = sizeof(GpuProbeVolume);
    update.shader = "assets/shaders/ddgi_update_irradiance.comp.spv";
    irradiancePipeline = std::make_unique<ComputePipeline>(device, update);
    update.storageImages = 1;
    update.shader = "assets/shaders/ddgi_update_distance.comp.spv";
    distancePipeline = std::make_unique<ComputePipeline>(device, update);

    // Sharing reads the scene event restarts, the probes' states, the cascades, both irradiance atlases and the
    // distances, and writes the two shared atlases.
    ComputePipelineDescription share;
    share.storageBuffers = 3;
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
* The seeds read the probes as the main pass does, through the frame's set 1 (sets 0 and 2 keep their numbers, as in the
* trace), plus a set of their own: which probe each seed is for, and the seeds they write.
*/
void VulkanDdgi::createSeedPipeline(VkDescriptorSetLayout texturesLayout, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout rayLayout) {
    seedSetLayout = createUniformLayout(device.getDevice(), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2, VK_SHADER_STAGE_COMPUTE_BIT,
                                        "Probe seed set layout creation failed");

    const std::array<VkDescriptorSetLayout, 4> sets{texturesLayout, frameLayout, rayLayout, seedSetLayout.get()};
    seedLayout = createPipelineLayout(device.getDevice(), sets, VK_SHADER_STAGE_COMPUTE_BIT, "Probe seed layout creation failed");

    ComputePipelineDescription seed;
    seed.shader = "assets/shaders/ddgi_seed.comp.spv";
    seed.sharedLayout = seedLayout.get();
    seedPipeline = std::make_unique<ComputePipeline>(device, seed);
}

/*
* The relocation traces against the frame's ray tracing set, as the trace does, and reads the cascades and the camera
* from set 1 (set 0 is in its layout so the sets keep their numbers, though it reads none of it), plus a set of its
* own: the probes asked for and the records it writes.
*/
void VulkanDdgi::createRelocatePipeline(VkDescriptorSetLayout texturesLayout, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout rayLayout) {
    relocateSetLayout = createUniformLayout(device.getDevice(), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2, VK_SHADER_STAGE_COMPUTE_BIT,
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
* nearer is greater). Their layout keeps the scene's two sets in place, so the frame's set 1 gives the camera, the
* states and the cascades, and adds both irradiance atlases as set 2.
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
* New cascades replace everything sized by their probes: the atlases (every cascade's one after another, probeAtlasGrid),
* the per-probe buffers and the relocation's records. The frames still in flight may be using the old ones, so this
* waits for the GPU first, as a shadow atlas resize does. The atlases start at zero: no light and no distance, which
* the first update replaces outright (hysteresis 0); every probe is relocated at the first update.
*/
void VulkanDdgi::createAtlases(VkCommandBuffer commands, std::span<const ProbeCascade> next) {
    device.waitIdle();
    counts = next.front().volume.counts;
    probes = probeCount(counts) * static_cast<int>(next.size());
    const ProbeCounts grid = probeAtlasGrid(counts, static_cast<int>(next.size()));

    const auto createAtlas = [&](VkFormat format, int side, const char* name) {
        const ProbeAtlasSize size = probeAtlasSize(grid, side);
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

    const VkDeviceSize probeTotal = static_cast<VkDeviceSize>(probes);
    nearFastLight = VulkanBuffer{device.getAllocator(), probeTotal * probeFlagBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Gpu};
    probeTurns = VulkanBuffer{device.getAllocator(), probeTotal * sizeof(GpuProbeTurn), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload};
    writeProbeTurns(turnsIndependent);
    rayCapacity = 0;

    // Written by the GPU straight into memory the CPU reads: a few bytes per probe asked for, when the camera moves a cell.
    for (RelocationFrame& relocation : relocationFrames) {
        relocation.records = VulkanBuffer{device.getAllocator(), probeTotal * sizeof(GpuProbeRelocation), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                          BufferMemory::Readback};
        relocation.asked.clear();
    }

    relocations.clear();
    relocationWanted.clear();
    relocateAll = true;
    states.clear();
    restarts.clear();
    updatedFrames = 0;
}

/*
* The rays are sized for the most the settings ask for, so the ray budget's divisions never reallocate; asking for more
* waits for the frames in flight to finish with the old buffers.
*/
void VulkanDdgi::reserveRays(int raysPerProbe) {
    if (raysPerProbe <= rayCapacity) {
        return;
    }

    if (rayCapacity > 0) {
        device.waitIdle();
    }

    const VkDeviceSize rayTotal = static_cast<VkDeviceSize>(probes) * static_cast<VkDeviceSize>(raysPerProbe);
    rays = VulkanBuffer{device.getAllocator(), rayTotal * rayResultBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Gpu};
    fastRays = VulkanBuffer{device.getAllocator(), rayTotal * fastRayResultBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Gpu};
    rayCapacity = raysPerProbe;
}

/*
* The turns are written where the GPU reads them, so a change waits for every frame in flight to finish with the old
* ones: a hitch, but only when sharing is switched on or off. The frame's own fence has passed, so the records this frame
* in flight asked for when it last ran are ready.
*/
void VulkanDdgi::prepare(VkCommandBuffer commands, std::span<const ProbeCascade> next, const GlobalIlluminationSettings& settings,
                         std::span<const DrawItem> draws, const Vec3& cameraOrigin, std::uint32_t frameIndex) {
    if (next.empty()) {
        throw std::invalid_argument("The light probes need at least one cascade");
    }

    for (const ProbeCascade& cascade : next) {
        validateProbeVolume(cascade.volume);
    }

    if (static_cast<long long>(probeCount(next.front().volume.counts)) * static_cast<long long>(next.size()) > maxProbesPerVolume) {
        throw std::invalid_argument("The light probes' cascades may hold at most " + std::to_string(maxProbesPerVolume) + " probes");
    }

    sharing = settings.probeSharing > 0.0f || settings.fastProbeSharing > 0.0f;
    std::vector<int> scrolled;

    if (!sameLayout(cascades, next)) {
        turnsIndependent = sharing;
        createAtlases(commands, next);
        irradiance.transition(commands, ImageState::SurfaceRead);
        fastIrradiance.transition(commands, ImageState::SurfaceRead);
        distance.transition(commands, ImageState::SurfaceRead);
        sharedIrradiance.transition(commands, ImageState::SurfaceRead);
        sharedFastIrradiance.transition(commands, ImageState::SurfaceRead);
        freshAtlases = true;
    } else {
        if (sharing != turnsIndependent) {
            device.waitIdle();
            turnsIndependent = sharing;
            writeProbeTurns(turnsIndependent);
        }

        for (std::size_t c = 0; c < next.size(); ++c) {
            const std::vector<int> brought = scrolledProbes(cascades[c], next[c]);
            scrolled.insert(scrolled.end(), brought.begin(), brought.end());
        }
    }

    cascades.assign(next.begin(), next.end());

    // A plane a scroll brought in waits, off, for its own relocation.
    for (const int probe : scrolled) {
        if (!relocations.empty()) {
            relocations[static_cast<std::size_t>(probe)] = pendingProbeRelocation();
        }
    }

    relocationWanted.insert(relocationWanted.end(), scrolled.begin(), scrolled.end());
    collectRelocation(frameIndex);

    // Switching relocation on or off relocates again: off, the probes are classified where the grid puts them.
    if (!relocations.empty() && settings.probeRelocation != relocatedWithMoves) {
        relocateAll = true;
    }

    reserveRays(std::max(std::clamp(settings.raysPerProbe, minRaysPerProbe, maxRaysPerProbe),
                         std::clamp(settings.fastRaysPerProbe, minRaysPerProbe, maxRaysPerProbe)));

    // The probes that start: those the states start, and those a scroll brought in that are traced at once (before any
    // records, when every probe is traced where the grid puts it), whose light is another place's.
    std::vector<GpuProbeState> nextStates = buildCascadeStates(cascades, relocations, movingBounds(draws), settings);
    restartedByStates = restartedProbes(states, nextStates);

    for (const int probe : scrolled) {
        if (probeTraced(static_cast<ProbeState>(nextStates[static_cast<std::size_t>(probe)].state))) {
            restartedByStates.push_back(probe);
        }
    }

    std::sort(restartedByStates.begin(), restartedByStates.end());
    restartedByStates.erase(std::unique(restartedByStates.begin(), restartedByStates.end()), restartedByStates.end());
    states = std::move(nextStates);
    stateUploads.at(frameIndex).write(states);
    cascadeUploads.at(frameIndex).write(packProbeCascades(cascades, cameraOrigin));

    // A seed is the coarser cascades' light, so it waits until they hold more frames than a seed stands for.
    const bool seedsReady = settings.seedProbes && !freshAtlases && static_cast<float>(updatedFrames) > probeSeedFrames;
    frameSeeds = seedsReady ? chooseProbeSeeds(restartedByStates, cascades) : std::vector<ProbeSeed>{};
}

void VulkanDdgi::collectRelocation(std::uint32_t frameIndex) {
    RelocationFrame& relocation = relocationFrames.at(frameIndex);

    if (relocation.asked.empty()) {
        return;
    }

    relocation.records.invalidate();
    const auto* found = static_cast<const GpuProbeRelocation*>(relocation.records.getMapped());

    if (relocations.empty()) {
        relocations.assign(static_cast<std::size_t>(probes), pendingProbeRelocation());
    }

    keepProbeRelocations(relocations, relocation.asked, std::span<const GpuProbeRelocation>{found, relocation.asked.size()}, cascades);
    relocation.asked.clear();
}

void VulkanDdgi::collectAllRelocations() {
    for (std::uint32_t frameIndex = 0; frameIndex < relocationFrames.size(); ++frameIndex) {
        collectRelocation(frameIndex);
    }
}

void VulkanDdgi::writeProbeTurns(bool independent) {
    const std::vector<GpuProbeTurn> turns = packProbeTurns(probes, independent);
    std::memcpy(probeTurns.getMapped(), turns.data(), turns.size() * sizeof(GpuProbeTurn));
    probeTurns.flush();
}

void VulkanDdgi::update(VkCommandBuffer commands, FrameDescriptors& descriptors, const ProbeSceneSets& sets, const GlobalIlluminationSettings& settings,
                        const Vec3& sky, float frameSeconds, bool fastLights, std::span<const ProbeEvent> events, std::uint32_t frameIndex) {
    if (cascades.empty()) {
        throw std::logic_error("Prepare the light probes for their cascades before updating them");
    }

    const bool fresh = std::exchange(freshAtlases, false);
    ProbeFrame frame = scheduleProbeFrame(schedule, settings, frameSeconds, fastLights, fresh);
    frame.sky = sky;
    lastPacked = packProbeVolume(counts, static_cast<int>(cascades.size()), frame);

    // Written to this frame in flight's upload memory, whose last reads finished with its fence.
    std::vector<ProbeVolume> volumes;

    for (const ProbeCascade& cascade : cascades) {
        volumes.push_back(cascade.volume);
    }

    advanceProbeRestarts(restarts, volumes, events, frame, restartedByStates, frameSeeds);
    UploadArray& frameRestarts = restartUploads.at(frameIndex);
    frameRestarts.write(restarts);
    const UploadArray& frameStates = stateUploads.at(frameIndex);
    const UploadArray& frameCascades = cascadeUploads.at(frameIndex);

    // A changed level moves what the probes may land in: relocate them all again, for a later frame's states.
    if (std::any_of(events.begin(), events.end(), [](const ProbeEvent& event) { return event.kind == ProbeEventKind::GeometryChange; })) {
        relocateAll = true;
    }

    if (relocateAll || !relocationWanted.empty()) {
        relocate(commands, descriptors, sets, settings.probeRelocation ? probeRelocationIterations : 0, frameIndex);
        relocatedWithMoves = settings.probeRelocation;
    }

    if (!frameSeeds.empty()) {
        seed(commands, descriptors, sets, frameIndex);
    }

    trace(commands, descriptors, sets, probeRayStride(frame));

    // The change statistics are only ever read and written by this update, so they stay in storage use between frames
    // (each frame's transition still orders its update after the last one's writes).
    const std::array<VulkanImage*, 3> irradianceAtlases{&irradiance, &fastIrradiance, &changeStatistics};
    const std::array<VulkanImage*, 1> distanceAtlas{&distance};
    blend(commands, descriptors, *irradiancePipeline, irradianceAtlases, frameRestarts, frameStates, frameCascades);
    blend(commands, descriptors, *distancePipeline, distanceAtlas, frameRestarts, frameStates, frameCascades);

    if (sharing) {
        shareProbes(commands, descriptors, settings, frame.irradianceHysteresis, frameRestarts, frameStates, frameCascades);
    }

    // The blends' writes made visible to every later read: this frame's main pass and debug balls, the next one's trace.
    irradiance.transition(commands, ImageState::SurfaceRead);
    fastIrradiance.transition(commands, ImageState::SurfaceRead);
    distance.transition(commands, ImageState::SurfaceRead);
    sharedIrradiance.transition(commands, ImageState::SurfaceRead);
    sharedFastIrradiance.transition(commands, ImageState::SurfaceRead);
    ++updatedFrames;
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
* sampled (last frame's main pass and balls, this frame's trace and seeds), whose reads must finish first.
*/
void VulkanDdgi::shareProbes(VkCommandBuffer commands, FrameDescriptors& descriptors, const GlobalIlluminationSettings& settings, float longKeep,
                             const UploadArray& frameRestarts, const UploadArray& frameStates, const UploadArray& frameCascades) {
    const std::array<VulkanImage*, 5> images{&irradiance, &fastIrradiance, &distance, &sharedIrradiance, &sharedFastIrradiance};
    const VkDescriptorSet set = descriptors.allocate(sharePipeline->getResourceLayout());
    writeUpload(set, 0, frameRestarts);
    writeUpload(set, 1, frameStates);
    writeUpload(set, 2, frameCascades);
    writeStorageImages(commands, device.getDevice(), set, 3, images);

    const GpuProbeShare packed = packProbeShare(counts, static_cast<int>(cascades.size()), settings.probeSharing, settings.fastProbeSharing,
                                                settings.eventProbeSharing, longKeep);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, sharePipeline->get());
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, sharePipeline->getLayout(), 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(commands, sharePipeline->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(packed), &packed);
    vkCmdDispatch(commands, static_cast<std::uint32_t>(probes), 1, 1);
}

/*
* Every ray of every probe, one invocation each, with room for the most rays any probe traces (the stride): a probe that
* traces fewer leaves the rest of its room unwritten, and its invocations there stop at once. The ray buffers are shared
* by the frames in flight: last frame's update may still be reading them, so the writes wait for those reads (nothing
* to flush after a read), and the update's reads wait for these writes.
*/
void VulkanDdgi::trace(VkCommandBuffer commands, FrameDescriptors& descriptors, const ProbeSceneSets& sets, int rayStride) {
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
* One workgroup per seed, reading last frame's atlases as the trace does (still readable), writing the seeds buffer,
* which last frame's update may still be reading (so the writes wait for those reads), and this frame's update reads
* after these writes.
*/
void VulkanDdgi::seed(VkCommandBuffer commands, FrameDescriptors& descriptors, const ProbeSceneSets& sets, std::uint32_t frameIndex) {
    std::vector<std::int32_t> seededProbes;

    for (const ProbeSeed& seeded : frameSeeds) {
        seededProbes.push_back(seeded.probe);
    }

    UploadArray& frameSeedProbes = seedUploads.at(frameIndex);
    frameSeedProbes.write(seededProbes);

    bufferBarrier(commands, seeds.get(), {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE},
                  {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});

    const VkDescriptorSet seedSet = descriptors.allocate(seedSetLayout.get());
    writeUpload(seedSet, 0, frameSeedProbes);
    const std::array<const VulkanBuffer*, 1> written{&seeds};
    writeBuffers(device.getDevice(), seedSet, 1, written);

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, seedPipeline->get());
    const std::array<VkDescriptorSet, 4> boundSets{sets.textures, sets.frame, sets.rayScene, seedSet};
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, seedLayout.get(), 0, static_cast<std::uint32_t>(boundSets.size()),
                            boundSets.data(), 0, nullptr);
    vkCmdPushConstants(commands, seedLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(lastPacked), &lastPacked);
    vkCmdDispatch(commands, static_cast<std::uint32_t>(seededProbes.size()), 1, 1);

    bufferBarrier(commands, seeds.get(), {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
                  {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT});
}

/*
* One workgroup per probe blends the rays into the atlases, in place. Each atlas goes to storage use from being sampled
* (this frame's trace and seeds, last frame's main pass and balls), whose reads must finish before it is written.
*/
void VulkanDdgi::blend(VkCommandBuffer commands, FrameDescriptors& descriptors, const ComputePipeline& pipeline, std::span<VulkanImage* const> atlases,
                       const UploadArray& frameRestarts, const UploadArray& frameStates, const UploadArray& frameCascades) {
    const VkDescriptorSet set = descriptors.allocate(pipeline.getResourceLayout());
    const std::array<const VulkanBuffer*, 4> buffers = probeBuffers();
    writeBuffers(device.getDevice(), set, 0, buffers);

    // The restarts, the states, the cascades and the seeds after the probe buffers, then the atlases.
    const std::uint32_t next = static_cast<std::uint32_t>(buffers.size());
    writeUpload(set, next, frameRestarts);
    writeUpload(set, next + 1, frameStates);
    writeUpload(set, next + 2, frameCascades);
    const std::array<const VulkanBuffer*, 1> seeded{&seeds};
    writeBuffers(device.getDevice(), set, next + 3, seeded);
    writeStorageImages(commands, device.getDevice(), set, next + 4, atlases);

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.get());
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getLayout(), 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(commands, pipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(lastPacked), &lastPacked);
    vkCmdDispatch(commands, static_cast<std::uint32_t>(probes), 1, 1);
}

/*
* One workgroup per probe asked for, its record written straight into this frame in flight's read-back memory, in the
* order asked; the barrier makes the writes visible to the CPU once the frame's fence has passed. The records were last
* read by the CPU after that fence, so the writes need nothing to wait for.
*/
void VulkanDdgi::relocate(VkCommandBuffer commands, FrameDescriptors& descriptors, const ProbeSceneSets& sets, int iterations, std::uint32_t frameIndex) {
    std::vector<std::int32_t> asked;

    if (relocateAll) {
        for (int probe = 0; probe < probes; ++probe) {
            asked.push_back(probe);
        }
    } else {
        asked.assign(relocationWanted.begin(), relocationWanted.end());
        std::sort(asked.begin(), asked.end());
        asked.erase(std::unique(asked.begin(), asked.end()), asked.end());
    }

    relocateAll = false;
    relocationWanted.clear();

    RelocationFrame& relocation = relocationFrames.at(frameIndex);
    relocation.asked = relocationRequests(cascades, std::vector<int>(asked.begin(), asked.end()));
    relocation.requests->write(asked);

    const VkDescriptorSet recordSet = descriptors.allocate(relocateSetLayout.get());
    writeUpload(recordSet, 0, *relocation.requests);
    const std::array<const VulkanBuffer*, 1> records{&relocation.records};
    writeBuffers(device.getDevice(), recordSet, 1, records);

    const GpuProbeRelocate packed{{counts.x, counts.y, counts.z, iterations}};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, relocatePipeline->get());
    const std::array<VkDescriptorSet, 4> boundSets{sets.textures, sets.frame, sets.rayScene, recordSet};
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, relocateLayout.get(), 0, static_cast<std::uint32_t>(boundSets.size()),
                            boundSets.data(), 0, nullptr);
    vkCmdPushConstants(commands, relocateLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(packed), &packed);
    vkCmdDispatch(commands, static_cast<std::uint32_t>(asked.size()), 1, 1);

    bufferBarrier(commands, relocation.records.get(), {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
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
    vkCmdDraw(commands, 6, static_cast<std::uint32_t>(probes), 0, 0);
}

bool VulkanDdgi::hasProbes() const {
    return !cascades.empty();
}

VkDescriptorBufferInfo VulkanDdgi::describeProbeStates(std::uint32_t frameIndex) const {
    return stateUploads.at(frameIndex).describe();
}

VkDescriptorBufferInfo VulkanDdgi::describeProbeCascades(std::uint32_t frameIndex) const {
    return cascadeUploads.at(frameIndex).describe();
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
