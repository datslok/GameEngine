#pragma once

#include "volk/volk.h"

/*
* Where in the pipeline a buffer is used and how: the stages that touch it, and whether they read or write it. A barrier
* between two uses waits for the first one's stages to finish before the second one's begin, and, if the first wrote,
* makes its writes visible to the second's accesses.
*/
struct BufferUse {
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
};

// One barrier on the whole of a buffer, from one use to the next. Recorded outside any rendering block.
void bufferBarrier(VkCommandBuffer commands, VkBuffer buffer, const BufferUse& before, const BufferUse& after);

// One barrier on all memory, from one use to the next: for work that touches many buffers (or none named), such as
// a chain of compute passes, an upload every later reader waits for, or acceleration structures.
void memoryBarrier(VkCommandBuffer commands, const BufferUse& before, const BufferUse& after);
