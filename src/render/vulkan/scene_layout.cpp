#include "render/vulkan/scene_layout.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/mesh_cluster_culling_shared.h"
#include "render/gpu/taa.h"
#include "render/vulkan/vulkan_check.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace {
    struct BindingKind {
        VkDescriptorType type;
        VkShaderStageFlags stages;
    };

    // What each binding of set 1 holds and which stages read it, in SceneBinding order.
    constexpr VkShaderStageFlags vertex = VK_SHADER_STAGE_VERTEX_BIT;
    constexpr VkShaderStageFlags fragment = VK_SHADER_STAGE_FRAGMENT_BIT;
    constexpr VkShaderStageFlags compute = VK_SHADER_STAGE_COMPUTE_BIT;

    constexpr std::array<BindingKind, SceneBindingCount> sceneBindings{{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, vertex | fragment | compute}, // frame data
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex | fragment | compute}, // instances (fragment and compute for ray hits)
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fragment | compute},          // materials
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fragment | compute},          // directional lights (compute for probe rays)
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fragment | compute},          // point lights
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fragment | compute},          // spotlights
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fragment | compute},          // shadow tiles (compute for the fog's grid)
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, fragment | compute},  // shadow atlas, compared
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, fragment | compute},  // shadow atlas, raw depths
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, fragment},            // BRDF table
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex | compute},            // draw list (compute for occlusion culling)
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fragment | compute},          // cluster masks
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, fragment | compute},  // probe irradiance (compute for probe rays' hits)
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, fragment | compute},  // probe distances
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, compute},                     // the lights probe rays' hits may need
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, fragment | compute},  // probe irradiance from fast lights
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex | fragment | compute}, // probe states (vertex for the probes' debug balls)
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fragment | compute},          // the flat ambient light
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, fragment | compute},  // the sky view table (probe rays that escape)
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fragment | compute},          // the atmosphere
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex | fragment | compute}, // the probes' cascades (vertex for the debug balls)
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex},                      // last frame's model matrices (the velocity pass)
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, fragment},            // the light mirrors reflect onto each pixel
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, fragment},            // the way it arrives
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fragment | compute},          // the mirror pairs (fragment for the probe's traced capture)
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, fragment},            // ambient occlusion
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fragment | compute},          // the glass a ray towards a light may cross
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fragment | compute},          // the frame's fog
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, fragment | compute},  // the fog integrated through its grid
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, fragment | compute},  // the sky visibility grid's points (compute for the fog)
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fragment | compute},          // its cascades
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, fragment},            // the sun's contact shadows
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, fragment},            // the local lights' contact shadows
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, fragment | compute},  // the coloured shadow maps' tint (compute for the fog)
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, fragment | compute},  // their nearest glass depths
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex},                      // the terrain's heights
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex},                      // the terrain's patches
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex | compute},            // the wind field's record (compute for the bones pass)
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, vertex | compute},    // the wind's springs (compute for the bones pass)
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, vertex | compute},    // and last frame's (the velocity pass, the bones pass)
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex},                      // the hanging vertices' anchors and arms
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex},                      // the anchors they hang from
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex},                      // the thinned meshes' cards
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex},                      // the planting cells' slots' plants (render/gpu/placed_drawing.h)
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex},                      // each placed part's instance in its plant's frame
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex},                      // the camera's placed entries
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex},                      // the camera's twig entries (render/gpu/twig_records.h)
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex},                      // the twigs
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex},                      // every pool vertex's binding to its bones (render/gpu/bone_records.h)
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex},                      // each twig's binding
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex},                      // the posed trees (render/gpu/bone_poses.h)
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex},                      // each plant's tree this frame
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex},                      // the bones' poses
    }};
}

