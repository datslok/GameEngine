#include "render/vulkan/vulkan_indirect_commands.h"
#include "render/vulkan/vulkan_device.h"

#include <cstring>

VulkanIndirectCommands::VulkanIndirectCommands(const VulkanDevice& device):
    device(device)
{
}

void VulkanIndirectCommands::reset() {
    chunk = 0;
    written = 0;
}

/*
* Into the current chunk, or the next one (made the first time a frame needs it) when this one is full. Commands
* already written keep their chunk, so the draws recorded with them stay valid.
*/
VulkanIndirectCommands::Slot VulkanIndirectCommands::write(const GpuDrawCommand& command) {
    if (written == indirectChunkCommands) {
        ++chunk;
        written = 0;
    }

    if (chunk == chunks.size()) {
        chunks.emplace_back(device.getAllocator(), static_cast<VkDeviceSize>(indirectChunkCommands) * sizeof(GpuDrawCommand),
                            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, BufferMemory::Upload);
    }

    const VkDeviceSize offset = static_cast<VkDeviceSize>(written) * sizeof(GpuDrawCommand);
    std::memcpy(static_cast<std::byte*>(chunks[chunk].getMapped()) + offset, &command, sizeof(command));
    ++written;
    return Slot{chunks[chunk].get(), offset};
}

void VulkanIndirectCommands::flush() const {
    for (std::size_t used = 0; used < chunks.size() && used <= chunk; ++used) {
        chunks[used].flush();
    }
}
