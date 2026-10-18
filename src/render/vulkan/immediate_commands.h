#pragma once

#include "render/vulkan/owned_handle.h"

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