/*
* With mesh shaders (render/gpu/mesh_cluster_culling.h), whatever the vertex shaders read the mesh shaders read too
* (they place vertices with the same code, triangle_vertex.glsl), and they push the style they draw after the vertex
* shaders' push constants. Naming the mesh stage needs the GPU's mesh shaders, so only then.
*/
SceneLayout::SceneLayout(VkDevice device, VkDescriptorSetLayout bindlessLayout, bool meshShaders) {
    std::array<VkDescriptorSetLayoutBinding, SceneBindingCount> bindings{};

    for (std::uint32_t binding = 0; binding < SceneBindingCount; ++binding) {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = sceneBindings[binding].type;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = sceneBindings[binding].stages;

        if (meshShaders && (sceneBindings[binding].stages & vertex) != 0) {
            bindings[binding].stageFlags |= VK_SHADER_STAGE_MESH_BIT_EXT;
        }
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    VkDescriptorSetLayout createdLayout = VK_NULL_HANDLE;
    vulkanCheck(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &createdLayout), "Scene descriptor layout creation failed");
    frameLayout = OwnedHandle<VkDescriptorSetLayout>{device, createdLayout, vkDestroyDescriptorSetLayout};

    const std::array<VkDescriptorSetLayout, 2> setLayouts{bindlessLayout, createdLayout};

    // The shadow pass and the captures push each tile's view, or each cube face's or mirror's view, for
    // their vertex shaders; the mesh shaders, the style they draw, after them.
    static_assert(std::max(sizeof(GpuShadowView), sizeof(GpuCaptureView)) == MESH_CLUSTER_PUSH_OFFSET, "The mesh shaders' push constants follow the vertex shaders'");
    const std::array<VkPushConstantRange, 2> pushRanges{
        VkPushConstantRange{VK_SHADER_STAGE_VERTEX_BIT, 0, MESH_CLUSTER_PUSH_OFFSET},
        VkPushConstantRange{VK_SHADER_STAGE_MESH_BIT_EXT, MESH_CLUSTER_PUSH_OFFSET, MESH_CLUSTER_PUSH_BYTES},
    };

    VkPipelineLayoutCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineInfo.setLayoutCount = static_cast<std::uint32_t>(setLayouts.size());
    pipelineInfo.pSetLayouts = setLayouts.data();
    pipelineInfo.pushConstantRangeCount = meshShaders ? 2u : 1u;
    pipelineInfo.pPushConstantRanges = pushRanges.data();

    VkPipelineLayout createdPipelineLayout = VK_NULL_HANDLE;
    vulkanCheck(vkCreatePipelineLayout(device, &pipelineInfo, nullptr, &createdPipelineLayout), "Scene pipeline layout creation failed");
    pipelineLayout = OwnedHandle<VkPipelineLayout>{device, createdPipelineLayout, vkDestroyPipelineLayout};
}

VkDescriptorSetLayout SceneLayout::getFrameLayout() const {
    return frameLayout.get();
}

VkPipelineLayout SceneLayout::getPipelineLayout() const {
    return pipelineLayout.get();
}

UploadArray::UploadArray(VmaAllocator allocator, VkBufferUsageFlags usage):
    allocator(allocator),
    usage(usage),
    buffer(allocator, smallestBytes, usage, BufferMemory::Upload)
{
}

void UploadArray::write(const void* data, std::size_t bytes) {
    writeParts(data, bytes, nullptr, 0);
}

void UploadArray::writeParts(const void* first, std::size_t firstBytes, const void* second, std::size_t secondBytes) {
    const std::size_t bytes = firstBytes + secondBytes;

    if (bytes > buffer.getSize()) {
        const std::size_t grown = std::max(bytes, 2 * static_cast<std::size_t>(buffer.getSize()));
        buffer = VulkanBuffer{allocator, grown, usage, BufferMemory::Upload};
    }

    if (firstBytes > 0) {
        std::memcpy(buffer.getMapped(), first, firstBytes);
    }

    if (secondBytes > 0) {
        std::memcpy(static_cast<std::byte*>(buffer.getMapped()) + firstBytes, second, secondBytes);
    }

    if (bytes > 0) {
        buffer.flush();
    }
}

// Grown buffers lose their contents, which is fine: every place is written each frame, by the CPU or the GPU.
void UploadArray::writeRuns(const DrawListLayout& layout) {
    const std::size_t bytes = static_cast<std::size_t>(layout.size()) * sizeof(std::uint32_t);

    if (bytes > buffer.getSize()) {
        const std::size_t grown = std::max(bytes, 2 * static_cast<std::size_t>(buffer.getSize()));
        buffer = VulkanBuffer{allocator, grown, usage, BufferMemory::Upload};
    }

    for (const DrawListLayout::Run& run : layout.getRuns()) {
        std::memcpy(static_cast<std::byte*>(buffer.getMapped()) + static_cast<std::size_t>(run.first) * sizeof(std::uint32_t), run.entries.data(),
                    run.entries.size() * sizeof(std::uint32_t));
    }

    if (!layout.getRuns().empty()) {
        buffer.flush();
    }
}

VkDescriptorBufferInfo UploadArray::describe() const {
    return VkDescriptorBufferInfo{buffer.get(), 0, VK_WHOLE_SIZE};
}

VkDeviceAddress UploadArray::getDeviceAddress() const {
    return buffer.getDeviceAddress();
}

