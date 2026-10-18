#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/vulkan_check.h"
#include "render/vulkan/vulkan_device.h"

#include <cstdint>
#include <limits>

ImmediateCommands::ImmediateCommands(const VulkanDevice& device):
    device(device),
    pool(device.createCommandPool()),
    finished(device.createFence(false))
{
    VkCommandBufferAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    info.commandPool = pool.get();
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;

    vulkanCheck(vkAllocateCommandBuffers(device.getDevice(), &info, &commands), "Upload command buffer allocation failed");
}

/*
* The pool is reset first, which resets its one command buffer. If recording throws, nothing is submitted; the next
* run() resets the half-recorded buffer anyway.
*/
void ImmediateCommands::run(const std::function<void(VkCommandBuffer)>& record) {
    const VkDevice handle = device.getDevice();
    vulkanCheck(vkResetCommandPool(handle, pool.get(), 0), "Upload command pool reset failed");

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vulkanCheck(vkBeginCommandBuffer(commands, &begin), "Upload command buffer start failed");

    record(commands);

    vulkanCheck(vkEndCommandBuffer(commands), "Upload command buffer end failed");

    VkCommandBufferSubmitInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandInfo.commandBuffer = commands;

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &commandInfo;

    const VkFence fence = finished.get();
    vulkanCheck(vkResetFences(handle, 1, &fence), "Upload fence reset failed");
    vulkanCheck(vkQueueSubmit2(device.getQueue(), 1, &submit, fence), "Upload submission failed");
    vulkanCheck(vkWaitForFences(handle, 1, &fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()), "Waiting for an upload failed");
}
