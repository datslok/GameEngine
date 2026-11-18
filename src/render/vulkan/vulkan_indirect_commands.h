#pragma once

#include "render/gpu/gpu_scene.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <cstdint>
#include <span>
#include <vector>

class VulkanDevice;

/*
* A frame's indirect draw commands (GpuDrawCommand), written by the CPU as each pass records its batches and read by
* vkCmdDrawIndexedIndirect. One per frame in flight, in upload memory, in chunks of indirectChunkCommands: a frame
* that needs more takes another chunk instead of growing a buffer its earlier commands already point into, and keeps it
* for later frames. First stage of GPU-driven drawing (docs/research/GPUDRIVEN.md): later the GPU writes the counts.
*/
class VulkanIndirectCommands {
public:
    explicit VulkanIndirectCommands(const VulkanDevice& device);

    // A new frame (its fence passed): every chunk free again.
    void reset();

    // Where a command was written: the buffer and the byte offset to draw from.
    struct Slot {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
    };

    Slot write(const GpuDrawCommand& command);

    // Commands side by side in one chunk (a multi-draw reads them so), from the slot returned; at most a chunk's worth.
    // Throws std::invalid_argument for more.
    Slot writeRun(std::span<const GpuDrawCommand> commands);

    // Before the frame is submitted: the CPU's writes made visible to the GPU, where memory is not kept coherent.
    void flush() const;

private:
    const VulkanDevice& device;
    std::vector<VulkanBuffer> chunks;
    std::size_t chunk = 0;      // the chunk being written
    std::uint32_t written = 0;  // commands in it so far
};

// Commands per chunk: 4096 of 20 bytes (80 KB); the forest's frames draw about 2,500 batches.
inline constexpr std::uint32_t indirectChunkCommands = 4096;