SceneBuffers::SceneBuffers(VmaAllocator allocator):
    frameData(allocator, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT),
    directionalLights(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
    pointLights(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
    spotLights(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
    shadowTiles(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
    // Copied from too: cull-compare reads back the lists the GPU wrote into it (render/vulkan/vulkan_camera_culling.h).
    drawList(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT),
    probeHitLights(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
    skyLight(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
    atmosphere(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
    mirrorPairs(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
    glassBounds(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
    fog(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
    skyVisibilityCascades(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
    terrainPatches(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
    wind(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
    placedParts(allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
{
}

VkDescriptorSet writeSceneSet(FrameDescriptors& descriptors, const SceneLayout& layout, const SceneBuffers& buffers, const SceneImages& images,
                              const VulkanBuffer& clusterMasks, const SceneBufferViews& views) {
    const VkDescriptorSet set = descriptors.allocate(layout.getFrameLayout());

    const std::array<VkDescriptorBufferInfo, SceneBindingCount> bufferInfos{
        buffers.frameData.describe(),
        views.instances,
        views.materials,
        buffers.directionalLights.describe(),
        buffers.pointLights.describe(),
        buffers.spotLights.describe(),
        buffers.shadowTiles.describe(),
        VkDescriptorBufferInfo{},
        VkDescriptorBufferInfo{},
        VkDescriptorBufferInfo{},
        buffers.drawList.describe(),
        VkDescriptorBufferInfo{clusterMasks.get(), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{},
        VkDescriptorBufferInfo{},
        buffers.probeHitLights.describe(),
        VkDescriptorBufferInfo{},
        views.probeStates,
        views.skyLight,
        VkDescriptorBufferInfo{},
        buffers.atmosphere.describe(),
        views.probeCascades,
        views.previousModels,
        VkDescriptorBufferInfo{},
        VkDescriptorBufferInfo{},
        buffers.mirrorPairs.describe(),
        VkDescriptorBufferInfo{},
        buffers.glassBounds.describe(),
        buffers.fog.describe(),
        VkDescriptorBufferInfo{},
        VkDescriptorBufferInfo{},
        buffers.skyVisibilityCascades.describe(),
        VkDescriptorBufferInfo{},
        VkDescriptorBufferInfo{},
        VkDescriptorBufferInfo{},
        VkDescriptorBufferInfo{},
        views.terrainHeights,
        buffers.terrainPatches.describe(),
        buffers.wind.describe(),
        VkDescriptorBufferInfo{},
        VkDescriptorBufferInfo{},
        views.hangingVertices,
        views.lianaShapes,
        views.thinningCards,
        views.placedPlants,
        buffers.placedParts.describe(),
        views.placedEntries,
        views.twigEntries,
        views.twigs,
        views.swayBindings,
        views.twigBindings,
        views.boneTrees,
        views.bonePlantTrees,
        views.bonePoses,
    };

    const auto imageInfo = [](const SampledImage& image) { return VkDescriptorImageInfo{image.sampler, image.view, image.layout}; };
    std::array<VkDescriptorImageInfo, SceneBindingCount> imageInfos{};
    imageInfos[ShadowAtlasBinding] = imageInfo(images.shadowAtlas);
    imageInfos[ShadowDepthsBinding] = imageInfo(images.shadowDepths);
    imageInfos[BrdfTableBinding] = imageInfo(images.brdfTable);
    imageInfos[ProbeIrradianceBinding] = imageInfo(images.probeIrradiance);
    imageInfos[ProbeDistanceBinding] = imageInfo(images.probeDistance);
    imageInfos[ProbeFastIrradianceBinding] = imageInfo(images.probeFastIrradiance);
    imageInfos[SkyViewBinding] = imageInfo(images.skyView);
    imageInfos[MirroredLightBinding] = imageInfo(images.mirroredLight);
    imageInfos[MirroredDirectionBinding] = imageInfo(images.mirroredDirection);
    imageInfos[AmbientOcclusionBinding] = imageInfo(images.ambientOcclusion);
    imageInfos[FogVolumeBinding] = imageInfo(images.fogVolume);
    imageInfos[SkyVisibilityBinding] = imageInfo(images.skyVisibility);
    imageInfos[ContactShadowsBinding] = imageInfo(images.contactShadows);
    imageInfos[LightContactShadowsBinding] = imageInfo(images.lightContactShadows);
    imageInfos[GlassTintBinding] = imageInfo(images.glassTint);
    imageInfos[GlassDepthsBinding] = imageInfo(images.glassDepths);
    imageInfos[WindSpringsBinding] = imageInfo(images.windSprings);
    imageInfos[WindPreviousSpringsBinding] = imageInfo(images.windPreviousSprings);

    std::array<VkWriteDescriptorSet, SceneBindingCount> writes{};

    for (std::uint32_t binding = 0; binding < SceneBindingCount; ++binding) {
        VkWriteDescriptorSet& write = writes[binding];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = sceneBindings[binding].type;

        if (write.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            write.pImageInfo = &imageInfos[binding];
        }
        else {
            write.pBufferInfo = &bufferInfos[binding];
        }
    }

    vkUpdateDescriptorSets(descriptors.getDevice(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return set;
}

void bindScene(VkCommandBuffer commands, const SceneLayout& layout, VkDescriptorSet textures, VkDescriptorSet frameSet,
                VkPipelineBindPoint bindPoint) {
    const std::array<VkDescriptorSet, 2> sets{textures, frameSet};
    vkCmdBindDescriptorSets(commands, bindPoint, layout.getPipelineLayout(), bindlessTextureSet,
                            static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
}
