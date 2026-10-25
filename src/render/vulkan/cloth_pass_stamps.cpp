#include "render/vulkan/cloth_pass_stamps.h"

#include "render/vulkan/vulkan_check.h"
#include "render/vulkan/vulkan_device.h"

#include <stdexcept>

ClothPassStamps::ClothPassStamps(const VulkanDevice& device, std::uint32_t frameCount) : device(device), frames(frameCount) {
    for (Frame& frame : frames) {
        frame.pool = device.createTimestampPool(maxClothPassStamps);
        frame.kinds.reserve(maxClothPassStamps - 1);
    }
}

// The whole pool is reset, since how many stamps the frame will write is not known until it has recorded them.
void ClothPassStamps::begin(VkCommandBuffer commands, std::uint32_t frame) {
    if (frame >= frames.size()) {
        throw std::invalid_argument("ClothPassStamps: no such frame");
    }
    current = frame;
    Frame& stamps = frames[frame];
    stamps.kinds.clear();
    stamps.begun = true;
    stamps.overflowed = false;
    vkCmdResetQueryPool(commands, stamps.pool.get(), 0, maxClothPassStamps);
    vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, stamps.pool.get(), 0);
}

void ClothPassStamps::mark(VkCommandBuffer commands, ClothPass kind) {
    Frame& stamps = frames[current];
    if (!stamps.begun || stamps.overflowed) {
        return;
    }
    if (stamps.kinds.size() + 1 >= maxClothPassStamps) {
        stamps.overflowed = true;
        return;
    }
    stamps.kinds.push_back(kind);
    vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, stamps.pool.get(), static_cast<std::uint32_t>(stamps.kinds.size()));
}

// VK_NOT_READY would mean a stamp is missing (the frame was abandoned): that frame is simply not reported.
ClothPassTimings ClothPassStamps::read(std::uint32_t frame) {
    if (frame >= frames.size()) {
        throw std::invalid_argument("ClothPassStamps: no such frame");
    }
    Frame& stamps = frames[frame];
    if (!stamps.begun) {
        return {};
    }
    stamps.begun = false;
    if (stamps.overflowed) {
        return {};
    }

    std::vector<std::uint64_t> ticks(stamps.kinds.size() + 1);
    const VkResult result = vkGetQueryPoolResults(device.getDevice(), stamps.pool.get(), 0, static_cast<std::uint32_t>(ticks.size()),
                                                  ticks.size() * sizeof(std::uint64_t), ticks.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
    if (result == VK_NOT_READY) {
        return {};
    }
    vulkanCheck(result, "Reading the cloth's GPU timestamps failed");
    return clothPassTimingsFromTicks(ticks, stamps.kinds, device.getTimestampPeriod(), device.getTimestampValidBits());
}
