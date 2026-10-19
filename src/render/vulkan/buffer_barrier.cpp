#include "render/vulkan/buffer_barrier.h"

/*
* A buffer has no layouts, unlike an image, so its barrier only orders the work and moves the data between caches. The
* queue family stays the same (one queue does everything), so no ownership changes hands.
*/
void bufferBarrier(VkCommandBuffer commands, VkBuffer buffer, const BufferUse& before, const BufferUse& after) {
    VkBufferMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = before.stages;
    barrier.srcAccessMask = before.access;
    barrier.dstStageMask = after.stages;
    barrier.dstAccessMask = after.access;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commands, &dependency);
}
