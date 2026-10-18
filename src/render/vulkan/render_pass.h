#pragma once

#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/vulkan_pipeline.h"

#include "volk/volk.h"

#include <array>
#include <span>

// The image a pass draws colour into, and what happens to its old contents first: kept, cleared, or not cared about.
struct ColourAttachment {
    VkImageView view = VK_NULL_HANDLE;
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    std::array<float, 4> clear{0.0f, 0.0f, 0.0f, 0.0f};
};

// The depth image a pass tests against, whether its old contents are kept, and whether the result is kept afterwards.
struct DepthAttachment {
    VkImageView view = VK_NULL_HANDLE;
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkAttachmentStoreOp store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    float clear = 0.0f;
};

/*
* Start drawing into these images (dynamic rendering: the images are named here, no render pass object needed), over
* the whole extent, with the viewport and scissor covering it as SDL_GPU set them. Colour is always stored. The images
* must already be in their target states.
*/
void beginRendering(VkCommandBuffer commands, VkExtent2D extent, const ColourAttachment* colour, const DepthAttachment* depth);

// A texture as a draw reads it: its view, how it is filtered, and the layout it is in.
struct SampledImage {
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};

// Write a fresh set 2 for one draw (textures at bindings 0.., then storage buffers) and bind it to the pipeline.
void bindFragmentResources(VkCommandBuffer commands, FrameDescriptors& descriptors, const VulkanPipeline& pipeline,
                           std::span<const SampledImage> images, std::span<const VkDescriptorBufferInfo> storageBuffers = {});

// The full-screen triangle (fullscreen_triangle.vert makes its corners; there is no vertex buffer).
void drawFullScreenTriangle(VkCommandBuffer commands);
