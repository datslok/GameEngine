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
}

/*
* Grown first where it does not fit, then copied in through an upload buffer; the barrier makes the copy visible to
* everything that reads meshes later (meshUploadBarrier).
*/
MeshPlacement VulkanMeshPool::add(const IndexedMesh& mesh) {
    checkMeshUpload(mesh);
    const std::uint64_t vertexCount = mesh.vertices.size();
    const std::uint64_t indexCount = mesh.indices.size();
    const std::uint64_t neededVertices = poolCapacity(vertexCapacity, layout.getVertices() + vertexCount, smallestPoolVertices, sizeof(MeshVertex));
    const std::uint64_t neededIndices = poolCapacity(indexCapacity, layout.getIndices() + indexCount, smallestPoolIndices, sizeof(std::uint32_t));

    if (neededVertices != vertexCapacity) {
        grow(vertices, layout.getVertices(), neededVertices, sizeof(MeshVertex), vertexUsage);
        vertexCapacity = neededVertices;
    }

    if (neededIndices != indexCapacity) {
        grow(indices, layout.getIndices(), neededIndices, sizeof(std::uint32_t), indexUsage);
        indexCapacity = neededIndices;
    }

    const MeshPlacement placement = layout.place(static_cast<std::uint32_t>(vertexCount), static_cast<std::uint32_t>(indexCount));
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

    return placement;
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
    return layout.getVertices() * sizeof(MeshVertex) + layout.getIndices() * sizeof(std::uint32_t);
}

std::uint64_t VulkanMeshPool::getHeldBytes() const {
    return vertexCapacity * sizeof(MeshVertex) + indexCapacity * sizeof(std::uint32_t);
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
