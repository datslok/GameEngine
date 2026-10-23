#include "render/vulkan/image_state.h"

#include <cassert>

namespace {
    // Any handle will do: barriers are only described here, never recorded.
    VkImage someImage() {
        return reinterpret_cast<VkImage>(static_cast<std::uintptr_t>(0x1234));
    }

    /*
    * The main pass draws the scene image, then bloom and tone mapping sample it. The barrier waits for colour writes
    * to finish (the source half), makes them visible to sampling in fragment shaders (the destination half), and
    * changes the layout from one suited to drawing to one suited to reading.
    */
    void testDrawnImageBecomesReadable() {
        const VkImageMemoryBarrier2 barrier = imageBarrier(someImage(), VK_IMAGE_ASPECT_COLOR_BIT, ImageState::ColourTarget,
                                                           ImageState::FragmentRead, ImageContents::Keep);

        assert(barrier.sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
        assert(barrier.image == someImage());
        assert(barrier.srcStageMask == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
        assert(barrier.srcAccessMask == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        assert(barrier.dstStageMask == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
        assert(barrier.dstAccessMask == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        assert(barrier.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        assert(barrier.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        assert(barrier.subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
        assert(barrier.subresourceRange.baseMipLevel == 0);
        assert(barrier.subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS);
        assert(barrier.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
        assert(barrier.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    }

    /*
    * Reading after reading is no hazard: two passes may sample the same image in either order, so no barrier is
    * needed between them. Anything that writes, or changes the layout, needs one.
    */
    void testOnlyReadAfterReadNeedsNoBarrier() {
        assert(!needsBarrier(ImageState::FragmentRead, ImageState::FragmentRead));
        assert(!needsBarrier(ImageState::DepthFragmentRead, ImageState::DepthFragmentRead));
        assert(needsBarrier(ImageState::ColourTarget, ImageState::ColourTarget));
        assert(needsBarrier(ImageState::FragmentRead, ImageState::ColourTarget));
        assert(needsBarrier(ImageState::FragmentRead, ImageState::TransferSource));
    }

    /*
    * Overwriting an image that was being read (write after read): the writes must wait until the reads are done, but
    * reads leave nothing in caches to flush, so the source half names the stage and no access. Discarding the old
    * contents lets the layout start from undefined, which is free.
    */
    void testOverwritingWaitsForReadsWithoutFlushing() {
        const VkImageMemoryBarrier2 barrier = imageBarrier(someImage(), VK_IMAGE_ASPECT_COLOR_BIT, ImageState::FragmentRead,
                                                           ImageState::ColourTarget, ImageContents::Discard);

        assert(barrier.srcStageMask == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
        assert(barrier.srcAccessMask == VK_ACCESS_2_NONE);
        assert(barrier.dstStageMask == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
        assert(barrier.dstAccessMask == (VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT));
        assert(barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
        assert(barrier.newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }

    /*
    * A swapchain image is handed over by a semaphore that the frame's commands wait for at the colour output stage.
    * The first barrier must start from that same stage, so it waits for the semaphore too (a dependency chain);
    * starting from no stage at all would let the layout change race the presentation engine still reading the image.
    */
    void testAcquiredImageChainsWithTheSemaphore() {
        const VkImageMemoryBarrier2 barrier = imageBarrier(someImage(), VK_IMAGE_ASPECT_COLOR_BIT, ImageState::Acquired,
                                                           ImageState::ColourTarget, ImageContents::Discard);

        assert(barrier.srcStageMask == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
        assert(barrier.srcAccessMask == VK_ACCESS_2_NONE);
        assert(barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
        assert(acquireWaitStage == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    }

    // Presenting happens outside the pipeline, after a semaphore, so the barrier only finishes the writes and the layout.
    void testPresentTakesThePresentLayout() {
        const VkImageMemoryBarrier2 barrier = imageBarrier(someImage(), VK_IMAGE_ASPECT_COLOR_BIT, ImageState::ColourTarget,
                                                           ImageState::Present, ImageContents::Keep);

        assert(barrier.srcStageMask == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
        assert(barrier.srcAccessMask == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        assert(barrier.dstStageMask == VK_PIPELINE_STAGE_2_NONE);
        assert(barrier.dstAccessMask == VK_ACCESS_2_NONE);
        assert(barrier.newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }

    /*
    * The shadow atlas: depth written by the early and late depth tests, then compared and read in fragment shaders, in
    * the read-only depth layout. Kept between frames, so going back to drawing keeps its contents.
    */
    void testShadowAtlasRoundTrip() {
        const VkImageMemoryBarrier2 toRead = imageBarrier(someImage(), VK_IMAGE_ASPECT_DEPTH_BIT, ImageState::DepthTarget,
                                                          ImageState::DepthFragmentRead, ImageContents::Keep);
        const VkPipelineStageFlags2 depthTests =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

        assert(toRead.srcStageMask == depthTests);
        assert(toRead.srcAccessMask == VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        assert(toRead.dstStageMask == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
        assert(toRead.dstAccessMask == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        assert(toRead.oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        assert(toRead.newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        assert(toRead.subresourceRange.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT);

        const VkImageMemoryBarrier2 toDraw = imageBarrier(someImage(), VK_IMAGE_ASPECT_DEPTH_BIT, ImageState::DepthFragmentRead,
                                                          ImageState::DepthTarget, ImageContents::Keep);

        assert(toDraw.srcStageMask == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
        assert(toDraw.srcAccessMask == VK_ACCESS_2_NONE);
        assert(toDraw.dstStageMask == depthTests);
        assert(toDraw.dstAccessMask == (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT));
        assert(toDraw.oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    }

    /*
    * The scene's depth, measured for the cascades' fit: written by the depth tests, then read by a compute shader in the
    * read-only depth layout. Next frame's depth pass overwrites it, waiting only for that compute read to finish.
    */
    void testSceneDepthIsMeasuredInCompute() {
        const VkImageMemoryBarrier2 toMeasure = imageBarrier(someImage(), VK_IMAGE_ASPECT_DEPTH_BIT, ImageState::DepthTarget,
                                                             ImageState::DepthComputeRead, ImageContents::Keep);
        const VkPipelineStageFlags2 depthTests =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

        assert(toMeasure.srcStageMask == depthTests);
        assert(toMeasure.srcAccessMask == VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        assert(toMeasure.dstStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        assert(toMeasure.dstAccessMask == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        assert(toMeasure.newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

        const VkImageMemoryBarrier2 nextFrame = imageBarrier(someImage(), VK_IMAGE_ASPECT_DEPTH_BIT, ImageState::DepthComputeRead,
                                                             ImageState::DepthTarget, ImageContents::Discard);

        assert(nextFrame.srcStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        assert(nextFrame.srcAccessMask == VK_ACCESS_2_NONE);
        assert(nextFrame.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
        assert(!needsBarrier(ImageState::DepthComputeRead, ImageState::DepthComputeRead));
    }

    // Mipmaps are made by copying each level from the one above: a barrier per level, so its range is that level alone,
    // in every layer (a cube's six faces change state together).
    void testBarrierCoversChosenLevels() {
        const VkImageMemoryBarrier2 barrier = imageBarrier(someImage(), VK_IMAGE_ASPECT_COLOR_BIT, ImageState::TransferDestination,
                                                           ImageState::TransferSource, ImageContents::Keep, 3, 1);

        assert(barrier.subresourceRange.baseMipLevel == 3);
        assert(barrier.subresourceRange.levelCount == 1);
        assert(barrier.subresourceRange.baseArrayLayer == 0);
        assert(barrier.subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS);
        assert(barrier.srcStageMask == VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
        assert(barrier.srcAccessMask == VK_ACCESS_2_TRANSFER_WRITE_BIT);
        assert(barrier.dstAccessMask == VK_ACCESS_2_TRANSFER_READ_BIT);
        assert(barrier.oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        assert(barrier.newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }

    // A fresh image has nothing worth keeping and nothing to wait for.
    void testUndefinedStartsFromNothing() {
        const VkImageMemoryBarrier2 barrier = imageBarrier(someImage(), VK_IMAGE_ASPECT_COLOR_BIT, ImageState::Undefined,
                                                           ImageState::TransferDestination, ImageContents::Keep);

        assert(barrier.srcStageMask == VK_PIPELINE_STAGE_2_NONE);
        assert(barrier.srcAccessMask == VK_ACCESS_2_NONE);
        assert(barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
        assert(barrier.dstStageMask == VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
        assert(barrier.newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }

    /*
    * The light probes' atlases are written by compute shaders as storage images (read and written in place, in the
    * GENERAL layout), then sampled: by the probe balls' fragment shader, and next frame by the probe rays in compute.
    * Storage writes after storage writes need a barrier like any write; reading after reading in compute does not.
    */
    void testProbeAtlasesAreWrittenInComputeAndSampled() {
        const VkImageMemoryBarrier2 toWrite = imageBarrier(someImage(), VK_IMAGE_ASPECT_COLOR_BIT, ImageState::FragmentRead,
                                                           ImageState::ComputeStorage, ImageContents::Keep);
        assert(toWrite.srcStageMask == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
        assert(toWrite.srcAccessMask == VK_ACCESS_2_NONE);
        assert(toWrite.dstStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        assert(toWrite.dstAccessMask == (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT));
        assert(toWrite.newLayout == VK_IMAGE_LAYOUT_GENERAL);

        const VkImageMemoryBarrier2 toSample = imageBarrier(someImage(), VK_IMAGE_ASPECT_COLOR_BIT, ImageState::ComputeStorage,
                                                            ImageState::ComputeRead, ImageContents::Keep);
        assert(toSample.srcStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        assert(toSample.srcAccessMask == VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        assert(toSample.dstStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        assert(toSample.dstAccessMask == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        assert(toSample.oldLayout == VK_IMAGE_LAYOUT_GENERAL);
        assert(toSample.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        assert(needsBarrier(ImageState::ComputeStorage, ImageState::ComputeStorage));
        assert(!needsBarrier(ImageState::ComputeRead, ImageState::ComputeRead));
        assert(needsBarrier(ImageState::ComputeStorage, ImageState::FragmentRead));
    }

    // The scene's textures are uploaded once and then sampled by the main pass and by the probes' rays, in compute: the
    // upload's last barrier must make them visible to both.
    void testSurfaceTexturesAreReadByFragmentAndComputeShaders() {
        const VkImageMemoryBarrier2 barrier = imageBarrier(someImage(), VK_IMAGE_ASPECT_COLOR_BIT, ImageState::TransferDestination,
                                                           ImageState::SurfaceRead, ImageContents::Keep);
        assert(barrier.srcAccessMask == VK_ACCESS_2_TRANSFER_WRITE_BIT);
        assert(barrier.dstStageMask == (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT));
        assert(barrier.dstAccessMask == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        assert(barrier.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        assert(!needsBarrier(ImageState::SurfaceRead, ImageState::SurfaceRead));
    }
}

void testImageState() {
    testDrawnImageBecomesReadable();
    testOnlyReadAfterReadNeedsNoBarrier();
    testOverwritingWaitsForReadsWithoutFlushing();
    testAcquiredImageChainsWithTheSemaphore();
    testPresentTakesThePresentLayout();
    testShadowAtlasRoundTrip();
    testSceneDepthIsMeasuredInCompute();
    testBarrierCoversChosenLevels();
    testUndefinedStartsFromNothing();
    testProbeAtlasesAreWrittenInComputeAndSampled();
    testSurfaceTexturesAreReadByFragmentAndComputeShaders();
}
