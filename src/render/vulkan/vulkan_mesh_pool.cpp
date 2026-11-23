#include "render/vulkan/vulkan_mesh_pool.h"
#include "render/mesh_upload.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/vulkan_device.h"

#include <cstring>

/*
* Every pool can be reached by address (a ray's hit reads its triangle's corners); with ray tracing, acceleration
* structures are built from it. Copies in and out for growing.
*/
VulkanMeshPool::VulkanMeshPool(const VulkanDevice& device, ImmediateCommands& uploads):
    device(device),
    uploads(uploads)
{
    VkBufferUsageFlags shared = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    if (device.supportsRayQueries()) {
        shared |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }

    vertexUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | shared;
    indexUsage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | shared;
    clusterUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
}

void VulkanMeshPool::ensure(VulkanBuffer& pool, std::uint64_t& capacity, std::uint64_t used, std::uint64_t needed, std::uint64_t smallest,
                            VkDeviceSize elementBytes, VkBufferUsageFlags usage) {
    const std::uint64_t grown = poolCapacity(capacity, needed, smallest, elementBytes);

    if (grown != capacity) {
        grow(pool, used, grown, elementBytes, usage);
        capacity = grown;
    }
}

/*
* Grown first where it does not fit, then copied in through an upload buffer; the barrier makes the copy visible to
* everything that reads meshes later (meshUploadBarrier). Then its clusters.
*/
MeshPlacement VulkanMeshPool::add(const IndexedMesh& mesh, const MeshClusters& clusters) {
    checkMeshUpload(mesh);
    const std::uint64_t vertexCount = mesh.vertices.size();
    const std::uint64_t indexCount = mesh.indices.size();
    ensure(vertices, vertexCapacity, layout.getVertices(), layout.getVertices() + vertexCount, smallestPoolVertices, sizeof(MeshVertex), vertexUsage);
    ensure(indices, indexCapacity, layout.getIndices(), layout.getIndices() + indexCount, smallestPoolIndices, sizeof(std::uint32_t), indexUsage);
    MeshPlacement placement = layout.place(static_cast<std::uint32_t>(vertexCount), static_cast<std::uint32_t>(indexCount));
    const VkDeviceSize vertexSize = vertexBytes(mesh);
    const VkDeviceSize indexSize = indexBytes(mesh);

    VulkanBuffer staging{device.getAllocator(), vertexSize + indexSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Upload};
    auto* destination = static_cast<std::byte*>(staging.getMapped());
    std::memcpy(destination, mesh.vertices.data(), vertexSize);
    std::memcpy(destination + vertexSize, mesh.indices.data(), indexSize);
    staging.flush();

    uploads.run([&](VkCommandBuffer commands) {
        const VkBufferCopy vertexCopy{0, VkDeviceSize{placement.firstVertex} * sizeof(MeshVertex), vertexSize};
        vkCmdCopyBuffer(commands, staging.get(), vertices.get(), 1, &vertexCopy);
        const VkBufferCopy indexCopy{vertexSize, VkDeviceSize{placement.firstIndex} * sizeof(std::uint32_t), indexSize};
        vkCmdCopyBuffer(commands, staging.get(), indices.get(), 1, &indexCopy);
        meshUploadBarrier(commands, device);
    });

    addClusters(clusters, mesh.swayBindings, placement);
    return placement;
}

/*
* Packed with the vertex pool's numbers, then copied in as the mesh was. The records are kept on the CPU too, for
* cluster-compare.
*/
void VulkanMeshPool::addClusters(const MeshClusters& built, std::span<const SwayBinding> bindings, MeshPlacement& placement) {
    std::uint32_t words = 0;

    for (const MeshCluster& cluster : built.clusters) {
        words += meshClusterTriangleWords(cluster.triangleCount);
    }

    const std::uint64_t clusterCount = built.clusters.size();
    ensure(clusters, clusterCapacity, layout.getClusters(), layout.getClusters() + clusterCount, smallestPoolClusters, sizeof(GpuMeshCluster), clusterUsage);
    ensure(clusterVertices, clusterVertexCapacity, layout.getClusterVertices(), layout.getClusterVertices() + built.vertices.size(),
           smallestPoolClusterVertices, sizeof(std::uint32_t), clusterUsage);
    ensure(triangleWords, triangleWordCapacity, layout.getTriangleWords(), layout.getTriangleWords() + words, smallestPoolTriangleWords,
           sizeof(std::uint32_t), clusterUsage);
    const ClusterPlacement place =
        layout.placeClusters(static_cast<std::uint32_t>(clusterCount), static_cast<std::uint32_t>(built.vertices.size()), words);
    const PackedMeshClusters packed = packMeshClusters(built, placement.firstVertex, place.firstVertex, place.firstTriangleWord, bindings);
    placement.firstCluster = place.firstCluster;
    placement.clusterCount = static_cast<std::uint32_t>(clusterCount); // every level: the culling passes' cut picks among them
    clusterRecords.insert(clusterRecords.end(), packed.clusters.begin(), packed.clusters.end());

    if (clusterCount == 0) {
        return;
    }

    const VkDeviceSize recordBytes = packed.clusters.size() * sizeof(GpuMeshCluster);
    const VkDeviceSize vertexBytes = packed.vertices.size() * sizeof(std::uint32_t);
    const VkDeviceSize wordBytes = packed.triangleWords.size() * sizeof(std::uint32_t);
    VulkanBuffer staging{device.getAllocator(), recordBytes + vertexBytes + wordBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Upload};
    auto* destination = static_cast<std::byte*>(staging.getMapped());
    std::memcpy(destination, packed.clusters.data(), recordBytes);
    std::memcpy(destination + recordBytes, packed.vertices.data(), vertexBytes);
    std::memcpy(destination + recordBytes + vertexBytes, packed.triangleWords.data(), wordBytes);
    staging.flush();

    uploads.run([&](VkCommandBuffer commands) {
        const VkBufferCopy recordCopy{0, VkDeviceSize{place.firstCluster} * sizeof(GpuMeshCluster), recordBytes};
        vkCmdCopyBuffer(commands, staging.get(), clusters.get(), 1, &recordCopy);
        const VkBufferCopy vertexCopy{recordBytes, VkDeviceSize{place.firstVertex} * sizeof(std::uint32_t), vertexBytes};
        vkCmdCopyBuffer(commands, staging.get(), clusterVertices.get(), 1, &vertexCopy);
        const VkBufferCopy wordCopy{recordBytes + vertexBytes, VkDeviceSize{place.firstTriangleWord} * sizeof(std::uint32_t), wordBytes};
        vkCmdCopyBuffer(commands, staging.get(), triangleWords.get(), 1, &wordCopy);
        meshUploadBarrier(commands, device);
    });
}

