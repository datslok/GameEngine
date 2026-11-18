#pragma once

#include "render/frames_in_flight.h"
#include "render/gpu/lasting_buffers.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <array>
#include <cstddef>
#include <span>

class VulkanDevice;

/*
* Records kept in GPU memory from frame to frame (render/gpu/lasting_buffers.h, docs/research/GPUDRIVEN.md stage 7), one
* buffer every frame in flight reads: a frame copies only what changed, from its own staging buffer, by copies recorded
* in its commands before anything reads the records. The earlier frames' reads come first in the queue, so a barrier
* from every earlier command to the copies keeps them from overwriting what those frames still read; a second barrier
* makes the copies visible to this frame's shaders.
*/
class VulkanLastingBuffer {
public:
    // Records of recordBytes each, read by shaders as a storage buffer (usage added to it).
    VulkanLastingBuffer(const VulkanDevice& device, std::size_t recordBytes, VkBufferUsageFlags usage = 0);

    /*
    * Room for count records: true when the buffer was made afresh (bigger), its records gone, so the caller writes
    * them all this frame. Growing waits for the GPU first: frames in flight may still read the old buffer.
    */
    bool reserve(std::size_t count);

    // The writes staged in this frame's staging buffer (grown if needed; its frame's fence has passed) and copied, between
    // the two barriers. Nothing is recorded without writes.
    void write(VkCommandBuffer commands, std::uint32_t frameIndex, std::span<const LastingWrite> writes);

    // The whole buffer, for a descriptor (shaders read only the records the frame names), and the buffer itself.
    VkDescriptorBufferInfo describe() const;
    VkBuffer get() const;

    // Bytes the last write copied.
    std::size_t getCopiedBytes() const;

private:
    // Room for a few records before the first growth: a buffer is never empty.
    static constexpr std::size_t smallestRecords = 64;

    const VulkanDevice& device;
    std::size_t recordBytes = 0;
    VkBufferUsageFlags usage = 0;
    std::size_t capacity = 0;
    VulkanBuffer buffer;
    std::array<VulkanBuffer, maxFramesInFlight> staging;
    std::size_t copiedBytes = 0;
};
