#include "render/vulkan/vulkan_mesh.h"
#include "render/mesh_upload.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/vulkan_device.h"

#include <cstring>

/*
* After the copies, one barrier makes the copied data visible to the stages that read it: vertex attribute fetching
* and index reading. Waiting for the upload to finish (ImmediateCommands does) orders it in time, but only a barrier
* makes the GPU's own writes visible to its later reads.
*/
VulkanMesh::VulkanMesh(const VulkanDevice& device, ImmediateCommands& uploads, const IndexedMesh& mesh) {
    checkMeshUpload(mesh);

    const std::uint32_t vertexSize = vertexBytes(mesh);
    const std::uint32_t indexSize = indexBytes(mesh);
    indexCount = static_cast<std::uint32_t>(mesh.indices.size());

    const VmaAllocator allocator = device.getAllocator();
    vertices = VulkanBuffer{allocator, vertexSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Gpu};
    indices = VulkanBuffer{allocator, indexSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Gpu};

    VulkanBuffer staging{allocator, VkDeviceSize{vertexSize} + indexSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Upload};
    auto* destination = static_cast<std::byte*>(staging.getMapped());
    std::memcpy(destination, mesh.vertices.data(), vertexSize);
    std::memcpy(destination + vertexSize, mesh.indices.data(), indexSize);
    staging.flush();

    uploads.run([&](VkCommandBuffer commands) {
        const VkBufferCopy vertexCopy{0, 0, vertexSize};
        vkCmdCopyBuffer(commands, staging.get(), vertices.get(), 1, &vertexCopy);

        const VkBufferCopy indexCopy{vertexSize, 0, indexSize};
        vkCmdCopyBuffer(commands, staging.get(), indices.get(), 1, &indexCopy);

        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commands, &dependency);
    });
}

VkBuffer VulkanMesh::getVertexBuffer() const {
    return vertices.get();
}

VkBuffer VulkanMesh::getIndexBuffer() const {
    return indices.get();
}

std::uint32_t VulkanMesh::getIndexCount() const {
    return indexCount;
}
