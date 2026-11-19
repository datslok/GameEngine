#pragma once

#include "render/vulkan/owned_handle.h"
#include "render/vulkan/vulkan_memory.h"

#include "volk/volk.h"

#include <functional>

class VulkanDevice;

/*
* Work done once and waited for, outside the frame: uploading a mesh or a texture, clearing a new image. record() is
* given a fresh command buffer, which is then submitted, and run() returns when the GPU has finished it, so staging
* buffers can be freed right after. Waiting costs a little time per upload, which only happens while assets load.
*/
class ImmediateCommands {
public:
    explicit ImmediateCommands(const VulkanDevice& device);

    ImmediateCommands(const ImmediateCommands&) = delete;
    ImmediateCommands& operator=(const ImmediateCommands&) = delete;

    void run(const std::function<void(VkCommandBuffer)>& record);

private:
    const VulkanDevice& device;
    OwnedHandle<VkCommandPool> pool;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    OwnedHandle<VkFence> finished;
};

/*
* A buffer in GPU memory holding bytes copied through a staging buffer (waited for), with usage and
* VK_BUFFER_USAGE_TRANSFER_DST_BIT: for records that change rarely (the terrain's heights, per-vertex records, the
* planting field). Whoever replaces one the frames in flight may still read waits for the GPU first.
*/
VulkanBuffer gpuBufferHolding(const VulkanDevice& device, ImmediateCommands& uploads, const void* bytes, VkDeviceSize size, VkBufferUsageFlags usage);
