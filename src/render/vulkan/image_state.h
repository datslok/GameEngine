#pragma once

#include <vulkan/vulkan_core.h>

#include <cstdint>

/*
* What an image is being used for, as far as synchronisation cares. The GPU runs commands in an overlapping pipeline and
* keeps written data in caches, so when one pass writes an image and the next reads it, Vulkan must be told: a barrier
* names what came before (the stages to wait for and the writes to flush) and what comes after (the stages that must
* wait and the reads that must see the data), and moves the image to the layout its next use wants. Each state below
* bundles the stages, accesses and layout of one kind of use, so a barrier is just "from this state to that one".
*/
enum class ImageState {
    Undefined,           // just created: nothing to wait for, nothing worth keeping
    Acquired,            // a swapchain image just handed over; its semaphore is waited for at the colour output stage
    ColourTarget,        // drawn into (and blended onto) by a render pass
    DepthTarget,         // depth tested and written by a render pass
    FragmentRead,        // sampled by fragment shaders
    SurfaceRead,         // sampled by fragment and compute shaders alike (the scene's textures, which probe rays read too)
    DepthFragmentRead,   // a depth image sampled (and compared) by fragment shaders
    DepthComputeRead,    // a depth image read by compute shaders (the scene's depth, measured for the sun's cascades)
    ComputeRead,         // sampled by compute shaders (the light probes, read by the next frame's probe rays)
    ComputeStorage,      // read and written in place by compute shaders as a storage image (the light probes' update)
    TransferSource,      // copied or blitted from
    TransferDestination, // copied or blitted into
    Present              // handed to the screen
};

// Whether the old contents matter. Discarding lets the layout start from undefined, which costs nothing.
enum class ImageContents {
    Keep,
    Discard
};

// The pipeline stages and memory accesses of one state, and the layout it needs.
struct ImageStateUse {
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 reads = VK_ACCESS_2_NONE;
    VkAccessFlags2 writes = VK_ACCESS_2_NONE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

ImageStateUse imageStateUse(ImageState state);

// The stage at which a frame's commands wait for the acquire semaphore; ImageState::Acquired starts from it.
inline constexpr VkPipelineStageFlags2 acquireWaitStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

// Only reading after the same kind of reading is safe without a barrier: writes and layout changes need one.
bool needsBarrier(ImageState from, ImageState to);

/*
* The barrier between two uses of an image (mip levels firstLevel onwards, levelCount of them; every array layer, so a
* cube's six faces change state together). The
* source half waits for the old state's stages and flushes only its writes (reads leave nothing to flush); the
* destination half makes the data visible to everything the new state reads or writes.
*/
VkImageMemoryBarrier2 imageBarrier(VkImage image, VkImageAspectFlags aspect, ImageState from, ImageState to, ImageContents contents,
                                   std::uint32_t firstLevel = 0, std::uint32_t levelCount = VK_REMAINING_MIP_LEVELS);