VkBuffer VulkanMeshPool::getClusters() const {
    return clusters.get();
}

VkDeviceAddress VulkanMeshPool::getClusterAddress() const {
    return clusters.getDeviceAddress();
}

VkDeviceAddress VulkanMeshPool::getClusterVertexAddress() const {
    return clusterVertices.getDeviceAddress();
}

VkDeviceAddress VulkanMeshPool::getTriangleWordAddress() const {
    return triangleWords.getDeviceAddress();
}

const std::vector<GpuMeshCluster>& VulkanMeshPool::getClusterRecords() const {
    return clusterRecords;
}

/*
* The frames in flight may still draw from the old pool, so the GPU is waited for before it is replaced. The copy is a
* transfer like the uploads, made visible by the same barrier.
*/
void VulkanMeshPool::grow(VulkanBuffer& pool, std::uint64_t used, std::uint64_t capacity, VkDeviceSize elementBytes, VkBufferUsageFlags usage) {
    VulkanBuffer grown{device.getAllocator(), capacity * elementBytes, usage, BufferMemory::Gpu};

    if (used > 0) {
        device.waitIdle();
        uploads.run([&](VkCommandBuffer commands) {
            const VkBufferCopy copy{0, 0, used * elementBytes};
            vkCmdCopyBuffer(commands, pool.get(), grown.get(), 1, &copy);
            meshUploadBarrier(commands, device);
        });
    }

    pool = std::move(grown);
}

VkBuffer VulkanMeshPool::getVertices() const {
    return vertices.get();
}

VkBuffer VulkanMeshPool::getIndices() const {
    return indices.get();
}

VkDeviceAddress VulkanMeshPool::getVertexAddress() const {
    return vertices.getDeviceAddress();
}

VkDeviceAddress VulkanMeshPool::getIndexAddress() const {
    return indices.getDeviceAddress();
}

std::uint64_t VulkanMeshPool::getUsedBytes() const {
    return layout.getVertices() * sizeof(MeshVertex) + layout.getIndices() * sizeof(std::uint32_t) + layout.getClusters() * sizeof(GpuMeshCluster) +
           (layout.getClusterVertices() + layout.getTriangleWords()) * sizeof(std::uint32_t);
}

std::uint64_t VulkanMeshPool::getHeldBytes() const {
    return vertexCapacity * sizeof(MeshVertex) + indexCapacity * sizeof(std::uint32_t) + clusterCapacity * sizeof(GpuMeshCluster) +
           (clusterVertexCapacity + triangleWordCapacity) * sizeof(std::uint32_t);
}

/*
* After a mesh's copies, one barrier makes the copied data visible to everything that reads it later: vertex attribute
* fetching and index reading, shaders reading the buffers through their addresses (a ray's hit rebuilding its surface),
* and, with ray tracing, building the mesh's acceleration structure. Waiting for the upload to finish (ImmediateCommands
* does) orders it in time, but only a barrier makes the GPU's own writes visible to its later reads, including those in
* later submissions.
*/
void meshUploadBarrier(VkCommandBuffer commands, const VulkanDevice& device) {
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                            VK_ACCESS_2_TRANSFER_READ_BIT;

    if (device.supportsRayQueries()) {
        barrier.dstStageMask |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        barrier.dstAccessMask |= VK_ACCESS_2_SHADER_READ_BIT;
    }

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commands, &dependency);
}
