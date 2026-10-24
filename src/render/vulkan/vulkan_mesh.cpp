#include "render/vulkan/vulkan_mesh.h"
#include "render/mesh_upload.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/vulkan_device.h"

#include <cstring>
#include <stdexcept>

/*
* After the copies, one barrier makes the copied data visible to everything that reads it later: vertex attribute
* fetching and index reading, shaders reading the buffers through their addresses (a ray's hit rebuilding its surface),
* and, with ray tracing, building the mesh's acceleration structure. Waiting for the upload to finish (ImmediateCommands
* does) orders it in time, but only a barrier makes the GPU's own writes visible to its later reads, including those in
* later submissions.
*/
VulkanMesh::VulkanMesh(const VulkanDevice& device, ImmediateCommands& uploads, const IndexedMesh& mesh, bool deforming):
    deforming(deforming)
{
    checkMeshUpload(mesh);

    const std::uint32_t vertexSize = vertexBytes(mesh);
    const std::uint32_t indexSize = indexBytes(mesh);
    indexCount = static_cast<std::uint32_t>(mesh.indices.size());
    vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());

    // Every mesh can be reached by address; with ray tracing it is also what acceleration structures are built from.
    VkBufferUsageFlags shared = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    if (device.supportsRayQueries()) {
        shared |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }

    const VmaAllocator allocator = device.getAllocator();
    vertices = VulkanBuffer{allocator, vertexSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | shared, BufferMemory::Gpu};

    // The CPU writes a deforming mesh's slots itself; its writes reach the GPU when the frame is submitted.
    if (deforming) {
        for (VulkanBuffer& slot : shapeSlots) {
            slot = VulkanBuffer{allocator, vertexSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | (shared & ~VK_BUFFER_USAGE_TRANSFER_DST_BIT), BufferMemory::Upload};
            std::memcpy(slot.getMapped(), mesh.vertices.data(), vertexSize);
            slot.flush();
        }
    }
    indices = VulkanBuffer{allocator, indexSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | shared, BufferMemory::Gpu};

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
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
                               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

        if (device.supportsRayQueries()) {
            barrier.dstStageMask |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            barrier.dstAccessMask |= VK_ACCESS_2_SHADER_READ_BIT;
        }

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commands, &dependency);
    });
}

bool VulkanMesh::isDeforming() const {
    return deforming;
}

void VulkanMesh::showShape(std::span<const MeshVertex> shape, std::uint64_t fingerprint) {
    if (!deforming) {
        throw std::logic_error("Only a deforming mesh changes shape");
    }
    if (shape.size() != vertexCount) {
        throw std::invalid_argument("A deforming mesh's shape needs one vertex per vertex of its rest shape");
    }
    if (fingerprint == slotShapes[slots.current()]) {
        slots.hold();
        return;
    }

    const std::uint32_t slot = slots.advance();
    std::memcpy(shapeSlots[slot].getMapped(), shape.data(), shape.size_bytes());
    shapeSlots[slot].flush();
    slotShapes[slot] = fingerprint;
}

void VulkanMesh::holdShape() {
    slots.hold();
}

bool VulkanMesh::changedShape() const {
    return deforming && slots.changed();
}

std::uint32_t VulkanMesh::getSlot() const {
    return deforming ? slots.current() : 0;
}

std::uint64_t VulkanMesh::getSlotVersion() const {
    return deforming ? slots.version(slots.current()) : 1;
}

VkDeviceAddress VulkanMesh::getSlotVertexAddress(std::uint32_t slot) const {
    return deforming ? shapeSlots.at(slot).getDeviceAddress() : vertices.getDeviceAddress();
}

VkBuffer VulkanMesh::getVertexBuffer() const {
    return deforming ? shapeSlots[slots.current()].get() : vertices.get();
}

VkBuffer VulkanMesh::getPreviousVertexBuffer() const {
    return deforming ? shapeSlots[slots.previous()].get() : vertices.get();
}

VkBuffer VulkanMesh::getIndexBuffer() const {
    return indices.get();
}

std::uint32_t VulkanMesh::getIndexCount() const {
    return indexCount;
}

std::uint32_t VulkanMesh::getVertexCount() const {
    return vertexCount;
}

VkDeviceAddress VulkanMesh::getVertexAddress() const {
    return getSlotVertexAddress(getSlot());
}

VkDeviceAddress VulkanMesh::getIndexAddress() const {
    return indices.getDeviceAddress();
}
