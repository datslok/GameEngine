#pragma once

#include "render/gpu/ray_tracing.h"
#include "render/vulkan/owned_handle.h"
#include "render/vulkan/scene_layout.h"
#include "render/vulkan/vulkan_memory.h"
#include "scene/frame_description.h"

#include "volk/volk.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class ImmediateCommands;
class VulkanDevice;
class VulkanMesh;

/*
* Hardware ray tracing's acceleration structures (render/gpu/ray_tracing.h) and the descriptor set shaders reach them
* through (ray_scene.glsl): binding 0 the frame's top-level structure, binding 1 every mesh's vertex and index buffer
* addresses, numbered like the meshes. Only made when the GPU supports ray queries.
*
* Each mesh gets a bottom-level structure when it is uploaded: built once (preferring fast tracing over a fast build),
* then compacted, copied into a buffer of just the size the finished tree needs (often about half), and the original
* dropped. Each frame in flight has its own top-level structure, rebuilt from scratch from that frame's instances, so
* moving objects need no bookkeeping; it grows (to twice its size, or what is needed) when a frame has more instances.
*/
class VulkanRayTracing {
public:
    VulkanRayTracing(const VulkanDevice& device, std::uint32_t framesInFlight);

    VulkanRayTracing(const VulkanRayTracing&) = delete;
    VulkanRayTracing& operator=(const VulkanRayTracing&) = delete;

    // Between frames only (the GPU idle): every frame's structure and set are made again.
    void setFramesInFlight(std::uint32_t count);

    // Build the next mesh's structure (meshes are numbered in upload order, like their handles). Waits for the GPU.
    void addMesh(ImmediateCommands& uploads, const VulkanMesh& mesh);
    std::size_t getMeshCount() const;

    // The frame's draws as instances: instance i is draw i, with its mesh's structure, its matrix and its mask. Throws
    // std::logic_error for a draw whose mesh has no structure yet.
    std::vector<GpuRayInstance> packInstances(const std::vector<DrawItem>& draws) const;

    // Record building this frame's top-level structure from the instances (outside any rendering block), and point the
    // frame's set at it and at the mesh table. Leaves it readable by ray queries in fragment and compute shaders.
    void buildScene(VkCommandBuffer commands, std::uint32_t frameIndex, const std::vector<GpuRayInstance>& instances);

    // The frame's set, after buildScene, and its layout.
    VkDescriptorSet getSceneSet(std::uint32_t frameIndex) const;
    VkDescriptorSetLayout getSetLayout() const;

private:
    // A structure and the buffer it lives in.
    struct AccelerationStructure {
        VulkanBuffer storage;
        OwnedHandle<VkAccelerationStructureKHR> handle;
        VkDeviceAddress address = 0;
    };

    struct MeshStructure {
        AccelerationStructure structure;
        VkDeviceAddress vertices = 0;
        VkDeviceAddress indices = 0;
    };

    struct Frame {
        AccelerationStructure scene;
        std::uint32_t capacity = 0; // the most instances the structure and scratch were sized for
        VulkanBuffer scratch;
        std::unique_ptr<UploadArray> instances;
        std::unique_ptr<UploadArray> meshTable;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };

    const VulkanDevice& device;
    OwnedHandle<VkDescriptorSetLayout> setLayout;
    OwnedHandle<VkDescriptorPool> pool;
    std::vector<MeshStructure> meshes;
    std::vector<Frame> frames;

    AccelerationStructure createStructure(VkAccelerationStructureTypeKHR type, VkDeviceSize size) const;
    VulkanBuffer createScratch(VkDeviceSize size) const;
    VkDeviceAddress scratchAddress(const VulkanBuffer& scratch) const;
};
