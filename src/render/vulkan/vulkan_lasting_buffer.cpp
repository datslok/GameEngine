#include "render/vulkan/vulkan_lasting_buffer.h"
#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/vulkan_device.h"

#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
    constexpr VkDeviceSize smallestStagingBytes = 4096;
}

VulkanLastingBuffer::VulkanLastingBuffer(const VulkanDevice& device, std::size_t recordBytes, VkBufferUsageFlags usage):
    device(device),
    recordBytes(recordBytes),
    usage(usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
{
    reserve(smallestRecords);
}

bool VulkanLastingBuffer::reserve(std::size_t count) {
    const std::size_t wanted = lastingCapacity(count, capacity, smallestRecords);

    if (wanted == capacity) {
        return false;
    }

    if (capacity > 0) {
        device.waitIdle();
    }

    buffer = VulkanBuffer{device.getAllocator(), static_cast<VkDeviceSize>(wanted * recordBytes), usage, BufferMemory::Gpu};
    capacity = wanted;
    return true;
}

/*
* Before the copies: every earlier command (earlier frames' shaders reading these records) finished. After: the copies'
* writes visible to every later stage that reads storage buffers (culling, vertex, fragment and compute shaders).
*/
void VulkanLastingBuffer::write(VkCommandBuffer commands, std::uint32_t frameIndex, std::span<const LastingWrite> writes) {
    const std::vector<LastingCopy> copies = planLastingCopies(writes, recordBytes);
    const std::size_t bytes = stagedBytes(copies);
    copiedBytes = bytes;

    if (copies.empty()) {
        return;
    }

    if (copies.back().target + copies.back().bytes > capacity * recordBytes) {
        throw std::logic_error("A lasting buffer was written past its room: reserve first");
    }

    VulkanBuffer& stage = staging.at(frameIndex);

    if (stage.getSize() < bytes) {
        stage = VulkanBuffer{device.getAllocator(), std::max<VkDeviceSize>({bytes, 2 * stage.getSize(), smallestStagingBytes}),
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Upload};
    }

    std::byte* mapped = static_cast<std::byte*>(stage.getMapped());
    std::size_t staged = 0;

    for (const LastingWrite& write : writes) {
        const std::size_t writeBytes = write.count * recordBytes;

        if (writeBytes > 0) {
            std::memcpy(mapped + staged, write.data, writeBytes);
            staged += writeBytes;
        }
    }

    stage.flush();
    std::vector<VkBufferCopy> regions;
    regions.reserve(copies.size());

    for (const LastingCopy& copy : copies) {
        regions.push_back(VkBufferCopy{copy.staging, copy.target, copy.bytes});
    }

    bufferBarrier(commands, buffer.get(), BufferUse{VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
                  BufferUse{VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT});
    vkCmdCopyBuffer(commands, stage.get(), buffer.get(), static_cast<std::uint32_t>(regions.size()), regions.data());
    bufferBarrier(commands, buffer.get(), BufferUse{VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
                  BufferUse{VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT});
}

VkDescriptorBufferInfo VulkanLastingBuffer::describe() const {
    return VkDescriptorBufferInfo{buffer.get(), 0, VK_WHOLE_SIZE};
}

VkBuffer VulkanLastingBuffer::get() const {
    return buffer.get();
}

std::size_t VulkanLastingBuffer::getCopiedBytes() const {
    return copiedBytes;
}
