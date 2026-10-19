#include "render/vulkan/image_state.h"

namespace {
    // Depth is tested before shading where possible (early) and after it otherwise (late); either may read or write.
    constexpr VkPipelineStageFlags2 depthTestStages =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
}

/*
* Colour and depth targets list reads as well as writes: loading old contents and blending read the image, and the
* depth test reads the stored depth. ALL_TRANSFER covers copies and blits alike.
*/
ImageStateUse imageStateUse(ImageState state) {
    switch (state) {
    case ImageState::Undefined:
        return {};
    case ImageState::Acquired:
        return {acquireWaitStage, VK_ACCESS_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED};
    case ImageState::ColourTarget:
        return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    case ImageState::DepthTarget:
        return {depthTestStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    case ImageState::FragmentRead:
        return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_ACCESS_2_NONE,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    case ImageState::DepthFragmentRead:
        return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_ACCESS_2_NONE,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    case ImageState::DepthComputeRead:
        return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_ACCESS_2_NONE,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    case ImageState::TransferSource:
        return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_ACCESS_2_NONE,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    case ImageState::TransferDestination:
        return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_NONE, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
    case ImageState::Present:
        // The presentation engine is outside the pipeline: the semaphore given to vkQueuePresentKHR orders it.
        return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    }

    return {};
}

bool needsBarrier(ImageState from, ImageState to) {
    const bool readOnly = imageStateUse(from).writes == VK_ACCESS_2_NONE && from != ImageState::Undefined &&
                          from != ImageState::Acquired && from != ImageState::Present;
    return !(from == to && readOnly);
}

VkImageMemoryBarrier2 imageBarrier(VkImage image, VkImageAspectFlags aspect, ImageState from, ImageState to, ImageContents contents,
                                   std::uint32_t firstLevel, std::uint32_t levelCount) {
    const ImageStateUse before = imageStateUse(from);
    const ImageStateUse after = imageStateUse(to);

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = before.stages;
    barrier.srcAccessMask = before.writes;
    barrier.dstStageMask = after.stages;
    barrier.dstAccessMask = after.reads | after.writes;
    barrier.oldLayout = contents == ImageContents::Discard ? VK_IMAGE_LAYOUT_UNDEFINED : before.layout;
    barrier.newLayout = after.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = firstLevel;
    barrier.subresourceRange.levelCount = levelCount;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    return barrier;
}
