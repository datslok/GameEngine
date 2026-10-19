#pragma once

#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/owned_handle.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/vulkan_memory.h"

#include "vma/vk_mem_alloc.h"
#include "volk/volk.h"

#include <cstddef>
#include <cstdint>
#include <vector>

/*
* The descriptor layout of the scene's pipelines (the main pass, the shadow pass and its tile reset), which left
* SDL_GPU's four sets behind with the GPU scene: set 0 is every texture (BindlessTextures, made once), set 1 the frame's
* scene (made once per frame and bound once per pass): its uniform block, the storage buffers of instances, materials,
* lights, shadow tiles and shadow instances, and the shadow atlas and BRDF table. Nothing is bound per draw.
*/
inline constexpr std::uint32_t bindlessTextureSet = 0;
inline constexpr std::uint32_t sceneFrameSet = 1;

// Set 1's bindings, as triangle.vert, triangle.frag and shadow.vert declare them.
enum SceneBinding : std::uint32_t {
    FrameDataBinding = 0,
    InstancesBinding = 1,
    MaterialsBinding = 2,
    DirectionalLightsBinding = 3,
    PointLightsBinding = 4,
    SpotLightsBinding = 5,
    ShadowTilesBinding = 6,
    ShadowAtlasBinding = 7,
    ShadowDepthsBinding = 8,
    BrdfTableBinding = 9,
    ShadowInstancesBinding = 10,
    SceneBindingCount = 11
};

// Set 1's layout and the pipeline layout of both sets, shared by every scene pipeline, so the sets bound for one stay
// bound for the next.
class SceneLayout {
public:
    SceneLayout(VkDevice device, VkDescriptorSetLayout bindlessLayout);

    SceneLayout(const SceneLayout&) = delete;
    SceneLayout& operator=(const SceneLayout&) = delete;

    VkDescriptorSetLayout getFrameLayout() const;
    VkPipelineLayout getPipelineLayout() const;

private:
    OwnedHandle<VkDescriptorSetLayout> frameLayout;
    OwnedHandle<VkPipelineLayout> pipelineLayout;
};

/*
* A buffer in upload memory that grows to fit what a frame writes into it, for one frame in flight: the CPU writes it
* while the GPU reads that frame's earlier use of it is long finished (the frame's fence). Growing replaces the buffer
* with one twice as big (or as big as needed), so a busy scene settles at its size after a frame or two.
*/
class UploadArray {
public:
    UploadArray(VmaAllocator allocator, VkBufferUsageFlags usage);

    // Replace the contents (growing first if needed) and make them visible to the GPU.
    void write(const void* data, std::size_t bytes);

    template<typename Element>
    void write(const std::vector<Element>& elements) {
        write(elements.data(), elements.size() * sizeof(Element));
    }

    // The whole buffer, for a descriptor (shaders read only as many elements as the frame says there are).
    VkDescriptorBufferInfo describe() const;

private:
    // Room for a few dozen records of any kind before the first growth.
    static constexpr std::size_t smallestBytes = 4096;

    VmaAllocator allocator = VK_NULL_HANDLE;
    VkBufferUsageFlags usage = 0;
    VulkanBuffer buffer;
};

// One frame in flight's scene data, in set 1's binding order.
struct SceneBuffers {
    explicit SceneBuffers(VmaAllocator allocator);

    UploadArray frameData;
    UploadArray instances;
    UploadArray materials;
    UploadArray directionalLights;
    UploadArray pointLights;
    UploadArray spotLights;
    UploadArray shadowTiles;
    UploadArray shadowInstances;
};

// The images set 1 points at: the shadow atlas read by comparison and as raw depths, and the BRDF table.
struct SceneImages {
    SampledImage shadowAtlas;
    SampledImage shadowDepths;
    SampledImage brdfTable;
};

// Write this frame's set 1 (from the frame's pools, so it goes when they are reset).
VkDescriptorSet writeSceneSet(FrameDescriptors& descriptors, const SceneLayout& layout, const SceneBuffers& buffers, const SceneImages& images);

// Bind both scene sets. Before a pass's draws: other passes' pipelines (the debug UI's) bind sets of their own.
void bindScene(VkCommandBuffer commands, const SceneLayout& layout, VkDescriptorSet textures, VkDescriptorSet frameSet);
