#pragma once

#include "render/vulkan/vulkan_memory.h"
#include "scene/indexed_mesh.h"

#include <cstdint>

class ImmediateCommands;
class VulkanDevice;

/*
* A mesh in GPU memory: a vertex buffer of MeshVertex and an index buffer of 32-bit indices, three per triangle. The
* data goes through an upload buffer the CPU can write, then a copy on the GPU into memory only it can see, which is
* where vertex fetching is fastest.
*/
class VulkanMesh {
public:
    VulkanMesh(const VulkanDevice& device, ImmediateCommands& uploads, const IndexedMesh& mesh);

    VkBuffer getVertexBuffer() const;
    VkBuffer getIndexBuffer() const;
    std::uint32_t getIndexCount() const;

private:
    VulkanBuffer vertices;
    VulkanBuffer indices;
    std::uint32_t indexCount = 0;
};
