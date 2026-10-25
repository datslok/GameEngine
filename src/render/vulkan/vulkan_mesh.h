#pragma once

#include "render/gpu/deforming_slots.h"
#include "render/vulkan/vulkan_memory.h"
#include "scene/indexed_mesh.h"

#include <array>
#include <cstdint>
#include <span>

class ImmediateCommands;
class VulkanDevice;

/*
* A mesh in GPU memory: a vertex buffer of MeshVertex and an index buffer of 32-bit indices, three per triangle. The
* data goes through an upload buffer the CPU can write, then a copy on the GPU into memory only it can see, which is
* where vertex fetching is fastest.
*
* A deforming mesh (scene/deforming_mesh.h) instead keeps its vertices in a ring of buffers the CPU writes directly
* (render/gpu/deforming_slots.h), each holding the whole mesh in one shape, all starting with the rest shape; its
* indices never change, so they live in GPU memory as any mesh's. Every frame must either show a shape or hold.
*/
class VulkanMesh {
public:
    VulkanMesh(const VulkanDevice& device, ImmediateCommands& uploads, const IndexedMesh& mesh, bool deforming = false);

    bool isDeforming() const;

    // A deforming mesh's shape for this frame, after its frame's fence: written into the next slot, or held when it
    // is the shape already showing (the same fingerprint), so a still shape costs nothing and has no motion.
    // Throws std::invalid_argument for the wrong number of vertices, std::logic_error for a mesh that does not deform.
    void showShape(std::span<const MeshVertex> vertices, std::uint64_t shape);

    // No shape for this frame: the last one stands still.
    void holdShape();

    /*
    * A shape the GPU writes this frame (a cloth's, render/vulkan/vulkan_cloth.h): the next slot, as showShape would
    * take, returned for the shape pass to write into before anything reads it. The slots take storage writes for this.
    */
    std::uint32_t advanceGpuShape(std::uint64_t shape);
    VkBuffer getSlotBuffer(std::uint32_t slot) const;

    // Whether this frame's shape is new (a deforming mesh that showed a different shape).
    bool changedShape() const;

    // The slot the current shape is in and the times it has been written (an acceleration structure built from it
    // knows when it is stale), and a slot's vertices' GPU address. A mesh that does not deform has one slot, 0.
    std::uint32_t getSlot() const;
    std::uint64_t getSlotVersion() const;
    VkDeviceAddress getSlotVertexAddress(std::uint32_t slot) const;

    // The current shape's vertices (the only ones of a mesh that does not deform), and last frame's.
    VkBuffer getVertexBuffer() const;
    VkBuffer getPreviousVertexBuffer() const;
    VkBuffer getIndexBuffer() const;
    std::uint32_t getIndexCount() const;
    std::uint32_t getVertexCount() const;

    // The buffers' GPU addresses, for building acceleration structures and for shaders reading a ray hit's corners.
    VkDeviceAddress getVertexAddress() const;
    VkDeviceAddress getIndexAddress() const;

private:
    VulkanBuffer vertices;
    VulkanBuffer indices;

    // A deforming mesh's ring, the shape in each slot, and whose turn it is.
    bool deforming = false;
    std::array<VulkanBuffer, deformingSlotCount> shapeSlots;
    std::array<std::uint64_t, deformingSlotCount> slotShapes{};
    DeformingSlots slots;

    std::uint32_t indexCount = 0;
    std::uint32_t vertexCount = 0;
};
