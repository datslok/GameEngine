#include "render/vulkan/vulkan_mesh.h"
#include "render/mesh_upload.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/vulkan_device.h"
#include "render/vulkan/vulkan_mesh_pool.h"

#include <cstring>
#include <stdexcept>

/*
* Into the pools when there are pools and the mesh does not deform; otherwise buffers of its own, copied in the same way
* and made visible by the same barrier (meshUploadBarrier).
*/
VulkanMesh::VulkanMesh(const VulkanDevice& device, ImmediateCommands& uploads, const IndexedMesh& mesh, bool deforming, VulkanMeshPool* pool):
    deforming(deforming)
{
    checkMeshUpload(mesh);
    indexCount = static_cast<std::uint32_t>(mesh.indices.size());
    vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());
    placement = MeshPlacement{0, vertexCount, 0, indexCount, false};

    if (pool != nullptr && !deforming) {
        this->pool = pool;
        placement = pool->add(mesh);
        return;
    }

    const std::uint32_t vertexSize = vertexBytes(mesh);
    const std::uint32_t indexSize = indexBytes(mesh);

    // Every mesh can be reached by address; with ray tracing it is also what acceleration structures are built from.
    VkBufferUsageFlags shared = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    if (device.supportsRayQueries()) {
        shared |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }

    const VmaAllocator allocator = device.getAllocator();
    vertices = VulkanBuffer{allocator, vertexSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | shared, BufferMemory::Gpu};

    // The CPU writes a deforming mesh's slots itself; its writes reach the GPU when the frame is submitted. A cloth on
    // the GPU writes them in a compute pass instead (and the GPU tests copy them out to check them).
    if (deforming) {
        for (VulkanBuffer& slot : shapeSlots) {
            slot = VulkanBuffer{allocator, vertexSize,
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                    (shared & ~VK_BUFFER_USAGE_TRANSFER_DST_BIT),
                                BufferMemory::Upload};
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
        meshUploadBarrier(commands, device);
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

std::uint32_t VulkanMesh::advanceGpuShape(std::uint64_t shape) {
    if (!deforming) {
        throw std::logic_error("Only a deforming mesh changes shape");
    }
    const std::uint32_t slot = slots.advance();
    slotShapes[slot] = shape;
    return slot;
}

VkBuffer VulkanMesh::getSlotBuffer(std::uint32_t slot) const {
    return shapeSlots.at(slot).get();
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
    if (deforming) {
        return shapeSlots.at(slot).getDeviceAddress();
    }

    return pool != nullptr ? pool->getVertexAddress() + VkDeviceAddress{placement.firstVertex} * sizeof(MeshVertex) : vertices.getDeviceAddress();
}

VkBuffer VulkanMesh::getVertexBuffer() const {
    if (deforming) {
        return shapeSlots[slots.current()].get();
    }

    return pool != nullptr ? pool->getVertices() : vertices.get();
}

VkBuffer VulkanMesh::getPreviousVertexBuffer() const {
    if (deforming) {
        return shapeSlots[slots.previous()].get();
    }

    return pool != nullptr ? pool->getVertices() : vertices.get();
}

VkBuffer VulkanMesh::getIndexBuffer() const {
    return pool != nullptr ? pool->getIndices() : indices.get();
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
    return pool != nullptr ? pool->getIndexAddress() + VkDeviceAddress{placement.firstIndex} * sizeof(std::uint32_t) : indices.getDeviceAddress();
}

const MeshPlacement& VulkanMesh::getPlacement() const {
    return placement;
}
