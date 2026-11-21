#pragma once

#include "render/gpu/mesh_cluster_culling.h"
#include "render/gpu/mesh_pool.h"
#include "render/vulkan/vulkan_memory.h"
#include "scene/indexed_mesh.h"
#include "volk/volk.h"

#include <cstdint>
#include <vector>

class ImmediateCommands;
class VulkanDevice;

/*
* The shared mesh buffer's pools in GPU memory (render/gpu/mesh_pool.h): every mesh that does not deform, its vertices in
* one buffer and its indices in another, bound once a pass; and its clusters (scene/mesh_clusters.h, packed by
* render/gpu/mesh_cluster_culling.h) in three more: records, vertex numbers, triangle words. A mesh is copied in after
* the last; when it does not fit, a pool is made twice as large (poolCapacity), the old one copied into it on the GPU and
* freed. Meshes arrive between frames, and growing waits for the GPU (frames in flight still read the old pools).
*/
class VulkanMeshPool {
public:
    VulkanMeshPool(const VulkanDevice& device, ImmediateCommands& uploads);

    // The mesh copied in after the last one, its clusters built and copied in after the last ones; its place.
    MeshPlacement add(const IndexedMesh& mesh);

    VkBuffer getVertices() const;
    VkBuffer getIndices() const;

    // The cluster pools and their GPU addresses (moving when a pool grows: read them when used), and a copy of every
    // cluster record (cluster-compare's reference).
    VkBuffer getClusters() const;
    VkDeviceAddress getClusterAddress() const;
    VkDeviceAddress getClusterVertexAddress() const;
    VkDeviceAddress getTriangleWordAddress() const;
    const std::vector<GpuMeshCluster>& getClusterRecords() const;

    // Seconds spent building clusters, all meshes together (measured when meshes load).
    double getClusterBuildSeconds() const;

    // The pools' GPU addresses, which move when a pool grows: read them when they are used.
    VkDeviceAddress getVertexAddress() const;
    VkDeviceAddress getIndexAddress() const;

    // Bytes the meshes take, and the pools' sizes (F4's MESHES line).
    std::uint64_t getUsedBytes() const;
    std::uint64_t getHeldBytes() const;

private:
    // A pool grown to hold capacity elements, its first used elements copied over.
    void grow(VulkanBuffer& pool, std::uint64_t used, std::uint64_t capacity, VkDeviceSize elementBytes, VkBufferUsageFlags usage);

    // A pool made at least needed elements large: grown when it does not fit, used elements kept.
    void ensure(VulkanBuffer& pool, std::uint64_t& capacity, std::uint64_t used, std::uint64_t needed, std::uint64_t smallest, VkDeviceSize elementBytes,
                VkBufferUsageFlags usage);

    // The mesh's clusters built, packed and copied in; their place.
    void addClusters(const IndexedMesh& mesh, MeshPlacement& placement);

    const VulkanDevice& device;
    ImmediateCommands& uploads;
    VkBufferUsageFlags vertexUsage = 0;
    VkBufferUsageFlags indexUsage = 0;
    VkBufferUsageFlags clusterUsage = 0;
    MeshPoolLayout layout;
    VulkanBuffer vertices;
    VulkanBuffer indices;
    VulkanBuffer clusters;
    VulkanBuffer clusterVertices;
    VulkanBuffer triangleWords;
    std::uint64_t vertexCapacity = 0;
    std::uint64_t indexCapacity = 0;
    std::uint64_t clusterCapacity = 0;
    std::uint64_t clusterVertexCapacity = 0;
    std::uint64_t triangleWordCapacity = 0;
    std::vector<GpuMeshCluster> clusterRecords;
    double clusterBuildSeconds = 0.0;
};

// After copying mesh data on the GPU: the copies made visible to everything that reads meshes (render/vulkan/vulkan_mesh_pool.cpp).
void meshUploadBarrier(VkCommandBuffer commands, const VulkanDevice& device);
