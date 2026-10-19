#include "render/vulkan/vulkan_ddgi.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/vulkan_check.h"
#include "render/vulkan/vulkan_device.h"

#include <algorithm>
#include <array>
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

    bool sameVolume(const ProbeVolume& a, const ProbeVolume& b) {
        return a.origin.x == b.origin.x && a.origin.y == b.origin.y && a.origin.z == b.origin.z && a.spacing.x == b.spacing.x &&
               a.spacing.y == b.spacing.y && a.spacing.z == b.spacing.z && a.counts.x == b.counts.x && a.counts.y == b.counts.y &&
               a.counts.z == b.counts.z;
    }

    OwnedHandle<VkPipelineLayout> createPipelineLayout(VkDevice device, std::span<const VkDescriptorSetLayout> sets, VkShaderStageFlags pushStages,
                                                       const char* failure) {
        const VkPushConstantRange push{pushStages, 0, sizeof(GpuProbeVolume)};

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

    // Both read the three ray buffers; the irradiance update writes two atlases, the distance update one.
    ComputePipelineDescription update;
    update.storageBuffers = 3;
    update.storageImages = 2;
    update.pushConstantBytes = sizeof(GpuProbeVolume);
    update.shader = "assets/shaders/ddgi_update_irradiance.comp.spv";
    irradiancePipeline = std::make_unique<ComputePipeline>(device, update);
    update.storageImages = 1;
    update.shader = "assets/shaders/ddgi_update_distance.comp.spv";
    distancePipeline = std::make_unique<ComputePipeline>(device, update);

    createBallsPipeline(layouts, texturesLayout, frameLayout, colourFormat, depthFormat);
}

/*
* The trace reads the scene the way the ray debug view does (every texture, the frame's instances, materials and lights,
* and the ray tracing set), plus a fourth set of its own: the buffers it writes the rays into.
*/
void VulkanDdgi::createTracePipeline(VkDescriptorSetLayout texturesLayout, VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout rayLayout) {
    raysLayout = createUniformLayout(device.getDevice(), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3, VK_SHADER_STAGE_COMPUTE_BIT,
                                     "Probe ray set layout creation failed");

    const std::array<VkDescriptorSetLayout, 4> sets{texturesLayout, frameLayout, rayLayout, raysLayout.get()};
    traceLayout = createPipelineLayout(device.getDevice(), sets, VK_SHADER_STAGE_COMPUTE_BIT, "Probe trace layout creation failed");

    ComputePipelineDescription trace;
    trace.shader = "assets/shaders/ddgi_trace.comp.spv";
    trace.sharedLayout = traceLayout.get();
    tracePipeline = std::make_unique<ComputePipeline>(device, trace);
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
    distance = createAtlas(distanceFormat, probeDistanceTexels, "Probe distance atlas");

    const VkDeviceSize probes = static_cast<VkDeviceSize>(probeCount(volume.counts));
    rays = VulkanBuffer{device.getAllocator(), probes * maxRaysPerProbe * rayResultBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Gpu};
    fastRays = VulkanBuffer{device.getAllocator(), probes * maxRaysPerProbe * fastRayResultBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Gpu};
    nearFastLight = VulkanBuffer{device.getAllocator(), probes * probeFlagBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Gpu};
    currentVolume = volume;
}

void VulkanDdgi::prepare(VkCommandBuffer commands, const ProbeVolume& volume) {
    validateProbeVolume(volume);

    if (!currentVolume || !sameVolume(*currentVolume, volume)) {
        createAtlases(commands, volume);
        irradiance.transition(commands, ImageState::SurfaceRead);
        fastIrradiance.transition(commands, ImageState::SurfaceRead);
        distance.transition(commands, ImageState::SurfaceRead);
        freshAtlases = true;
    }
}

void VulkanDdgi::update(VkCommandBuffer commands, FrameDescriptors& descriptors, const ProbeSceneSets& sets, const ProbeVolume& volume,
                        const GlobalIlluminationSettings& settings, const Vec3& sky, float frameSeconds, bool fastLights) {
    if (!currentVolume || !sameVolume(*currentVolume, volume)) {
        throw std::logic_error("Prepare the light probes for a volume before updating them");
    }

    const bool fresh = std::exchange(freshAtlases, false);
    ProbeFrame frame = scheduleProbeFrame(schedule, settings, frameSeconds, fastLights, fresh);
    frame.sky = sky;
    lastPacked = packProbeVolume(volume, frame);

    const int probes = probeCount(volume.counts);
    trace(commands, descriptors, sets, probes, probeRayStride(frame));

    const std::array<VulkanImage*, 2> irradianceAtlases{&irradiance, &fastIrradiance};
    const std::array<VulkanImage*, 1> distanceAtlas{&distance};
    blend(commands, descriptors, *irradiancePipeline, irradianceAtlases, probes);
    blend(commands, descriptors, *distancePipeline, distanceAtlas, probes);

    // The blends' writes made visible to every later read: this frame's main pass and debug balls, the next one's trace.
    irradiance.transition(commands, ImageState::SurfaceRead);
    fastIrradiance.transition(commands, ImageState::SurfaceRead);
    distance.transition(commands, ImageState::SurfaceRead);
}

std::array<const VulkanBuffer*, 3> VulkanDdgi::rayBuffers() const {
    return {&rays, &fastRays, &nearFastLight};
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
    writeBuffers(device.getDevice(), raySet, 0, rayBuffers());

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
                       int probes) {
    const VkDescriptorSet set = descriptors.allocate(pipeline.getResourceLayout());
    const std::array<const VulkanBuffer*, 3> buffers = rayBuffers();
    writeBuffers(device.getDevice(), set, 0, buffers);

    std::vector<VkDescriptorImageInfo> imageInfos;

    for (VulkanImage* atlas : atlases) {
        atlas->transition(commands, ImageState::ComputeStorage);
        imageInfos.push_back(VkDescriptorImageInfo{VK_NULL_HANDLE, atlas->getView(), VK_IMAGE_LAYOUT_GENERAL});
    }

    std::vector<VkWriteDescriptorSet> writes(atlases.size());

    for (std::size_t index = 0; index < atlases.size(); ++index) {
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = set;
        writes[index].dstBinding = static_cast<std::uint32_t>(buffers.size() + index);
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

// Six corners (two triangles) per ball, one instance per probe.
void VulkanDdgi::drawProbes(VkCommandBuffer commands, FrameDescriptors& descriptors, VkDescriptorSet textures, VkDescriptorSet frame) {
    if (!hasProbes()) {
        return;
    }

    const VkDescriptorSet atlasSet = descriptors.allocate(ballsSetLayout.get());
    const std::array<VkDescriptorImageInfo, 2> imageInfos{VkDescriptorImageInfo{sampler, irradiance.getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                                          VkDescriptorImageInfo{sampler, fastIrradiance.getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
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

SampledImage VulkanDdgi::sampledIrradiance() const {
    return SampledImage{irradiance.getView(), sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

SampledImage VulkanDdgi::sampledDistance() const {
    return SampledImage{distance.getView(), sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

SampledImage VulkanDdgi::sampledFastIrradiance() const {
    return SampledImage{fastIrradiance.getView(), sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

VulkanImage& VulkanDdgi::getFastIrradianceAtlas() {
    return fastIrradiance;
}

VulkanImage& VulkanDdgi::getIrradianceAtlas() {
    return irradiance;
}

VulkanImage& VulkanDdgi::getDistanceAtlas() {
    return distance;
}
