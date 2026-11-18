#pragma once

#include "render/gpu/mesh_pool.h"
#include "render/vulkan/vulkan_memory.h"
#include "scene/indexed_mesh.h"
#include "volk/volk.h"

#include <cstdint>

class ImmediateCommands;
class VulkanDevice;

/*
* The shared mesh buffer's two pools in GPU memory (render/gpu/mesh_pool.h): every mesh that does not deform, its
* vertices in one buffer and its indices in another, bound once a pass. A mesh is copied in after the last; when it does
* not fit, a pool is made twice as large (poolCapacity), the old one copied into it on the GPU and freed. Meshes arrive
* between frames, and growing waits for the GPU (frames in flight still read the old pools).
*/
class VulkanMeshPool {
public:
    VulkanMeshPool(const VulkanDevice& device, ImmediateCommands& uploads);

    // The mesh copied in after the last one; its place.
    MeshPlacement add(const IndexedMesh& mesh);

    VkBuffer getVertices() const;
    VkBuffer getIndices() const;

    // The pools' GPU addresses, which move when a pool grows: read them when they are used.
    VkDeviceAddress getVertexAddress() const;
    VkDeviceAddress getIndexAddress() const;

    // Bytes the meshes take, and the pools' sizes (F4's MESHES line).
    std::uint64_t getUsedBytes() const;
    std::uint64_t getHeldBytes() const;

private:
    // A pool grown to hold capacity elements, its first used elements copied over.
    void grow(VulkanBuffer& pool, std::uint64_t used, std::uint64_t capacity, VkDeviceSize elementBytes, VkBufferUsageFlags usage);

    const VulkanDevice& device;
    ImmediateCommands& uploads;
    VkBufferUsageFlags vertexUsage = 0;
    VkBufferUsageFlags indexUsage = 0;
    MeshPoolLayout layout;
    VulkanBuffer vertices;
    VulkanBuffer indices;
    std::uint64_t vertexCapacity = 0;
    std::uint64_t indexCapacity = 0;
};

// After copying mesh data on the GPU: the copies made visible to everything that reads meshes (render/vulkan/vulkan_mesh_pool.cpp).
void meshUploadBarrier(VkCommandBuffer commands, const VulkanDevice& device);
